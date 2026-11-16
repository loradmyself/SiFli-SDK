/*
 * SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 触摸采集 → USB HID 上报
 *
 * 数据流：
 *   GT911 (i2c4) → /dev/touch 驱动 → 本线程 → usb_touch_hid_report()
 *   → HID 中断端点 0x83 (EP3 IN) → Windows "HID-compliant touch screen"
 *   注意：MUSB 端点号决定收发方向，EP 号不能与显示 BulkOut(0x02, EP2) 冲突
 *
 * PC 侧不需要改显示驱动；HID 接口由 Windows 自带 hidusb 免驱加载。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>
#include <stdbool.h>

#include "usb_display_config.h"
#include "usb_touch.h"

#define DBG_TAG "usb.touch"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef BSP_USING_TOUCHD

#include "drv_touch.h"

#define TOUCH_DEV_NAME  "touch"

static rt_device_t      tp_dev  = RT_NULL;
static struct rt_semaphore tp_sem;
static rt_thread_t      tp_tid  = RT_NULL;
static volatile bool    tp_debug = false;

/* 统计（touch_test 查看） */
static volatile uint32_t st_ev_down = 0;
static volatile uint32_t st_ev_move = 0;
static volatile uint32_t st_ev_up   = 0;
static volatile uint32_t st_ev_skip = 0;
static volatile uint32_t st_dedup   = 0;
static volatile uint32_t st_send_fail = 0;

/* HID Digitizer 逻辑坐标范围 */
#define HID_ABS_MAX  32767u

static rt_err_t tp_rx_indicate(rt_device_t dev, rt_size_t size)
{
    rt_sem_release(&tp_sem);
    return RT_EOK;
}

/* 原始坐标 → 面板坐标（含镜像/旋转修正） */
static void tp_map_raw(uint16_t raw_x, uint16_t raw_y, uint16_t *px, uint16_t *py)
{
#if TOUCH_SWAP_XY
    uint16_t tmp = raw_x;
    raw_x = raw_y;
    raw_y = tmp;
#endif

    /* 钳位到面板范围，避免越界坐标导致光标飞出屏幕 */
    if (raw_x >= TOUCH_PANEL_WIDTH)  raw_x = TOUCH_PANEL_WIDTH  - 1;
    if (raw_y >= TOUCH_PANEL_HEIGHT) raw_y = TOUCH_PANEL_HEIGHT - 1;

#if TOUCH_MIRROR_X
    raw_x = TOUCH_PANEL_WIDTH - 1 - raw_x;
#endif
#if TOUCH_MIRROR_Y
    raw_y = TOUCH_PANEL_HEIGHT - 1 - raw_y;
#endif

    *px = raw_x;
    *py = raw_y;
}

/* 面板坐标 → HID 逻辑坐标（0..32767） */
static inline uint16_t tp_to_hid(uint16_t v, uint16_t panel_size)
{
    if (panel_size <= 1) {
        return 0;
    }
    return (uint16_t)(((uint32_t)v * HID_ABS_MAX) / (uint32_t)(panel_size - 1));
}

static void tp_send(uint16_t x, uint16_t y, bool tip)
{
    uint16_t hx = tp_to_hid(x, TOUCH_PANEL_WIDTH);
    uint16_t hy = tp_to_hid(y, TOUCH_PANEL_HEIGHT);

    /* 端点忙时重试几毫秒：触摸事件频率低（~100Hz），不会堆积 */
    for (int i = 0; i < 5; i++) {
        if (usb_touch_hid_report(hx, hy, tip)) {
            return;
        }
        rt_thread_mdelay(1);
    }
    /* 5 次重试都失败：端点卡死或未配置 */
    st_send_fail++;
}

static void touch_thread_entry(void *parameter)
{
    struct touch_message msg;
    uint16_t last_x = 0, last_y = 0;
    bool last_tip = false;
    bool have_last = false;
    uint32_t rx_cnt = 0;

    LOG_I("thread started, panel %ux%u -> HID 0..32767",
          TOUCH_PANEL_WIDTH, TOUCH_PANEL_HEIGHT);

    while (1) {
        rt_sem_take(&tp_sem, RT_WAITING_FOREVER);

        while (rt_device_read(tp_dev, 0, &msg, 1) == 1) {
            bool tip;
            uint16_t x, y;

            if (msg.event == TOUCH_EVENT_DOWN || msg.event == TOUCH_EVENT_MOVE) {
                tip = true;
                if (msg.event == TOUCH_EVENT_DOWN) st_ev_down++; else st_ev_move++;
            } else if (msg.event == TOUCH_EVENT_UP) {
                tip = false;
                st_ev_up++;
            } else {
                st_ev_skip++;
                if (tp_debug) {
                    rt_kprintf("[TP] SKIP unknown ev=0x%02x\n", msg.event);
                }
                continue;
            }

            tp_map_raw(msg.x, msg.y, &x, &y);
            rx_cnt++;

            if (tp_debug) {
                rt_kprintf("[TP] ev=0x%02x raw=(%u,%u) mapped=(%u,%u) cnt=%u\n",
                           msg.event, msg.x, msg.y, x, y, rx_cnt);
            }

            /* 去重：位置与按下状态都没变就不发（省 USB 带宽） */
            if (have_last && tip == last_tip && x == last_x && y == last_y) {
                st_dedup++;
                continue;
            }

            tp_send(x, y, tip);
            last_x = x;
            last_y = y;
            last_tip = tip;
            have_last = true;
        }
    }
}

void usb_touch_start(void)
{
    if (tp_tid != RT_NULL) {
        return;
    }

    tp_dev = rt_device_find(TOUCH_DEV_NAME);
    if (tp_dev == RT_NULL) {
        LOG_W("touch device \"%s\" not found, touch disabled", TOUCH_DEV_NAME);
        return;
    }

    if (rt_device_open(tp_dev, RT_DEVICE_FLAG_RDONLY) != RT_EOK) {
        LOG_E("open touch device failed");
        return;
    }

    rt_sem_init(&tp_sem, "tpsem", 0, RT_IPC_FLAG_FIFO);
    rt_device_set_rx_indicate(tp_dev, tp_rx_indicate);

    tp_tid = rt_thread_create("usb_tp", touch_thread_entry, RT_NULL,
                              2048, RT_THREAD_PRIORITY_MAX / 2, 10);
    if (tp_tid == RT_NULL) {
        LOG_E("create touch thread failed");
        rt_device_close(tp_dev);
        return;
    }
    rt_thread_startup(tp_tid);

    LOG_I("touch ready: %s", TOUCH_DEV_NAME);
}

/* ================ 调试命令 ================ */

static void touch_test(int argc, char **argv)
{
    uint32_t ok, err, unconf, busy;

    tp_debug = !tp_debug;
    usb_touch_stats_get(&ok, &err, &unconf, &busy);
    rt_kprintf("touch debug %s\n", tp_debug ? "ON (touch the panel)" : "OFF");
    rt_kprintf("[TP stat] ev down=%u move=%u up=%u skip=%u dedup=%u sendfail=%u\n",
               st_ev_down, st_ev_move, st_ev_up, st_ev_skip, st_dedup, st_send_fail);
    rt_kprintf("[HID stat] tx_ok=%u tx_err=%u rej_unconf=%u rej_busy=%u\n",
               ok, err, unconf, busy);
    if (ok == 0) {
        rt_kprintf("[HID] no report ever sent! check: USB configured? events arriving?\n");
    } else if (err > 0) {
        rt_kprintf("[HID] EP3 write errors occurred - check endpoint/FIFO\n");
    }
}
MSH_CMD_EXPORT(touch_test, toggle touch event debug print);

#else /* !BSP_USING_TOUCHD */

void usb_touch_start(void)
{
    rt_kprintf("usb touch: BSP_USING_TOUCHD not enabled, skipped\n");
}

#endif /* BSP_USING_TOUCHD */
