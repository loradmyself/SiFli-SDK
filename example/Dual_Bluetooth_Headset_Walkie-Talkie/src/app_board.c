/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <rtdevice.h>
#include <rtthread.h>

#include "bf0_hal.h"
#include "drv_gpio.h"
#include "drv_io.h"

#include "app_board.h"

#define APP_LED1_PIN GET_PIN(1, 31)
#define APP_LED2_PIN GET_PIN(1, 32)
#define APP_ROLE_PIN GET_PIN(1, 20)
#define APP_LED_BLINK_MS 500U

static const rt_base_t g_led_pins[APP_HEADSET_COUNT] =
{
    APP_LED1_PIN,
    APP_LED2_PIN,
};

static uint8_t g_connected_mask;
static bool g_blink_on;
static bool g_led_indicators_enabled;
static struct rt_timer g_led_timer;

static void app_board_write_led(uint8_t index, bool on)
{
    rt_pin_write(g_led_pins[index], on ? PIN_LOW : PIN_HIGH);
}

static void app_board_refresh_leds(void)
{
    uint8_t index;
    uint8_t connected_mask;
    bool blink_on;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    connected_mask = g_connected_mask;
    blink_on = g_blink_on;
    rt_hw_interrupt_enable(level);

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        bool connected = (connected_mask & (1U << index)) != 0U;
        bool on = g_led_indicators_enabled && (connected || blink_on);

        app_board_write_led(index, on);
    }
}

static void app_board_led_timer(void *parameter)
{
    rt_base_t level;

    (void)parameter;
    level = rt_hw_interrupt_disable();
    g_blink_on = !g_blink_on;
    rt_hw_interrupt_enable(level);
    app_board_refresh_leds();
}

app_role_t app_board_init(void)
{
    app_role_t role;

    HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_PULLDOWN, 1);
    rt_pin_mode(APP_ROLE_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_thread_mdelay(1U);
    role = (rt_pin_read(APP_ROLE_PIN) == PIN_HIGH) ?
           APP_ROLE_SOURCE : APP_ROLE_RELAY;

    HAL_PIN_Set(PAD_PA31, GPIO_A31, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA32, GPIO_A32, PIN_NOPULL, 1);
    rt_pin_mode(APP_LED1_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(APP_LED2_PIN, PIN_MODE_OUTPUT);
    app_board_write_led(0, false);
    app_board_write_led(1, false);

    g_connected_mask = 0U;
    g_blink_on = false;
    g_led_indicators_enabled = (role == APP_ROLE_RELAY);
    if (g_led_indicators_enabled)
    {
        rt_timer_init(&g_led_timer, "iled", app_board_led_timer, RT_NULL,
                      rt_tick_from_millisecond(APP_LED_BLINK_MS),
                      RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        RT_ASSERT(rt_timer_start(&g_led_timer) == RT_EOK);
    }

    app_board_refresh_leds();
    return role;
}

void app_board_set_headset_connected(uint8_t index, bool connected)
{
    rt_base_t level;

    if (!g_led_indicators_enabled || (index >= APP_HEADSET_COUNT))
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    if (connected)
    {
        g_connected_mask |= (uint8_t)(1U << index);
    }
    else
    {
        g_connected_mask &= (uint8_t)~(1U << index);
    }
    rt_hw_interrupt_enable(level);

    app_board_refresh_leds();
}
