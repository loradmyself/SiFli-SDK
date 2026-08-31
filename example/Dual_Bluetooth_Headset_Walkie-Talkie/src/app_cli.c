/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <rtthread.h>

#include "app_bt.h"
#include "app_source.h"
#include "app_state.h"

static void app_cli_relay_help(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  intercom status\n");
    rt_kprintf("  intercom scan\n");
    rt_kprintf("  intercom connect <scan-index|XX:XX:XX:XX:XX:XX>\n");
    rt_kprintf("  intercom disconnect <headset-index|XX:XX:XX:XX:XX:XX>\n");
    rt_kprintf("  intercom talk <start|stop>\n");
    rt_kprintf("  intercom volume\n");
    rt_kprintf("  intercom volume <music|talk> <headset-index> <0-15>\n");
}

static void app_cli_source_help(void)
{
    rt_kprintf("Usage:\n");
    rt_kprintf("  intercom status\n");
    rt_kprintf("  intercom play\n");
    rt_kprintf("  intercom stop\n");
}

static void app_cli_help(void)
{
    if (app_state_get_role() == APP_ROLE_SOURCE)
    {
        app_cli_source_help();
    }
    else
    {
        app_cli_relay_help();
    }
}

static bool app_cli_parse_volume(const char *text, uint8_t *level)
{
    uint32_t value = 0U;

    if ((text == RT_NULL) || (level == RT_NULL) || (*text == '\0'))
    {
        return false;
    }
    while (*text != '\0')
    {
        if ((*text < '0') || (*text > '9'))
        {
            return false;
        }
        value = value * 10U + (uint32_t)(*text - '0');
        if (value > APP_VOLUME_LEVEL_MAX)
        {
            return false;
        }
        text++;
    }
    *level = (uint8_t)value;
    return true;
}

static bool app_cli_parse_headset(const char *text, uint8_t *headset)
{
    if ((text == RT_NULL) || (headset == RT_NULL) ||
            (text[0] < '1') || (text[0] > '2') || (text[1] != '\0'))
    {
        return false;
    }
    *headset = (uint8_t)(text[0] - '0');
    return true;
}

static bool app_cli_parse_volume_target(const char *text,
                                        app_volume_target_t *target)
{
    if (strcmp(text, "music") == 0)
    {
        *target = APP_VOLUME_TARGET_MUSIC;
        return true;
    }
    if (strcmp(text, "talk") == 0)
    {
        *target = APP_VOLUME_TARGET_TALK;
        return true;
    }
    return false;
}

static void app_cli_print_result(const char *operation, int error)
{
    if (error == RT_EOK)
    {
        rt_kprintf("[intercom] %s accepted\n", operation);
        return;
    }

    switch (error)
    {
    case -RT_ENOSYS:
        rt_kprintf("[intercom] %s rejected: subsystem is not initialized\n",
                   operation);
        break;
    case -RT_EINVAL:
        rt_kprintf("[intercom] %s rejected: invalid argument\n", operation);
        break;
    case -RT_EBUSY:
        rt_kprintf("[intercom] %s rejected: device is not ready or is busy\n",
                   operation);
        break;
    case -RT_EFULL:
        rt_kprintf("[intercom] %s rejected: command queue is full\n",
                   operation);
        break;
    default:
        rt_kprintf("[intercom] %s failed: %d\n", operation, error);
        break;
    }
}

__ROM_USED void intercom(int argc, char **argv)
{
    app_role_t role = app_state_get_role();

    if (argc < 2)
    {
        app_cli_help();
        return;
    }

    if ((strcmp(argv[1], "status") == 0) && (argc == 2))
    {
        if (role == APP_ROLE_SOURCE)
        {
            app_source_print_status();
        }
        else
        {
            app_bt_print_status();
        }
    }
    else if (role == APP_ROLE_SOURCE)
    {
        if ((strcmp(argv[1], "play") == 0) && (argc == 2))
        {
            app_cli_print_result("play", app_source_play());
        }
        else if ((strcmp(argv[1], "stop") == 0) && (argc == 2))
        {
            app_cli_print_result("stop", app_source_stop());
        }
        else
        {
            app_cli_source_help();
        }
    }
    else if ((strcmp(argv[1], "scan") == 0) && (argc == 2))
    {
        app_cli_print_result("scan", app_bt_scan());
    }
    else if ((strcmp(argv[1], "connect") == 0) && (argc == 3))
    {
        app_cli_print_result("connect", app_bt_connect(argv[2]));
    }
    else if ((strcmp(argv[1], "disconnect") == 0) && (argc == 3))
    {
        app_cli_print_result("disconnect", app_bt_disconnect(argv[2]));
    }
    else if ((strcmp(argv[1], "talk") == 0) && (argc == 3))
    {
        if (strcmp(argv[2], "start") == 0)
        {
            app_cli_print_result("talk start", app_bt_talk(true));
        }
        else if (strcmp(argv[2], "stop") == 0)
        {
            app_cli_print_result("talk stop", app_bt_talk(false));
        }
        else
        {
            app_cli_help();
        }
    }
    else if ((strcmp(argv[1], "volume") == 0) && (argc == 2))
    {
        app_bt_print_volume();
    }
    else if ((strcmp(argv[1], "volume") == 0) && (argc == 5))
    {
        app_volume_target_t target;
        uint8_t headset;
        uint8_t level;

        if (!app_cli_parse_volume_target(argv[2], &target) ||
                !app_cli_parse_headset(argv[3], &headset))
        {
            app_cli_help();
            return;
        }
        if (!app_cli_parse_volume(argv[4], &level))
        {
            rt_kprintf("[intercom] volume rejected: expected 0-15\n");
            return;
        }
        app_cli_print_result((target == APP_VOLUME_TARGET_MUSIC) ?
                             "headset music volume" :
                             "headset talk volume",
                             app_bt_set_headset_volume(target, headset,
                                     level));
    }
    else
    {
        app_cli_help();
    }
}
MSH_CMD_EXPORT(intercom, dual Bluetooth headset intercom command)
