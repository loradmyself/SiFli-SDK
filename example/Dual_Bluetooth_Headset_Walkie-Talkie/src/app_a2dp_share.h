/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_A2DP_SHARE_H
#define APP_A2DP_SHARE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_A2DP_SHARE_STATE_UNINITIALIZED = 0,
    APP_A2DP_SHARE_STATE_IDLE,
    APP_A2DP_SHARE_STATE_STARTING,
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
    /* Per-AVDTP-connection media forwarding diagnostics (connection
     * index order, see bts2_app_av_src.c). req = payloads handed to the
     * AVDTP layer, nomem = fan-out malloc failures, qos = media actually
     * moving through the stream, buff = stream buffer fullness (0..10).
     * A streaming link whose req/qos stall while another link advances
     * has a wedged media path. */
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
    uint32_t producer_starts;
    uint32_t producer_stops;
    uint32_t pcm_bytes;
    uint32_t pcm_read_timeouts;
    uint32_t sbc_frames;
    uint32_t packets_encoded;
    uint32_t packet_bytes;
    uint32_t queue_drops;
    uint32_t ring_drops;
    uint32_t records_pumped;
    uint32_t kick_count;
    uint32_t config_errors;
    uint32_t encode_errors;
} app_a2dp_share_stats_t;

typedef void (*app_a2dp_share_callback_t)(app_a2dp_share_event_t event,
                                          uint32_t sequence, int result,
                                          void *user_data);

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data);
int app_a2dp_share_start(void);
int app_a2dp_share_stop(uint32_t sequence);
int app_a2dp_share_set_volume(uint8_t level);
uint8_t app_a2dp_share_get_volume(void);

/* Call this directly from the application's BT_NOTIFY_A2DP callback. */
void app_a2dp_share_on_a2dp_event(uint16_t event_id);

/* Reconcile open/streaming Source links with the current start/stop request. */
void app_a2dp_share_reconcile_streams(void);

bool app_a2dp_share_is_stopped(void);
/* True while forwarded media is not yet safe to interrupt with connect
 * signaling: the share is coming up (stream start in progress), the send
 * ring is low (host about to starve the air), or the AVDTP send FIFOs are
 * congested (the pump is holding on credits). The connect breathe window
 * and the HFP "媒体未稳不重试" latch poll this to stay out of the media
 * critical window (see BT底层修改汇总.md L2). */
bool app_a2dp_share_media_unsettled(void);
void app_a2dp_share_get_status(app_a2dp_share_status_t *status);
void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats);
const char *app_a2dp_share_state_name(app_a2dp_share_state_t state);

#endif
