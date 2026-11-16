/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rthw.h>

#include "app_state.h"

static app_mode_t g_mode = APP_MODE_IDLE;
static app_role_t g_role = APP_ROLE_RELAY;

void app_state_init(app_role_t role)
{
    rt_base_t level = rt_hw_interrupt_disable();

    g_role = (role < APP_ROLE_COUNT) ? role : APP_ROLE_RELAY;
    g_mode = APP_MODE_IDLE;
    rt_hw_interrupt_enable(level);
}

app_role_t app_state_get_role(void)
{
    app_role_t role;
    rt_base_t level = rt_hw_interrupt_disable();

    role = g_role;
    rt_hw_interrupt_enable(level);
    return role;
}

const char *app_state_role_name(app_role_t role)
{
    static const char *const names[] =
    {
        "relay",
        "source",
    };

    if ((unsigned int)role >= APP_ROLE_COUNT)
    {
        return "unknown";
    }
    return names[role];
}

app_mode_t app_state_get_mode(void)
{
    app_mode_t mode;
    rt_base_t level = rt_hw_interrupt_disable();

    mode = g_mode;
    rt_hw_interrupt_enable(level);
    return mode;
}

void app_state_set_mode(app_mode_t mode)
{
    app_mode_t old_mode;
    rt_base_t level;

    if (mode >= APP_MODE_COUNT)
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    old_mode = g_mode;
    g_mode = mode;
    rt_hw_interrupt_enable(level);
    if (old_mode == mode)
    {
        return;
    }

    rt_kprintf("[intercom] state: %s -> %s\n",
               app_state_mode_name(old_mode), app_state_mode_name(mode));
}

const char *app_state_mode_name(app_mode_t mode)
{
    static const char *const names[] =
    {
        "idle",
        "scanning",
        "connecting",
        "media",
        "talk-starting",
        "talking",
        "talk-stopping",
        "source-playing",
        "error",
    };

    if ((unsigned int)mode >= APP_MODE_COUNT)
    {
        return "unknown";
    }

    return names[mode];
}
