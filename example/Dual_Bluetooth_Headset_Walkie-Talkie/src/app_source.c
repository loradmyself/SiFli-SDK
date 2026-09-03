/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>

#include <rtthread.h>

#include "audio_mp3ctrl.h"
#include "audio_server.h"
#include "dfs_file.h"
#include "dfs_posix.h"
#include "drv_flash.h"
#include "drv_io.h"

#include "app_source.h"
#include "app_state.h"

#define APP_SOURCE_FS_DEVICE "music"
#define APP_SOURCE_MUSIC_PATH "/test1.mp3"
#define APP_SOURCE_QUEUE_DEPTH 8U
#define APP_SOURCE_THREAD_STACK 2048U

typedef enum
{
    APP_SOURCE_CMD_PLAY = 0,
    APP_SOURCE_CMD_STOP,
    APP_SOURCE_CMD_FINISHED,
} app_source_command_t;

typedef struct
{
    app_source_command_t command;
    uint32_t generation;
} app_source_message_t;

static rt_mq_t g_source_queue;
static rt_thread_t g_source_thread;
static mp3ctrl_handle g_mp3_handle;
static uint32_t g_play_generation;
static bool g_fs_mounted;
static volatile bool g_source_ready;

static int app_source_post(app_source_command_t command, uint32_t generation)
{
    app_source_message_t message =
    {
        .command = command,
        .generation = generation,
    };

    if (!g_source_ready || (g_source_queue == RT_NULL))
    {
        return -RT_ENOSYS;
    }
    return rt_mq_send(g_source_queue, &message, sizeof(message));
}

static int app_source_mp3_callback(audio_server_callback_cmt_t command,
                                   void *userdata, uint32_t reserved)
{
    (void)reserved;
    if (command == as_callback_cmd_play_to_end)
    {
        int error = app_source_post(APP_SOURCE_CMD_FINISHED,
                                    (uint32_t)(uintptr_t)userdata);

        if (error != RT_EOK)
        {
            rt_kprintf("[intercom] failed to queue playback completion: %d\n",
                       error);
        }
    }
    return 0;
}

static void app_source_close_current(void)
{
    mp3ctrl_handle handle = g_mp3_handle;

    g_mp3_handle = RT_NULL;
    if (handle != RT_NULL)
    {
        mp3ctrl_close(handle);
    }
}

static void app_source_start_playback(void)
{
    int error;

    g_play_generation++;
    if (g_play_generation == 0U)
    {
        g_play_generation++;
    }
    app_source_close_current();

    g_mp3_handle = mp3ctrl_open2(AUDIO_TYPE_LOCAL_MUSIC,
                                 APP_SOURCE_MUSIC_PATH,
                                 app_source_mp3_callback,
                                 (void *)(uintptr_t)g_play_generation,
                                 AUDIO_DEVICE_SPEAKER);
    if (g_mp3_handle == RT_NULL)
    {
        rt_kprintf("[intercom] cannot open embedded song %s\n",
                   APP_SOURCE_MUSIC_PATH);
        app_state_set_mode(APP_MODE_ERROR);
        return;
    }

    error = mp3ctrl_ioctl(g_mp3_handle, MP3CTRL_IOCTRL_LOOP_TIMES,
                          UINT32_MAX);
    if (error != 0)
    {
        rt_kprintf("[intercom] cannot enable MP3 loop playback: %d\n",
                   error);
        app_source_close_current();
        app_state_set_mode(APP_MODE_ERROR);
        return;
    }

    error = mp3ctrl_play(g_mp3_handle);
    if (error != 0)
    {
        rt_kprintf("[intercom] MP3 playback failed: %d\n", error);
        app_source_close_current();
        app_state_set_mode(APP_MODE_ERROR);
        return;
    }

    app_state_set_mode(APP_MODE_SOURCE_PLAYING);
    rt_kprintf("[intercom] playing embedded song %s (looping)\n",
               APP_SOURCE_MUSIC_PATH);
}

static void app_source_stop_playback(void)
{
    g_play_generation++;
    if (g_play_generation == 0U)
    {
        g_play_generation++;
    }
    app_source_close_current();
    app_state_set_mode(APP_MODE_IDLE);
}

static void app_source_thread_entry(void *parameter)
{
    app_source_message_t message;

    (void)parameter;
    while (1)
    {
        if (rt_mq_recv(g_source_queue, &message, sizeof(message),
                       RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }

        switch (message.command)
        {
        case APP_SOURCE_CMD_PLAY:
            app_source_start_playback();
            break;
        case APP_SOURCE_CMD_STOP:
            app_source_stop_playback();
            rt_kprintf("[intercom] source playback stopped\n");
            break;
        case APP_SOURCE_CMD_FINISHED:
            if (message.generation == g_play_generation)
            {
                app_source_close_current();
                app_state_set_mode(APP_MODE_IDLE);
                rt_kprintf("[intercom] embedded song finished\n");
            }
            break;
        default:
            break;
        }
    }
}

static int app_source_mount_music(void)
{
    register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE,
                        APP_SOURCE_FS_DEVICE);
    if (dfs_mount(APP_SOURCE_FS_DEVICE, "/", "elm", 0, 0) != 0)
    {
        rt_kprintf("[intercom] failed to mount embedded music image\n");
        return -RT_EIO;
    }
    g_fs_mounted = true;
    return RT_EOK;
}

int app_source_init(void)
{
    int error;

    HAL_PIN_Set(PAD_PA24, I2S2_MCLK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA21, I2S2_SDO, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA09, I2S2_BCK, PIN_NOPULL, 1);

    HAL_PIN_Set_DS0(PAD_PA09, 1, 1);
    HAL_PIN_Set_DS1(PAD_PA09, 1, 1);
    HAL_PIN_Set(PAD_PA08, I2S2_LRCK, PIN_NOPULL, 1);

    error = app_source_mount_music();
    if (error != RT_EOK)
    {
        return error;
    }

    g_source_queue = rt_mq_create("srcmq", sizeof(app_source_message_t),
                                  APP_SOURCE_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (g_source_queue == RT_NULL)
    {
        return -RT_ENOMEM;
    }

    g_source_thread = rt_thread_create("srcplay", app_source_thread_entry,
                                       RT_NULL, APP_SOURCE_THREAD_STACK,
                                       RT_THREAD_PRIORITY_MIDDLE,
                                       RT_THREAD_TICK_DEFAULT);
    if (g_source_thread == RT_NULL)
    {
        rt_mq_delete(g_source_queue);
        g_source_queue = RT_NULL;
        return -RT_ENOMEM;
    }

    error = rt_thread_startup(g_source_thread);
    if (error != RT_EOK)
    {
        rt_thread_delete(g_source_thread);
        rt_mq_delete(g_source_queue);
        g_source_thread = RT_NULL;
        g_source_queue = RT_NULL;
        return error;
    }

    g_source_ready = true;
    return RT_EOK;
}

bool app_source_is_ready(void)
{
    return g_source_ready;
}

int app_source_play(void)
{
    return app_source_post(APP_SOURCE_CMD_PLAY, 0U);
}

int app_source_stop(void)
{
    return app_source_post(APP_SOURCE_CMD_STOP, 0U);
}

void app_source_print_status(void)
{
    rt_kprintf("[intercom] role=source state=%s source=%s fs=%s\n",
               app_state_mode_name(app_state_get_mode()),
               g_source_ready ? "ready" : "not-ready",
               g_fs_mounted ? "mounted" : "not-mounted");
    rt_kprintf("  song: %s (MP3 44100 Hz stereo, looping until stop)\n",
               APP_SOURCE_MUSIC_PATH);
    rt_kprintf("  I2S1 TX master: PA25=SDO PA29=BCK PA30=LRCK PA24=MCLK\n");
}
