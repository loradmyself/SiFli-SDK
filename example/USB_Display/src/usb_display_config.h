/*
 * SPDX-FileCopyrightText: 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB Display 统一配置（唯一事实来源）
 *
 * 换屏时只需要改这里的 DISPLAY_WIDTH / DISPLAY_HEIGHT：
 *   - usb_display_device.c 用它生成产品字符串、计算帧缓冲大小
 *   - main.c 用它生成持久帧缓冲、绘制区域
 *   - usb_touch.c 用它做触摸坐标映射
 */

#ifndef __USB_DISPLAY_CONFIG_H__
#define __USB_DISPLAY_CONFIG_H__

/* ---------------- 分辨率 / 像素格式 ---------------- */
#define DISPLAY_WIDTH   1024
#define DISPLAY_HEIGHT  600
#define DISPLAY_BPP     2       /* 2 = RGB565, 3 = RGB888 */

/* ---------------- 触摸 ---------------- */

/* 触摸面板的坐标范围。GT911 等驱动上报的就是面板像素坐标，
 * 通常与 DISPLAY_WIDTH/HEIGHT 一致；若触摸 IC 的原生分辨率不同，改这里。 */
#define TOUCH_PANEL_WIDTH   DISPLAY_WIDTH
#define TOUCH_PANEL_HEIGHT  DISPLAY_HEIGHT

/* 触摸方向修正：若手指位置与画面不一致（镜像 / 旋转 90°），改这三项 */
#define TOUCH_SWAP_XY       0   /* 1 = 交换 X/Y（屏幕旋转 90/270 度时用） */
#define TOUCH_MIRROR_X      0   /* 1 = X 轴镜像 */
#define TOUCH_MIRROR_Y      0   /* 1 = Y 轴镜像 */

#endif /* __USB_DISPLAY_CONFIG_H__ */
