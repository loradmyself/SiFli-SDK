/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_A2DP_SHARE_H
#define APP_A2DP_SHARE_H

#include <stdbool.h>
#include <stdint.h>

#include "bts2_app_interface_type.h"

typedef enum
{
    APP_A2DP_SHARE_STATE_UNINITIALIZED = 0,
    APP_A2DP_SHARE_STATE_IDLE,
    APP_A2DP_SHARE_STATE_PREFILLING,
    APP_A2DP_SHARE_STATE_RUNNING,
    APP_A2DP_SHARE_STATE_STOPPING,
    APP_A2DP_SHARE_STATE_ERROR,
} app_a2dp_share_state_t;

typedef enum
{
    APP_A2DP_SHARE_EVENT_STARTED = 0,
    APP_A2DP_SHARE_EVENT_STOPPED,
    APP_A2DP_SHARE_EVENT_ERROR,
} app_a2dp_share_event_t;

typedef struct
{
    bool initialized;
    bool desired;
    bool phone_connected;
    bool phone_streaming;
    bool producer_active;
    bool config_valid;
    bool kicked;
    uint8_t connected_count;
    uint8_t streaming_count;
    uint8_t start_pending_count;
    uint8_t suspend_pending_count;
    app_a2dp_share_state_t state;
    int last_error;
    uint32_t stop_sequence;
    uint32_t ring_bytes;
    uint32_t ring_capacity;
    uint32_t prefill_target;
    uint16_t sample_rate;
    uint16_t sbc_frame_size;
    uint16_t frames_per_packet;
    uint16_t pcm_bytes_per_packet;
    uint8_t media_con_count;
    uint8_t media_con_active;
    uint8_t media_con_mac[4][6];
    uint32_t media_req[4];
    uint32_t media_nomem[4];
    uint32_t media_qos[4];
    uint8_t media_buff[4];
} app_a2dp_share_status_t;

typedef struct
{
    uint32_t start_requests;
    uint32_t stop_requests;
    uint32_t phone_packets;
    uint32_t packets_relayed;
    uint32_t packet_bytes;
    uint32_t invalid_packets;
    uint32_t ring_drops;
    uint32_t kick_count;
    uint32_t start_errors;
    uint32_t bounce_count;
} app_a2dp_share_stats_t;

typedef void (*app_a2dp_share_callback_t)(app_a2dp_share_event_t event,
                                          uint32_t sequence, int result,
                                          void *user_data);

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data);
int app_a2dp_share_start(void);
int app_a2dp_share_stop(uint32_t sequence);
int app_a2dp_share_set_volume(uint8_t level);
uint8_t app_a2dp_share_get_volume(void);

void app_a2dp_share_set_phone_connected(bool connected);
void app_a2dp_share_set_phone_streaming(bool streaming);
void app_a2dp_share_on_media_data(const bt_notify_a2dp_data_t *media);
void app_a2dp_share_on_a2dp_event(uint16_t event_id);
void app_a2dp_share_reconcile_streams(void);

bool app_a2dp_share_is_stopped(void);
void app_a2dp_share_get_status(app_a2dp_share_status_t *status);
void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats);
const char *app_a2dp_share_state_name(app_a2dp_share_state_t state);

#endif
