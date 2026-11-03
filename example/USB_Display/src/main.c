/*
 * SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB Display Example
 *
 * Receives frames from PC via USB and displays them on the ST7265 RGB LCD.
 * Uses direct LCD draw_rect API (no framebuffer path).
 */

/*
 * USB Display support display resolution （60Hz）
 *
 * 240x240  280x240  320x240  480x272  480x320  480x480  640x480  800x480
 * 800x600  960x960  1024x600 1024x768 1280x600 1280x720 1280x800 1600x1200 1920x1080
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "bf0_hal.h"
#include "mem_section.h"
#include "usbd_display.h"

/* Defined in usb_display_device.c */
extern void usb_display_init(uint8_t busid, uintptr_t reg_base);
extern bool usb_display_poll_frame(struct usbd_display_frame **frame);
extern void usb_display_release_frame(struct usbd_display_frame *frame);
extern bool usb_display_is_ready(void);

/* USB frame resolution (must match product string in usb_display_device.c) */
#define FRAME_WIDTH   1024    //width
#define FRAME_HEIGHT  600    //height
#define FB_BPP        2   /* RGB565 = 2 bytes/pixel */

/* LCD device */
static rt_device_t lcd_dev = NULL;

/* 与 usbd_display.c 中的 struct usbd_disp_frame_header 保持一致 */
struct usb_disp_frame_header {
    uint16_t crc16;
    uint8_t  type;      /* 0=RGB565 */
    uint8_t  cmd;
    uint16_t x, y, width, height;
    uint32_t frame_id     : 10;
    uint32_t payload_total: 22;
} __attribute__((packed));

/*
 * 持久全屏帧缓冲（PSRAM）。
 *
 * DPI 屏没有 GRAM，drv_lcd 的 draw_rect 语义是"整层缓冲替换"：
 * HAL_LCDC_LayerSetData 只是把指针交给 LCDC，画布其余区域显示背景色（黑）。
 * 因此脏矩形不能直接画，必须先合成进这块帧缓冲，再整屏推送给 LCDC。
 */
static uint8_t screen_fb[FRAME_WIDTH * FRAME_HEIGHT * FB_BPP] L2_RET_BSS_SECT(usb_screen_fb);

static void display_frame(struct usbd_display_frame *frame)
{
    struct usb_disp_frame_header *h = (struct usb_disp_frame_header *)frame->frame_buf;
    const uint8_t *src;
    uint32_t row_bytes;

    if (h->width == 0 || h->height == 0 ||
        (h->x + h->width) > FRAME_WIDTH || (h->y + h->height) > FRAME_HEIGHT) {
        return;  /* 非法帧，丢弃 */
    }

    /* 1. 把这一帧（全帧或脏矩形）按行合成进持久帧缓冲 */
    src = frame->frame_buf + sizeof(struct usb_disp_frame_header);
    row_bytes = (uint32_t)h->width * FB_BPP;
    for (uint32_t row = 0; row < h->height; row++) {
        uint32_t off = ((uint32_t)(h->y + row) * FRAME_WIDTH + h->x) * FB_BPP;
        memcpy(&screen_fb[off], src + row * row_bytes, row_bytes);
    }

    /* 2. 帧缓冲在 WB 缓存的 PSRAM 里，LCDC 用 DMA 直读，先把脏区清出 cache */
    {
        uint32_t s = ((uint32_t)h->y * FRAME_WIDTH + h->x) * FB_BPP;
        uint32_t e = s + (uint32_t)(h->height - 1) * FRAME_WIDTH * FB_BPP + row_bytes;

        s &= ~31u;              /* 按 32 字节 cache line 对齐 */
        e = (e + 31u) & ~31u;
        if (e > sizeof(screen_fb)) {
            e = sizeof(screen_fb);
        }
        SCB_CleanDCache_by_Addr((uint32_t *)&screen_fb[s], e - s);
    }

    /* 3. 整屏推送：把 LCDC 图层指向合成后的帧缓冲 */
    // rt_graphix_ops(lcd_dev)->draw_rect((const char *)screen_fb, 0, 0,
    //                                    FRAME_WIDTH - 1, FRAME_HEIGHT - 1);
}


int main(void)
{
    rt_kprintf("=== USB Display Example ===\n");
    rt_kprintf("LCD:RGB565\n");
    rt_kprintf("USB Frame: %dx%d RGB565\n", FRAME_WIDTH, FRAME_HEIGHT);

    /* Find and open LCD device */
    lcd_dev = rt_device_find("lcd");
    if (!lcd_dev) {
        rt_kprintf("ERROR: Cannot find lcd device!\n");
        return -1;
    }

    rt_kprintf("Opening LCD device...\n");
    rt_err_t err = rt_device_open(lcd_dev, RT_DEVICE_OFLAG_RDWR);
    if (err != RT_EOK && err != -RT_EBUSY) {
        rt_kprintf("ERROR: Cannot open lcd device, err=%d\n", err);
        return -1;
    }

    /* Set buffer format to RGB565 */
    uint16_t cf = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    rt_device_control(lcd_dev, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &cf);

    rt_kprintf("LCD opened successfully.\n");

    /* Initialize USB display device */
    usb_display_init(0, (uintptr_t)USBC_BASE);

    rt_kprintf("Ready. Waiting for USB frames...\n");
    rt_graphix_ops(lcd_dev)->draw_rect(screen_fb, 0, 0, FRAME_WIDTH - 1, FRAME_HEIGHT - 1);
    while (1) {
        struct usbd_display_frame *frame;
        static uint32_t rx_cnt = 0, last_cnt = 0;
        static rt_tick_t last_tick = 0;

        if (usb_display_poll_frame(&frame)) {
            rx_cnt++;
            /* 每秒打印一次USB收帧率（P1后draw_core不再跑，lcd_ctrl fps测不到） */
            if ((rt_tick_get() - last_tick) >= RT_TICK_PER_SECOND) {
                rt_kprintf("USB fps=%u, total=%u\n", rx_cnt - last_cnt, rx_cnt);
                last_cnt = rx_cnt;
                last_tick = rt_tick_get();
            }
            display_frame(frame);
            usb_display_release_frame(frame);
        } else {
            rt_thread_mdelay(1);
        }
    }

    return RT_EOK;
}
