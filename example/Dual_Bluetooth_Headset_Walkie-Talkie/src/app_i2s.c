/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>
#include <drivers/audio.h>
#include <ipc/ringbuffer.h>

#include "audio_pipe.h"
#include "bf0_hal.h"
#include "drv_gpio.h"
#include "drv_i2s_audio.h"
#include "drv_io.h"

#include "app_i2s.h"

#define APP_I2S_DEVICE_NAME "i2s2"
#define APP_I2S_DRIVER_PIPE_SIZE (APP_I2S_DMA_HALF_SIZE * 2U)
#define APP_I2S_RX_THREAD_PRIORITY                                         \
    (RT_THREAD_PRIORITY_HIGH + (RT_THREAD_PRIORITY_HIGHER * 2))
#define APP_I2S_EVENT_RX (1U << 0)
#define APP_I2S_EVENT_DATA (1U << 1)
#define APP_I2S_SLAVE_SPCLK_DIV 4U
#define APP_I2S_SLAVE_LRCK_HALF_DUTY 128U
#define APP_I2S_SLAVE_BCLK_DUTY 4U

static rt_device_t g_i2s_device;
static rt_event_t g_i2s_event;
static rt_mutex_t g_rx_lock;
static rt_thread_t g_i2s_thread;
static struct rt_audio_pipe *g_rx_pipe;
static struct rt_ringbuffer g_rx_ring;
ALIGN(4) static uint8_t g_rx_pool[APP_I2S_RX_RING_SIZE];
ALIGN(4) static uint8_t g_rx_dma_buffer[APP_I2S_DRIVER_PIPE_SIZE];
static app_i2s_stats_t g_stats;
static bool g_rx_enabled;
static bool g_i2s_device_opened;
static volatile bool g_i2s_initialized;
static rt_size_t g_rx_pipe_tracked_bytes;
static CLK_DIV_T g_slave_double_clock_div =
{
    APP_PCM_SAMPLE_RATE,
    APP_I2S_SLAVE_LRCK_HALF_DUTY,
    APP_I2S_SLAVE_LRCK_HALF_DUTY,
    APP_I2S_SLAVE_BCLK_DUTY,
};

static int app_i2s_apply_slave_double_clock(void)
{
    I2S_HandleTypeDef handle = {0};
    I2S_CFG_T config = {0};

    handle.Instance = hwp_i2s2;
    config.bus_dw = 32U;
    config.data_dw = APP_PCM_BITS;
    config.slave_mode = 1U;
    config.track = (APP_PCM_CHANNELS == 1U) ? 1U : 0U;
    config.chnl_sel = 0U;
    config.lrck_invert = 0U;
    config.sample_rate = APP_PCM_SAMPLE_RATE;
    config.bclk = APP_PCM_SAMPLE_RATE * 32U;
    config.vol = 4U;
    config.extern_intf = 0U;
    config.pcm_dw = APP_PCM_BITS;
    config.clk_div = &g_slave_double_clock_div;

    __HAL_I2S_SET_SPCLK_DIV(&handle, APP_I2S_SLAVE_SPCLK_DIV);
    if ((HAL_I2S_Config_Transmit(&handle, &config) != HAL_OK) ||
            (HAL_I2S_Config_Receive(&handle, &config) != HAL_OK))
    {
        return -RT_ERROR;
    }
    __DSB();
    return RT_EOK;
}

static void app_i2s_config_pins(void)
{
    HAL_PIN_Set(PAD_PA24, GPIO_A24, PIN_PULLDOWN, 1);
    rt_pin_mode(GET_PIN(1, 24), PIN_MODE_INPUT_PULLDOWN);
    //HAL_PIN_Set(PAD_PA21, GPIO_A21, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA27, I2S2_SDI, PIN_PULLDOWN, 1);
    HAL_PIN_Set(PAD_PA23, I2S2_BCK, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA25, I2S2_LRCK, PIN_NOPULL, 1);
}

static int app_i2s_configure(void)
{
    struct rt_audio_caps caps = {0};
    rt_err_t error;

    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.channels = APP_PCM_CHANNELS;
    caps.udata.config.samplerate = APP_PCM_SAMPLE_RATE;
    caps.udata.config.samplefmt = APP_PCM_BITS;
    error = rt_device_control(g_i2s_device, AUDIO_CTL_CONFIGURE, &caps);
    if (error != RT_EOK)
    {
        return error;
    }

    error = rt_device_control(g_i2s_device, AUDIO_CTL_SETINPUT, (void *)0);
    if (error != RT_EOK)
    {
        return error;
    }

    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_MODE;
    caps.udata.value = 1U;
    return rt_device_control(g_i2s_device, AUDIO_CTL_CONFIGURE, &caps);
}

static rt_err_t app_i2s_rx_indicate(rt_device_t device, rt_size_t size)
{
    rt_size_t occupancy;
    rt_size_t accepted;

    (void)device;
    g_stats.rx_indicated_bytes += size;
    if (g_rx_pipe != RT_NULL)
    {
        occupancy = rt_ringbuffer_data_len(&g_rx_pipe->ringbuffer);
        if (occupancy < g_rx_pipe_tracked_bytes)
        {
            g_stats.device_errors++;
            accepted = 0U;
        }
        else
        {
            accepted = occupancy - g_rx_pipe_tracked_bytes;
        }
        if (accepted > size)
        {
            g_stats.device_errors++;
            accepted = size;
        }
        if (accepted < size)
        {
            g_stats.rx_pipe_dropped_bytes += size - accepted;
        }
        g_rx_pipe_tracked_bytes = occupancy;
    }
    return rt_event_send(g_i2s_event, APP_I2S_EVENT_RX);
}

static void app_i2s_rx_thread(void *parameter)
{
    rt_uint32_t events;

    (void)parameter;
    while (1)
    {
        rt_event_recv(g_i2s_event, APP_I2S_EVENT_RX,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &events);

        while (1)
        {
            bool buffered = false;
            rt_base_t level = rt_hw_interrupt_disable();
            rt_size_t length = rt_device_read(g_i2s_device, 0,
                                              g_rx_dma_buffer,
                                              sizeof(g_rx_dma_buffer));

            if (length > g_rx_pipe_tracked_bytes)
            {
                g_rx_pipe_tracked_bytes = 0U;
                g_stats.device_errors++;
            }
            else
            {
                g_rx_pipe_tracked_bytes -= length;
            }
            rt_hw_interrupt_enable(level);
            if (length == 0U)
            {
                break;
            }
            if ((length % APP_I2S_DMA_HALF_SIZE) != 0U)
            {
                g_stats.device_errors++;
                continue;
            }

            g_stats.rx_bytes += length;
            rt_mutex_take(g_rx_lock, RT_WAITING_FOREVER);
            if (!g_rx_enabled)
            {
                rt_mutex_release(g_rx_lock);
                continue;
            }
            {
                rt_size_t space = rt_ringbuffer_space_len(&g_rx_ring);

                if (space < length)
                {
                    g_stats.rx_overflow_bytes += length - space;
                }
                rt_ringbuffer_put_force(&g_rx_ring, g_rx_dma_buffer, length);
                buffered = true;
            }
            rt_mutex_release(g_rx_lock);
            if (buffered)
            {
                rt_event_send(g_i2s_event, APP_I2S_EVENT_DATA);
            }
        }
    }
}

static void app_i2s_cleanup(void)
{
    g_i2s_initialized = false;
    g_rx_enabled = false;

    if (g_i2s_device_opened && (g_i2s_device != RT_NULL))
    {
        rt_device_set_rx_indicate(g_i2s_device, RT_NULL);
    }
    if (g_i2s_thread != RT_NULL)
    {
        rt_thread_delete(g_i2s_thread);
        g_i2s_thread = RT_NULL;
    }
    if (g_i2s_device_opened && (g_i2s_device != RT_NULL))
    {
        rt_device_close(g_i2s_device);
    }
    g_i2s_device_opened = false;
    g_i2s_device = RT_NULL;
    g_rx_pipe = RT_NULL;
    g_rx_pipe_tracked_bytes = 0U;

    if (g_i2s_event != RT_NULL)
    {
        rt_event_delete(g_i2s_event);
        g_i2s_event = RT_NULL;
    }
    if (g_rx_lock != RT_NULL)
    {
        rt_mutex_delete(g_rx_lock);
        g_rx_lock = RT_NULL;
    }
}

int app_i2s_init(void)
{
    struct rt_audio_device *audio;
    rt_err_t error;
    int stream;

    if (g_i2s_initialized)
    {
        return RT_EOK;
    }
    app_i2s_cleanup();
    memset(&g_stats, 0, sizeof(g_stats));
    app_i2s_config_pins();

    g_i2s_event = rt_event_create("i2se", RT_IPC_FLAG_FIFO);
    g_rx_lock = rt_mutex_create("i2srb", RT_IPC_FLAG_FIFO);
    if ((g_i2s_event == RT_NULL) || (g_rx_lock == RT_NULL))
    {
        error = -RT_ENOMEM;
        goto failed;
    }
    rt_ringbuffer_init(&g_rx_ring, g_rx_pool, sizeof(g_rx_pool));

    g_i2s_device = rt_device_find(APP_I2S_DEVICE_NAME);
    if (g_i2s_device == RT_NULL)
    {
        error = -RT_ENOSYS;
        goto failed;
    }

    error = rt_device_open(g_i2s_device, RT_DEVICE_FLAG_RDWR);
    if (error != RT_EOK)
    {
        goto failed;
    }
    g_i2s_device_opened = true;

    audio = (struct rt_audio_device *)g_i2s_device;
    if ((audio->record == RT_NULL) || (audio->record->audio_pipe == RT_NULL))
    {
        error = -RT_ENOSYS;
        goto failed;
    }
    g_rx_pipe = audio->record->audio_pipe;
    g_rx_pipe_tracked_bytes =
        rt_ringbuffer_data_len(&g_rx_pipe->ringbuffer);

    error = app_i2s_configure();
    if (error != RT_EOK)
    {
        goto failed;
    }

    g_i2s_thread = rt_thread_create("i2srx", app_i2s_rx_thread, RT_NULL,
                                    1024, APP_I2S_RX_THREAD_PRIORITY,
                                    RT_THREAD_TICK_DEFAULT);
    if (g_i2s_thread == RT_NULL)
    {
        error = -RT_ENOMEM;
        goto failed;
    }
    error = rt_device_set_rx_indicate(g_i2s_device, app_i2s_rx_indicate);
    if (error != RT_EOK)
    {
        goto failed;
    }
    error = rt_thread_startup(g_i2s_thread);
    if (error != RT_EOK)
    {
        goto failed;
    }

    stream = AUDIO_STREAM_RECORD;
    error = rt_device_control(g_i2s_device, AUDIO_CTL_START, &stream);
    if (error != RT_EOK)
    {
        goto failed;
    }
    error = app_i2s_apply_slave_double_clock();
    if (error != RT_EOK)
    {
        goto failed;
    }

    g_i2s_initialized = true;
    return RT_EOK;

failed:
    app_i2s_cleanup();
    return error;
}

bool app_i2s_is_ready(void)
{
    return g_i2s_initialized;
}

void app_i2s_set_rx_enabled(bool enabled)
{
    if (!g_i2s_initialized || (g_rx_lock == RT_NULL))
    {
        return;
    }

    rt_mutex_take(g_rx_lock, RT_WAITING_FOREVER);
    g_rx_enabled = enabled;
    rt_ringbuffer_reset(&g_rx_ring);
    rt_mutex_release(g_rx_lock);
}

rt_size_t app_i2s_rx_buffered(void)
{
    rt_size_t length;

    if (!g_i2s_initialized || (g_rx_lock == RT_NULL))
    {
        return 0U;
    }

    rt_mutex_take(g_rx_lock, RT_WAITING_FOREVER);
    length = g_rx_enabled ? rt_ringbuffer_data_len(&g_rx_ring) : 0U;
    rt_mutex_release(g_rx_lock);
    return length;
}

rt_size_t app_i2s_read(void *buffer, rt_size_t size, uint32_t timeout_ms)
{
    rt_tick_t timeout;
    rt_tick_t start;

    if (!g_i2s_initialized || (g_rx_lock == RT_NULL) ||
            (buffer == RT_NULL) || (size == 0U) ||
            ((size % APP_I2S_DMA_HALF_SIZE) != 0U) ||
            (size > APP_I2S_RX_RING_SIZE))
    {
        return 0U;
    }

    timeout = rt_tick_from_millisecond(timeout_ms);
    start = rt_tick_get();
    while (1)
    {
        rt_size_t length;
        rt_uint32_t events;
        rt_tick_t elapsed;
        rt_tick_t remaining;

        rt_mutex_take(g_rx_lock, RT_WAITING_FOREVER);
        if (!g_rx_enabled)
        {
            rt_mutex_release(g_rx_lock);
            return 0U;
        }
        if (rt_ringbuffer_data_len(&g_rx_ring) >= size)
        {
            length = rt_ringbuffer_get(&g_rx_ring, buffer, size);
            rt_mutex_release(g_rx_lock);
            return length;
        }
        rt_mutex_release(g_rx_lock);

        elapsed = rt_tick_get() - start;
        if (elapsed >= timeout)
        {
            g_stats.rx_underflows++;
            return 0U;
        }
        remaining = timeout - elapsed;

        rt_event_recv(g_i2s_event, APP_I2S_EVENT_DATA,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      remaining, &events);
    }
}

void app_i2s_get_stats(app_i2s_stats_t *stats)
{
    if (stats != RT_NULL)
    {
        *stats = g_stats;
    }
}
