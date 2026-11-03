/*
 * SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB Display Device
 *
 * Receives frame data from PC via USB Bulk endpoints and provides
 * frames to the application through a poll interface.
 *
 * PC side requires: xfz1986 USB Graphic IDD driver
 *   https://github.com/chuanjinpang/win10_idd_xfz1986_usb_graphic_driver_display
 */

#include "usbd_core.h"
#include "usbd_display.h"
#include "usbd_hid.h"
#include <rtthread.h>
#include "mem_section.h"
#include "usb_display_config.h"
#include "usb_touch.h"

/* ================ Endpoint Configuration ================ */

#define DISPLAY_IN_EP   0x81
#define DISPLAY_OUT_EP  0x02

/* HID 触摸屏：中断 IN 端点（报告 = 1 字节 ID + 5 字节数据） */
#define TOUCH_IN_EP        0x83  /* EP3 IN：MUSB 端点号决定收发方向（TXCSRH MODE 位），
                                    不能与显示 EP2 OUT(0x02) 共用 EP 号，否则后注册的会把
                                    显示接收端点切成发送方向，导致 PC 推流永远 NAK */
#define TOUCH_EP_MPS       8
#define TOUCH_EP_INTERVAL  1    /* 1ms，GT911 约 100Hz 上报足够 */

/* ================ USB Descriptors ================ */

#define USBD_VID           0x303A
#define USBD_PID           0x2986
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

/*
 * 配置描述符总长：
 *   9 (config) + 9 (display 接口) + 7 + 7 (两个 Bulk 端点)
 * + 9 (HID 接口) + 9 (HID 描述符) + 7 (中断 IN 端点) = 57
 */
#define USB_CONFIG_SIZE (9 + 9 + 7 + 7 + 9 + 9 + 7)

#ifdef CONFIG_USB_HS
#define DISPLAY_EP_MPS 512
#else
#define DISPLAY_EP_MPS 64
#endif

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01),
};

/*
 * HID 报告描述符：单点触摸屏（Digitizer / Touch Screen）
 *
 * Windows 把 HID 设备识别为"触摸屏"的必要字段：
 *   - 顶层 Touch Screen (0x04) 集合
 *   - Finger 集合内：Tip Switch + Contact ID + X/Y（绝对坐标）
 *   - 顶层 Contact Count（每次报告的有效触点数）
 *
 * 报告（含 Report ID 共 8 字节）：
 *   [0]   Report ID = 1
 *   [1]   bit0 = Tip Switch, bit1 = In Range, 其余填充
 *   [2]   Contact ID（每次新按下自增，Windows 用来跟踪触点）
 *   [3..4] X 逻辑坐标（0..32767, 小端）
 *   [5..6] Y 逻辑坐标（0..32767, 小端）
 *   [7]   Contact Count（tip ? 1 : 0）
 */
static const uint8_t touch_report_desc[] = {
    0x05, 0x0D,             /* USAGE_PAGE (Digitizer) */
    0x09, 0x04,             /* USAGE (Touch Screen) */
    0xA1, 0x01,             /* COLLECTION (Application) */
    0x85, 0x01,             /*   REPORT_ID (1) */
    0x09, 0x22,             /*   USAGE (Finger) */
    0xA1, 0x00,             /*   COLLECTION (Physical) */
    0x09, 0x42,             /*     USAGE (Tip Switch) */
    0x15, 0x00,             /*     LOGICAL_MINIMUM (0) */
    0x25, 0x01,             /*     LOGICAL_MAXIMUM (1) */
    0x75, 0x01,             /*     REPORT_SIZE (1) */
    0x95, 0x01,             /*     REPORT_COUNT (1) */
    0x81, 0x02,             /*     INPUT (Data,Var,Abs) */
    0x09, 0x32,             /*     USAGE (In Range) */
    0x81, 0x02,             /*     INPUT (Data,Var,Abs) */
    0x95, 0x06,             /*     REPORT_COUNT (6) */
    0x81, 0x03,             /*     INPUT (Cnst,Var,Abs) —— 补齐第 2 字节 */
    0x09, 0x51,             /*     USAGE (Contact ID) */
    0x75, 0x08,             /*     REPORT_SIZE (8) */
    0x95, 0x01,             /*     REPORT_COUNT (1) */
    0x15, 0x00,             /*     LOGICAL_MINIMUM (0) */
    0x25, 0x7F,             /*     LOGICAL_MAXIMUM (127) */
    0x81, 0x02,             /*     INPUT (Data,Var,Abs) */
    /* X/Y 必须挂在 Generic Desktop 页（0x01）下，微软触摸屏规范强制要求；
       若停留在 Digitizer 页（0x0D），0x30/0x31 会被解析成 Tip/Barrel Pressure，
       Windows 将不认此集合为功能触摸屏（SM_MAXIMUMTOUCHES=0） */
    0x05, 0x01,             /*     USAGE_PAGE (Generic Desktop) */
    0x09, 0x30,             /*     USAGE (X) */
    0x09, 0x31,             /*     USAGE (Y) */
    0x15, 0x00,             /*     LOGICAL_MINIMUM (0) */
    0x26, 0xFF, 0x7F,       /*     LOGICAL_MAXIMUM (32767) */
    0x75, 0x10,             /*     REPORT_SIZE (16) */
    0x95, 0x02,             /*     REPORT_COUNT (2) */
    0x81, 0x02,             /*     INPUT (Data,Var,Abs) */
    0x05, 0x0D,             /*     USAGE_PAGE (Digitizers) —— 恢复页上下文 */
    0xC0,                   /*   END_COLLECTION (Finger) */
    0x09, 0x54,             /*   USAGE (Contact Count) */
    0x15, 0x00,             /*   LOGICAL_MINIMUM (0) */
    0x25, 0x01,             /*   LOGICAL_MAXIMUM (1) */
    0x75, 0x08,             /*   REPORT_SIZE (8) */
    0x95, 0x01,             /*   REPORT_COUNT (1) */
    0x81, 0x02,             /*   INPUT (Data,Var,Abs) */
    0xC0,                   /* END_COLLECTION (Application) */
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),

    /* ---------- 接口 0：vendor 自定义显示（Class 0xFF） ---------- */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_IN_EP, 0x02, DISPLAY_EP_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_OUT_EP, 0x02, DISPLAY_EP_MPS, 0x00),

    /* ---------- 接口 1：HID 触摸屏（Class 0x03） ---------- */
    USB_INTERFACE_DESCRIPTOR_INIT(0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00),
    /* HID 描述符：bcdHID=1.11, country=0, 1 个报表描述符 */
    0x09, HID_DESCRIPTOR_TYPE_HID,
    0x11, 0x01, 0x00,
    0x01, HID_DESCRIPTOR_TYPE_HID_REPORT,
    WBVAL(sizeof(touch_report_desc)),
    USB_ENDPOINT_DESCRIPTOR_INIT(TOUCH_IN_EP, 0x03, TOUCH_EP_MPS, TOUCH_EP_INTERVAL),
};

static const uint8_t device_quality_descriptor[] = {
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x00,
    0x00,
};
/*
 * Product string encodes display parameters for the PC driver:
 *   R<width>x<height>_E<encoding>
 *
 * 支持的分辨率必须在 PC 驱动内置模式表里（见 main.c 顶部注释）。
 * 编码只能是 rgb16（DLL 里只有 rgb565 编码器）。
 */
#define _STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) _STRINGIFY_IMPL(x)

#define DISPLAY_PRODUCT_STRING \
    "sifli_R" STRINGIFY(DISPLAY_WIDTH) "x" STRINGIFY(DISPLAY_HEIGHT) "_Ergb16"

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },            /* Langid */
    "SiFli",                                  /* Manufacturer */
    DISPLAY_PRODUCT_STRING,                   /* Product：随分辨率自动生成 */
    "2025010101",                             /* Serial Number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    if (index >= (sizeof(string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return string_descriptors[index];
}

const struct usb_descriptor usbdisplay_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/* ================ USB Event Handler ================ */

static volatile bool usb_display_configured = false;

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
        case USBD_EVENT_RESET:
            usb_display_configured = false;
            break;
        case USBD_EVENT_CONNECTED:
            rt_kprintf("USB display connected\n");
            break;
        case USBD_EVENT_DISCONNECTED:
            rt_kprintf("USB display disconnected\n");
            usb_display_configured = false;
            break;
        case USBD_EVENT_RESUME:
            break;
        case USBD_EVENT_SUSPEND:
            break;
        case USBD_EVENT_CONFIGURED:
            rt_kprintf("USB display configured - device ready!\n");
            usb_display_configured = true;
            break;
        default:
            break;
    }
}

/* ================ Frame Buffers ================ */

/*
 * Double-buffered frame pool.
 * Frame buffer size = width * height * bytes_per_pixel.
 * For RGB565 (2 bytes/pixel): 390*450*2 = 351000 bytes.
 * Must be a multiple of 16384 (USB bulk transfer alignment).
 *
 * Frame buffers are allocated from PSRAM heap to save SRAM.
 * Adjust resolution and format to match your LCD and product string above.
 */

/*
 * 类驱动按 16384 字节分包读取，帧尺寸必须是 16384 的整数倍；
 * 且缓冲要容纳 16 字节帧头 + payload（800x480x2 + 16 = 768016 → 47*16384 = 770048）
 */
#define FRAME_BUF_SIZE  (((DISPLAY_WIDTH * DISPLAY_HEIGHT * DISPLAY_BPP + 16 + 16383) / 16384) * 16384)

/* PSRAM buffer for frame buffer allocation (manual pool) */
static uint8_t usb_display_psram_pool[FRAME_BUF_SIZE * 2 + 4096] L2_RET_BSS_SECT(usb_display_psram);
static uint32_t usb_display_psram_offset = 0;

static struct usbd_display_frame frame_pool[2];
static struct usbd_interface intf0;

/* ================ HID Touch ================ */

static struct usbd_interface intf1;

/* 报告缓冲：8 字节，布局见 touch_report_desc 注释 */
static uint8_t touch_report_buf[8] __attribute__((aligned(32)));
static volatile bool touch_ep_busy = false;
static uint8_t touch_contact_id = 0;   /* 每次新按下自增，Windows 跟踪触点用 */
static bool touch_prev_tip = false;

/* HID 触摸发送统计（touch_test 查看） */
static volatile uint32_t hid_tx_ok_cnt     = 0;  /* usbd_ep_start_write 成功次数 */
static volatile uint32_t hid_tx_err_cnt    = 0;  /* 端点写返回错误次数 */
static volatile uint32_t hid_rej_unconf_cnt= 0;  /* USB 未配置被拒绝 */
static volatile uint32_t hid_rej_busy_cnt  = 0;  /* 上一包未完成被拒绝 */
static bool hid_first_ok_logged = false;

static void usbd_touch_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    touch_ep_busy = false;
}

static struct usbd_endpoint touch_in_ep = {
    .ep_cb = usbd_touch_in_callback,
    .ep_addr = TOUCH_IN_EP,
};

bool usb_touch_hid_report(uint16_t x, uint16_t y, bool tip)
{
    int ret;

    if (!usb_display_configured) {
        hid_rej_unconf_cnt++;
        return false;
    }
    if (touch_ep_busy) {
        hid_rej_busy_cnt++;
        return false;
    }

    /* 新的一次按下：分配新的 Contact ID（单点触摸 0..127 循环足够） */
    if (tip && !touch_prev_tip) {
        touch_contact_id = (uint8_t)((touch_contact_id + 1) & 0x7F);
    }
    touch_prev_tip = tip;

    touch_report_buf[0] = 0x01;                     /* Report ID */
    touch_report_buf[1] = tip ? 0x03 : 0x00;        /* bit0 Tip Switch, bit1 In Range */
    touch_report_buf[2] = touch_contact_id;         /* Contact ID */
    touch_report_buf[3] = (uint8_t)(x & 0xFF);      /* X LSB */
    touch_report_buf[4] = (uint8_t)(x >> 8);        /* X MSB */
    touch_report_buf[5] = (uint8_t)(y & 0xFF);      /* Y LSB */
    touch_report_buf[6] = (uint8_t)(y >> 8);        /* Y MSB */
    touch_report_buf[7] = tip ? 0x01 : 0x00;        /* Contact Count */

    touch_ep_busy = true;
    ret = usbd_ep_start_write(0, TOUCH_IN_EP, touch_report_buf, sizeof(touch_report_buf));
    if (ret < 0) {
        touch_ep_busy = false;
        hid_tx_err_cnt++;
        rt_kprintf("[HID] ep_start_write err=%d ep=0x%02x\n", ret, TOUCH_IN_EP);
        return false;
    }
    hid_tx_ok_cnt++;
    if (!hid_first_ok_logged) {
        hid_first_ok_logged = true;
        rt_kprintf("[HID] first report sent: x=%u y=%u tip=%d (ep 0x%02x OK)\n",
                   x, y, tip, TOUCH_IN_EP);
    }
    return true;
}

void usb_touch_stats_get(uint32_t *ok, uint32_t *err, uint32_t *unconf, uint32_t *busy)
{
    *ok     = hid_tx_ok_cnt;
    *err    = hid_tx_err_cnt;
    *unconf = hid_rej_unconf_cnt;
    *busy   = hid_rej_busy_cnt;
}

/* ================ Public API ================ */

bool usb_display_is_ready(void)
{
    return usb_display_configured;
}

void usb_display_init(uint8_t busid, uintptr_t reg_base)
{
    rt_kprintf("USB display: init (%dx%d, %d bpp, %d bytes/frame)\n",
               DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_BPP * 8, FRAME_BUF_SIZE);

    /* Allocate frame buffers from PSRAM pool (double buffered) */
    for (int i = 0; i < 2; i++) {
        uint8_t *fb = (uint8_t *)&usb_display_psram_pool[usb_display_psram_offset];
        usb_display_psram_offset += FRAME_BUF_SIZE;
        if (usb_display_psram_offset > sizeof(usb_display_psram_pool)) {
            rt_kprintf("ERROR: PSRAM pool exhausted!\n");
            return;
        }

        frame_pool[i].frame_buf = fb;
        frame_pool[i].frame_bufsize = FRAME_BUF_SIZE;
    }

    /* Register USB descriptors */
    usbd_desc_register(busid, &usbdisplay_descriptor);

    /* Add display interface (接口 0) */
    usbd_add_interface(busid,
        usbd_display_init_intf(&intf0, DISPLAY_OUT_EP, DISPLAY_IN_EP, frame_pool, 2));

    /* Add HID touch interface (接口 1)，必须在 display 之后以匹配描述符顺序 */
    usbd_add_interface(busid,
        usbd_hid_init_intf(busid, &intf1, touch_report_desc, sizeof(touch_report_desc)));
    usbd_add_endpoint(busid, &touch_in_ep);

    /* Initialize USB device */
    usbd_initialize(busid, reg_base, usbd_event_handler);

    rt_kprintf("USB display: HID touch interface added (ep 0x%02x, %u bytes report)\n",
               TOUCH_IN_EP, (unsigned)sizeof(touch_report_desc));

    rt_kprintf("USB display: initialized, waiting for enumeration...\n");
}

/*
 * Poll for received frames. Call this in the main loop.
 * Returns true if a new frame was received, false otherwise.
 *
 * When a frame is received:
 *   frame->frame_format = pixel format (USBD_DISPLAY_TYPE_RGB565 etc.)
 *   frame->frame_size   = payload size in bytes
 *   frame->frame_buf    = frame data pointer
 *
 * After processing, call usb_display_release_frame() to recycle the buffer.
 */
bool usb_display_poll_frame(struct usbd_display_frame **frame)
{
    int ret = usbd_display_dequeue(frame, 0);
    if (ret < 0) {
        return false;
    }
    return true;
}

void usb_display_release_frame(struct usbd_display_frame *frame)
{
    usbd_display_enqueue(frame);
}
