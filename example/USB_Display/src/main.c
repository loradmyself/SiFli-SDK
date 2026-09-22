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

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "bf0_hal.h"
#include "usbd_display.h"

/* Defined in usb_display_device.c */
extern void usb_display_init(uint8_t busid, uintptr_t reg_base);
extern bool usb_display_poll_frame(struct usbd_display_frame **frame);
extern void usb_display_release_frame(struct usbd_display_frame *frame);
extern bool usb_display_is_ready(void);

/* USB frame resolution (must match product string in usb_display_device.c) */
#define FRAME_WIDTH   390
#define FRAME_HEIGHT  450
#define FB_BPP        2   /* RGB565 = 2 bytes/pixel */

/* LCD device */
static rt_device_t lcd_dev = NULL;

static void display_frame(struct usbd_display_frame *frame)
{
    /*
     * v1.0 protocol: frame_buf starts with a 20-byte header,
     * actual pixel data follows after the header.
     * v2.0 protocol: frame_buf is pure pixel data.
     */
    uint32_t expected_rgb565_size = FRAME_WIDTH * FRAME_HEIGHT * FB_BPP;
    uint8_t *pixel_data;
    uint32_t pixel_size;

    if (frame->frame_size > expected_rgb565_size) {
        /* v1.0 protocol: skip 20-byte frame header */
        pixel_data = frame->frame_buf + 20;
        pixel_size = frame->frame_size - 20;
    } else {
        /* v2.0 protocol: pixel data starts at beginning */
        pixel_data = frame->frame_buf;
        pixel_size = frame->frame_size;
    }

    if (pixel_size > expected_rgb565_size) {
        pixel_size = expected_rgb565_size;
    }

    /* Draw frame to LCD using direct draw_rect API */
    rt_graphix_ops(lcd_dev)->draw_rect(
        (const char *)pixel_data,
        0, 0,
        FRAME_WIDTH - 1,
        FRAME_HEIGHT - 1);
}

int main(void)
{
    rt_kprintf("=== USB Display Example ===\n");
    rt_kprintf("LCD: CO5300 390x450 QSPI\n");
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

    while (1) {
        struct usbd_display_frame *frame;

        if (usb_display_poll_frame(&frame)) {
            display_frame(frame);
            usb_display_release_frame(frame);
        } else {
            rt_thread_mdelay(1);
        }
    }

    return RT_EOK;
}
