/*
 * SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB HID 触摸屏（Digitizer）对外接口
 */

#ifndef __USB_TOUCH_H__
#define __USB_TOUCH_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 发送一个 HID 触摸报告（逻辑坐标 0..32767）
 *
 * @param x   逻辑 X 坐标
 * @param y   逻辑 Y 坐标
 * @param tip true = 按下/接触, false = 抬起
 * @return true 已提交给 USB 栈, false 端点忙或设备未配置
 *
 * 必须在 usb_display_init() 之后调用；由 usb_display_device.c 实现。
 */
bool usb_touch_hid_report(uint16_t x, uint16_t y, bool tip);

/**
 * 启动触摸采集线程：读取 /dev/touch，映射坐标并通过 HID 上报。
 * 触摸驱动未开启（BSP_USING_TOUCHD 未定义）时为空实现。
 */
void usb_touch_start(void);

/**
 * 读取 HID 发送统计（touch_test 调试用），由 usb_display_device.c 实现。
 */
void usb_touch_stats_get(uint32_t *ok, uint32_t *err, uint32_t *unconf, uint32_t *busy);

#ifdef __cplusplus
}
#endif

#endif /* __USB_TOUCH_H__ */
