/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_I2S_H
#define APP_I2S_H

#include <stdbool.h>
#include <stdint.h>

#include <rtthread.h>

#define APP_PCM_SAMPLE_RATE 44100U
#define APP_PCM_CHANNELS 2U
#define APP_PCM_BITS 16U
#define APP_I2S_DMA_HALF_SIZE 320U
#define APP_I2S_RX_RING_BLOCKS 96U
#define APP_I2S_RX_RING_SIZE (APP_I2S_DMA_HALF_SIZE * APP_I2S_RX_RING_BLOCKS)

typedef struct
{
    uint32_t rx_indicated_bytes;
    uint32_t rx_bytes;
    uint32_t rx_pipe_dropped_bytes;
    uint32_t rx_overflow_bytes;
    uint32_t rx_underflows;
    uint32_t device_errors;
} app_i2s_stats_t;

int app_i2s_init(void);
bool app_i2s_is_ready(void);
void app_i2s_set_rx_enabled(bool enabled);
rt_size_t app_i2s_rx_buffered(void);
rt_size_t app_i2s_read(void *buffer, rt_size_t size, uint32_t timeout_ms);
void app_i2s_get_stats(app_i2s_stats_t *stats);

#endif
