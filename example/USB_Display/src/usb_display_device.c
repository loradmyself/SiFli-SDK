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
#include <rtthread.h>
#include "mem_section.h"

/* ================ Endpoint Configuration ================ */

#define DISPLAY_IN_EP   0x81
#define DISPLAY_OUT_EP  0x02

/* ================ USB Descriptors ================ */

#define USBD_VID           0x303A
#define USBD_PID           0x2986
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)

#ifdef CONFIG_USB_HS
#define DISPLAY_EP_MPS 512
#else
#define DISPLAY_EP_MPS 64
#endif

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_IN_EP, 0x02, DISPLAY_EP_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_OUT_EP, 0x02, DISPLAY_EP_MPS, 0x00),
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
 *   R<width>x<height>_E<encoding>_Fps<fps>_Bl<block_size>
 *
 * Supported encodings:
 *   rgb16  - RGB565 (16-bit)
 *   rgb24  - RGB888 (24-bit)
 *   yuv420 - YUV 4:2:0
 *   jpg    - JPEG compressed (recommended for bandwidth)
 */
static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },            /* Langid */
    "SiFli",                                  /* Manufacturer */
    "sifli_R390x450_Ergb16_Fps15_Bl128",     /* Product */
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
#define DISPLAY_WIDTH   390
#define DISPLAY_HEIGHT  450
#define DISPLAY_BPP     2   /* 2 for RGB565, 3 for RGB888 */
#define FRAME_BUF_SIZE  (DISPLAY_WIDTH * DISPLAY_HEIGHT * DISPLAY_BPP)

/* PSRAM buffer for frame buffer allocation (manual pool) */
static uint8_t usb_display_psram_pool[FRAME_BUF_SIZE + 4096] L2_RET_BSS_SECT(usb_display_psram);
static uint32_t usb_display_psram_offset = 0;

static struct usbd_display_frame frame_pool[1];
static struct usbd_interface intf0;

/* ================ Public API ================ */

bool usb_display_is_ready(void)
{
    return usb_display_configured;
}

void usb_display_init(uint8_t busid, uintptr_t reg_base)
{
    rt_kprintf("USB display: init (%dx%d, %d bpp, %d bytes/frame)\n",
               DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_BPP * 8, FRAME_BUF_SIZE);

    /* Allocate frame buffer from PSRAM pool */
    uint8_t *fb0 = (uint8_t *)&usb_display_psram_pool[usb_display_psram_offset];
    usb_display_psram_offset += FRAME_BUF_SIZE;
    if (usb_display_psram_offset > sizeof(usb_display_psram_pool)) {
        rt_kprintf("ERROR: PSRAM pool exhausted!\n");
        return;
    }

    /* Setup frame pool */
    frame_pool[0].frame_buf = fb0;
    frame_pool[0].frame_bufsize = FRAME_BUF_SIZE;

    /* Register USB descriptors */
    usbd_desc_register(busid, &usbdisplay_descriptor);

    /* Add display interface */
    usbd_add_interface(busid,
        usbd_display_init_intf(&intf0, DISPLAY_OUT_EP, DISPLAY_IN_EP, frame_pool, 1));

    /* Initialize USB device */
    usbd_initialize(busid, reg_base, usbd_event_handler);

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
