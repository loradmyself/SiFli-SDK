/*
 * Copyright (c) 2026, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

#include <rtthread.h>

/* ================ USB common Configuration ================ */

#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)

#ifndef CONFIG_USB_DBG_LEVEL
    #define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

#define CONFIG_USB_PRINTF_COLOR_ENABLE

/* data align size when use dma or use dcache */
#ifdef CONFIG_USB_DCACHE_ENABLE
    #define CONFIG_USB_ALIGN_SIZE 32
#else
    #define CONFIG_USB_ALIGN_SIZE 4
#endif

/* attribute data into no cache ram */
#define USB_NOCACHE_RAM_SECTION __attribute__((section(".noncacheable")))

/* ================= USB Device Stack Configuration ================ */

#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
    #define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

#define CONFIG_USBDEV_ADVANCE_DESC

#ifndef CONFIG_USBDEV_EP0_PRIO
    #define CONFIG_USBDEV_EP0_PRIO 4
#endif

#ifndef CONFIG_USBDEV_EP0_STACKSIZE
    #define CONFIG_USBDEV_EP0_STACKSIZE 2048
#endif

/* ================ USB Device Port Configuration ================ */

#ifndef CONFIG_USBDEV_MAX_BUS
    #define CONFIG_USBDEV_MAX_BUS 1
#endif

#define CONFIG_USB_MUSB_EP_NUM 8

#define CONFIG_USB_MUSB_SIFLI

#ifndef usb_phyaddr2ramaddr
    #define usb_phyaddr2ramaddr(addr) (addr)
#endif

#ifndef usb_ramaddr2phyaddr
    #define usb_ramaddr2phyaddr(addr) (addr)
#endif

/* ================ USB HOST Stack Configuration ================== */
/* Required by MUSB port files (usb_dma_musb.c, usb_glue_sifli.c)
 * even when only device mode is used. */

#define CONFIG_USBHOST_MAX_RHPORTS          1
#define CONFIG_USBHOST_MAX_EXTHUBS          0
#define CONFIG_USBHOST_MAX_EHPORTS          4
#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_ENDPOINTS        4

#define CONFIG_USBHOST_MAX_CDC_ACM_CLASS 4
#define CONFIG_USBHOST_MAX_HID_CLASS     4
#define CONFIG_USBHOST_MAX_MSC_CLASS     2
#define CONFIG_USBHOST_MAX_AUDIO_CLASS   1
#define CONFIG_USBHOST_MAX_VIDEO_CLASS   1

#define CONFIG_USBHOST_DEV_NAMELEN 16

#ifndef CONFIG_USBHOST_PSC_PRIO
    #define CONFIG_USBHOST_PSC_PRIO 0
#endif
#ifndef CONFIG_USBHOST_PSC_STACKSIZE
    #define CONFIG_USBHOST_PSC_STACKSIZE 2048
#endif

#ifndef CONFIG_USBHOST_REQUEST_BUFFER_LEN
    #define CONFIG_USBHOST_REQUEST_BUFFER_LEN 512
#endif

#ifndef CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT
    #define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500
#endif

#ifndef CONFIG_USBHOST_MSC_TIMEOUT
    #define CONFIG_USBHOST_MSC_TIMEOUT 5000
#endif

#define CONFIG_USBHOST_BLUETOOTH_HCI_H4

/* ================ USB Host Port Configuration ================== */

#ifndef CONFIG_USBHOST_MAX_BUS
    #define CONFIG_USBHOST_MAX_BUS 1
#endif

#define CONFIG_USB_MUSB_PIPE_NUM 8

#endif /* CHERRYUSB_CONFIG_H */
