/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RGB LED (WS2812B) test for SF32LB57 57GYD7N6 dev board
 * (board: sf32lb57-lcd_n16r16_dpi)
 *
 * Hardware: WS2812B data-in on PA11 (shared with TF_CLK), driven by
 * GPTIM2_CH4 PWM + DMA via the built-in "rgbled" device driver.
 *
 * Test items:
 *  1. Color display test : cycles through 8 basic colors (each 1s)
 *  2. Brightness test    : ramps brightness 0% -> 100% in 10 steps (red)
 *  3. Breathing test     : smooth breathing on white
 *
 * Interactive FinSH commands (serial console):
 *  rgb <r> <g> <b>   : set arbitrary color, e.g. "rgb 255 0 128"
 *  rgbtest           : re-run the full test sequence
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "drivers/rt_drv_pwm.h"

#define RGBLED_NAME    "rgbled"

static struct rt_device *rgbled_device = RT_NULL;

/* ------------------------------------------------------------------ */
/* Basic helpers                                                       */
/* ------------------------------------------------------------------ */

static void rgb_led_init(void)
{
#if defined(SF32LB57X)
    /* WS2812 DI is on PA11 on this board, remap to PWM channel */
    HAL_PIN_Set(PAD_PA11, GPTIM2_CH4, PIN_NOPULL, 0);   /* pwmt2_cc4 */
#endif

    rgbled_device = rt_device_find(RGBLED_NAME);
    if (!rgbled_device)
    {
        rt_kprintf("ERROR: rgbled device not found, "
                   "check RGB_SK6812MINI_HS_ENABLE config\n");
        return;
    }
    rt_kprintf("rgbled device found\n");
}

static void rgb_led_set_color(uint32_t color)
{
    struct rt_rgbled_configuration configuration;
    configuration.color_rgb = color;
    rt_device_control(rgbled_device, PWM_CMD_SET_COLOR, &configuration);
}

/* Apply brightness (0~100) by scaling each RGB channel */
static uint32_t rgb_apply_brightness(uint32_t color, uint8_t percent)
{
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);

    r = (uint8_t)((r * percent) / 100);
    g = (uint8_t)((g * percent) / 100);
    b = (uint8_t)((b * percent) / 100);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* Test 1: color display test                                          */
/* ------------------------------------------------------------------ */

static const struct
{
    char *name;
    uint32_t color;
} color_table[] =
{
    {"black (off)", 0x000000},
    {"red",         0xFF0000},
    {"green",       0x00FF00},
    {"blue",        0x0000FF},
    {"cyan",        0x00FFFF},
    {"purple",      0xFF00FF},
    {"yellow",      0xFFFF00},
    {"white",       0xFFFFFF},
};

static void test_color_display(void)
{
    rt_kprintf("\n=== Test 1: color display test ===\n");

    for (int i = 0; i < sizeof(color_table) / sizeof(color_table[0]); i++)
    {
        rt_kprintf("  color: %s\n", color_table[i].name);
        rgb_led_set_color(color_table[i].color);
        rt_thread_mdelay(1000);
    }
}

/* ------------------------------------------------------------------ */
/* Test 2: brightness test                                             */
/* ------------------------------------------------------------------ */

static void test_brightness(void)
{
    rt_kprintf("\n=== Test 2: brightness test ===\n");

    /* red ramps 0% -> 100% */
    for (int step = 0; step <= 10; step++)
    {
        uint8_t percent = step * 10;
        rt_kprintf("  red brightness: %d%%\n", percent);
        rgb_led_set_color(rgb_apply_brightness(0xFF0000, percent));
        rt_thread_mdelay(500);
    }

    /* white ramps 100% -> 0% to verify all three channels scale */
    for (int step = 10; step >= 0; step--)
    {
        uint8_t percent = step * 10;
        rt_kprintf("  white brightness: %d%%\n", percent);
        rgb_led_set_color(rgb_apply_brightness(0xFFFFFF, percent));
        rt_thread_mdelay(300);
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: breathing effect (smooth brightness)                        */
/* ------------------------------------------------------------------ */

/* Breathing with any color: keep the color constant and ramp the
 * brightness factor. period_ms controls one full in+out cycle. */
static void rgb_breathe(uint32_t color, uint8_t cycles, uint32_t period_ms)
{
    uint32_t half_ms = period_ms / 2;

    for (uint8_t c = 0; c < cycles; c++)
    {
        /* fade in */
        for (int level = 0; level <= 100; level += 2)
        {
            rgb_led_set_color(rgb_apply_brightness(color, level));
            rt_thread_mdelay(half_ms / 50);
        }
        /* fade out */
        for (int level = 100; level >= 0; level -= 2)
        {
            rgb_led_set_color(rgb_apply_brightness(color, level));
            rt_thread_mdelay(half_ms / 50);
        }
    }
    rgb_led_set_color(0x000000);
}

static void test_breathing(void)
{
    rt_kprintf("\n=== Test 3: breathing test (green) ===\n");
    rgb_breathe(0x00FF00, 2, 4000);
}

/* ------------------------------------------------------------------ */
/* Full test sequence                                                  */
/* ------------------------------------------------------------------ */

static void rgb_test_run(void)
{
    if (!rgbled_device)
        return;

    test_color_display();
    test_brightness();
    test_breathing();

    rt_kprintf("\n=== RGB LED test finished ===\n");
}

int main(void)
{
    rt_kprintf("RGB LED (WS2812B) test start, PA11 / pwmt2 ch4\n");

    rgb_led_init();

    while (1)
    {
        rgb_test_run();
        rt_thread_mdelay(3000);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* FinSH interactive commands                                          */
/* ------------------------------------------------------------------ */

static int cmd_rgb(int argc, char **argv)
{
    int r, g, b;

    if (argc != 4)
    {
        rt_kprintf("Usage: rgb <r> <g> <b>  (0~255 each)\n");
        return -1;
    }

    r = atoi(argv[1]);
    g = atoi(argv[2]);
    b = atoi(argv[3]);

    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
    {
        rt_kprintf("Invalid value, each channel must be 0~255\n");
        return -1;
    }

    if (!rgbled_device)
    {
        rt_kprintf("rgbled device not ready\n");
        return -1;
    }

    rgb_led_set_color(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    rt_kprintf("RGB set to (%d, %d, %d)\n", r, g, b);
    return 0;
}
MSH_CMD_EXPORT(cmd_rgb, set rgb led color: rgb <r> <g> <b>);

static int cmd_rgbtest(void)
{
    if (!rgbled_device)
    {
        rt_kprintf("rgbled device not ready\n");
        return -1;
    }
    rgb_test_run();
    return 0;
}
MSH_CMD_EXPORT(cmd_rgbtest, run full rgb led test sequence);

static int cmd_breathe(int argc, char **argv)
{
    int r = 0, g = 0, b = 0, period = 4000;

    if (argc < 2)
    {
        rt_kprintf("Usage: breathe <r> [g] [b] [period_ms]\n"
                   "  e.g. breathe 0 255 0    : green breathing, 4s cycle\n"
                   "  e.g. breathe 0 255 0 2000 : green breathing, 2s cycle\n");
        return -1;
    }
    if (argc >= 2) r = atoi(argv[1]);
    if (argc >= 3) g = atoi(argv[2]);
    if (argc >= 4) b = atoi(argv[3]);
    if (argc >= 5) period = atoi(argv[4]);

    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255 ||
        period < 1000 || period > 20000)
    {
        rt_kprintf("Invalid args: channels 0~255, period 1000~20000 ms\n");
        return -1;
    }

    if (!rgbled_device)
    {
        rt_kprintf("rgbled device not ready\n");
        return -1;
    }

    rt_kprintf("Breathing (%d, %d, %d), %d ms per cycle, Ctrl-C or "
               "'rgb 0 0 0' to stop\n", r, g, b, period);
    /* 10 cycles per call, blocking; use small period for responsiveness */
    rgb_breathe(((uint32_t)r << 16) | ((uint32_t)g << 8) | b, 10, period);
    return 0;
}
MSH_CMD_EXPORT(cmd_breathe, breathing led: breathe <r> [g] [b] [period_ms]);
