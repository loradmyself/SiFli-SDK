/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "app_a2dp_share.h"
#include "app_bt.h"
#include "app_i2s.h"
#include "app_types.h"

#include "av_sbc_api.h"
#include "bts2_app_inc.h"
#include "bts2_msg.h"
#include "ipc/ringbuffer32.h"

#if defined(CFG_AV_SRC) && defined(CFG_AV_SHARING)

#define APP_SHARE_RTP_HEADER_SIZE AV_FIXED_MEDIA_PKT_HDR_SIZE
#define APP_SHARE_SBC_HEADER_SIZE 1U
#define APP_SHARE_PACKET_HEADER_SIZE                                      \
    (APP_SHARE_RTP_HEADER_SIZE + APP_SHARE_SBC_HEADER_SIZE)
#define APP_SHARE_RING_SIZE                                               \
    (SINK_DATA_LIST_MAX_THRESHOLD * AV_MTU_SIZE)
#define APP_SHARE_PREFILL_MS 100U
#define APP_SHARE_MAX_FRAMES_PER_PACKET 10U
#define APP_SHARE_MAX_PCM_BYTES                                           \
    (APP_SHARE_MAX_FRAMES_PER_PACKET * 16U * 8U * 2U * sizeof(int16_t))
#define APP_SHARE_RECORD_QUEUE_DEPTH 12U
#define APP_SHARE_RECORD_SIZE (sizeof(uint16_t) + AV_MTU_SIZE)
#define APP_SHARE_QUEUE_NODE_SIZE                                         \
    (RT_ALIGN(APP_SHARE_RECORD_SIZE, RT_ALIGN_SIZE) + sizeof(void *))
#define APP_SHARE_QUEUE_POOL_SIZE                                         \
    (APP_SHARE_QUEUE_NODE_SIZE * APP_SHARE_RECORD_QUEUE_DEPTH)
#define APP_SHARE_THREAD_STACK_SIZE 3072U
#define APP_SHARE_CONTROL_EVENT (1UL << 0)
#define APP_SHARE_PUMP_ACTIVE_MS 5U
#define APP_SHARE_PUMP_WAIT_MS 10U
#define APP_SHARE_STOP_SETTLE_MS 100U
#define APP_SHARE_PCM_READ_TIMEOUT_MS 20U
#define APP_SHARE_VOLUME_RAMP_MS 10U
#define APP_SHARE_GAIN_UNITY_Q15 32768L
#define APP_SHARE_VOLUME_RAMP_FRAMES                                    \
    ((APP_PCM_SAMPLE_RATE * APP_SHARE_VOLUME_RAMP_MS + 999U) / 1000U)

/* Minimum spacing between bt_av_start_stream() attempts per connection.
 * Covers both the post-open initial delay (A) and the reject back-off (B):
 * after each attempt the next one is held off for this interval, which
 * breaks the ~34 ms "reject → pump → reconcile → start" death loop and
 * gives the headset time to finish its A2DP sink init after reconnect. */
#define APP_SHARE_START_RETRY_MS 1500U

/* Maximum time to wait for the ring to reach the prefill target before
 * force-kicking the stream. When a second sink joins and starts draining
 * the ring, the buffer may never reach the full prefill target — without
 * this timeout the share stays in PREFILLING forever (silent). */
#define APP_SHARE_PREFILL_TIMEOUT_MS 2000U

/* Ring level controller for two-sink sync. bt_avsrc_sharing distributes
 * from one shared ring to all sinks equally — per-sink pacing is not
 * possible at this layer. Instead we steer the ENCODE rate to keep the
 * ring near a target level: if the ring drains low the faster sink is
 * winning (slow the encoder); if it fills high both sinks lag (speed up).
 * This pulls the two sinks' consumption rates together and shrinks
 * playback drift without touching the BT submodule. */
#define APP_SHARE_SYNC_RING_TARGET_MS 80U
#define APP_SHARE_SYNC_STEP_US 50U

/* Forced two-sink resync: every APP_SHARE_RESYNC_INTERVAL_MS, briefly
 * suspend then resume all A2DP streams. This flushes both headsets'
 * jitter buffers so they restart from the same ring position, acting
 * as a hard "sync floor" against clock-drift accumulation. The brief
 * pause (APP_SHARE_RESYNC_PAUSE_MS) is the cost of the guarantee. */
#define APP_SHARE_RESYNC_INTERVAL_MS 30000U
#define APP_SHARE_RESYNC_PAUSE_MS 100U

typedef struct
{
    uint16_t packet_length;
    uint8_t packet[AV_MTU_SIZE];
} app_share_record_t;

typedef struct
{
    bts2_sbc_cfg sbc;
    uint8_t connected_count;
    uint8_t streaming_count;
    uint8_t start_pending_count;
    uint8_t suspend_pending_count;
    bool config_valid;
    bool all_streaming;
    uint8_t media_streaming_mask;
    uint8_t media_con_active;
    uint8_t media_mac[MAX_CONNS][6];
    uint32_t media_req[MAX_CONNS];
    uint32_t media_nomem[MAX_CONNS];
    uint32_t media_qos[MAX_CONNS];
    uint8_t media_buff[MAX_CONNS];
} app_share_link_snapshot_t;

static struct rt_mutex g_share_lock;
static struct rt_event g_control_event;
static struct rt_messagequeue g_record_queue;
static struct rt_thread g_producer_thread;
static struct rt_ringbuffer32 g_share_ring;

/* Per-connection tick of the last bt_av_start_stream() attempt (0 = none
 * yet). Used to space out start attempts (see APP_SHARE_START_RETRY_MS). */
static rt_tick_t g_start_retry_tick[MAX_CONNS];

/* Tick when the producer entered PREFILLING state; used to force-kick
 * after APP_SHARE_PREFILL_TIMEOUT_MS if the ring never fills enough. */
static rt_tick_t g_prefill_start_tick;

ALIGN(4) static rt_uint8_t g_share_ring_pool[APP_SHARE_RING_SIZE];
static rt_uint32_t
g_record_queue_pool[(APP_SHARE_QUEUE_POOL_SIZE + sizeof(uint32_t) - 1U) /
                    sizeof(uint32_t)];
ALIGN(4) static rt_uint8_t g_producer_stack[APP_SHARE_THREAD_STACK_SIZE];
ALIGN(4) static uint8_t g_i2s_block[APP_I2S_DMA_HALF_SIZE];
ALIGN(4) static uint8_t
g_pcm_buffer[APP_SHARE_MAX_PCM_BYTES + APP_I2S_DMA_HALF_SIZE];
ALIGN(4) static app_share_record_t g_producer_record;
ALIGN(4) static app_share_record_t g_pump_record;
/* Separate staging buffer for the stop-path drain so the producer's
 * pump and the BT task's discard cannot scribble on each other. */
ALIGN(4) static app_share_record_t g_discard_record;

static app_a2dp_share_status_t g_status;
static app_a2dp_share_stats_t g_stats;
static bts2_sbc_cfg g_active_config;
static app_a2dp_share_callback_t g_callback;
static void *g_callback_user_data;
static rt_size_t g_pcm_used;
static bool g_started_callback_pending;
static bool g_started_callback_sent;
static bool g_stop_callback_pending;
static bool g_bts_quiesced;
static bool g_stop_needs_settle;
static bool g_pump_scheduled;
static bool g_encoder_cleanup_pending;
static rt_tick_t g_stop_settle_deadline;
static uint32_t g_encoder_generation;
static uint8_t g_music_volume_level;
static int32_t g_music_gain_q15;
static int32_t g_music_gain_target_q15;
static uint32_t g_music_gain_ramp_frames;

/* Encode-rate pacing for two-sink sync (see APP_SHARE_SYNC_*). */
static int32_t g_sync_encode_delay_us;

/* Forced resync state: 0 = idle, 1 = suspended, waiting to resume. */
static uint8_t g_resync_phase;
static rt_tick_t g_resync_last_tick;

static void app_share_bts_pump(uint16_t message, void *parameter);
static void app_share_pump_records(void);

static void app_share_lock(void)
{
    rt_mutex_take(&g_share_lock, RT_WAITING_FOREVER);
}

static void app_share_unlock(void)
{
    rt_mutex_release(&g_share_lock);
}

static bool app_share_tick_reached(rt_tick_t now, rt_tick_t deadline)
{
    return (rt_int32_t)(now - deadline) >= 0;
}

static int32_t app_share_volume_gain(uint8_t level)
{
    return ((int32_t)level * APP_SHARE_GAIN_UNITY_Q15 +
            (APP_VOLUME_LEVEL_MAX / 2U)) / APP_VOLUME_LEVEL_MAX;
}

static void app_share_apply_music_volume(uint8_t *pcm, rt_size_t bytes)
{
    int16_t *samples = (int16_t *)pcm;
    rt_size_t frame_count = bytes /
                            (APP_PCM_CHANNELS * sizeof(int16_t));
    rt_size_t frame;
    uint8_t channel;

    app_share_lock();
    if ((g_music_gain_ramp_frames == 0U) &&
            (g_music_gain_q15 == APP_SHARE_GAIN_UNITY_Q15))
    {
        app_share_unlock();
        return;
    }

    for (frame = 0U; frame < frame_count; frame++)
    {
        if (g_music_gain_ramp_frames != 0U)
        {
            int32_t difference = g_music_gain_target_q15 -
                                 g_music_gain_q15;

            if (g_music_gain_ramp_frames == 1U)
            {
                g_music_gain_q15 = g_music_gain_target_q15;
                g_music_gain_ramp_frames = 0U;
            }
            else
            {
                g_music_gain_q15 += difference /
                                    (int32_t)g_music_gain_ramp_frames;
                g_music_gain_ramp_frames--;
            }
        }

        for (channel = 0U; channel < APP_PCM_CHANNELS; channel++)
        {
            rt_size_t sample_index = frame * APP_PCM_CHANNELS + channel;
            int32_t scaled = (int32_t)samples[sample_index] *
                             g_music_gain_q15;

            samples[sample_index] = (int16_t)(scaled >> 15);
        }
    }
    app_share_unlock();
}

static void app_share_notify(app_a2dp_share_event_t event,
                             uint32_t sequence, int result)
{
    app_a2dp_share_callback_t callback;
    void *user_data;

    app_share_lock();
    callback = g_callback;
    user_data = g_callback_user_data;
    app_share_unlock();
    if (callback != RT_NULL)
    {
        callback(event, sequence, result, user_data);
    }
}

static bool app_share_config_equal(const bts2_sbc_cfg *left,
                                   const bts2_sbc_cfg *right)
{
    return (left->chnl_mode == right->chnl_mode) &&
           (left->alloc_method == right->alloc_method) &&
           (left->sample_freq == right->sample_freq) &&
           (left->frmsize == right->frmsize) &&
           (left->blocks == right->blocks) &&
           (left->subbands == right->subbands) &&
           (left->bit_pool == right->bit_pool) &&
           (left->chnls == right->chnls) &&
           (left->frms_per_payload == right->frms_per_payload) &&
           (left->bytes_per_encoding == right->bytes_per_encoding) &&
           (left->samples_per_l2c_frm == right->samples_per_l2c_frm) &&
           (left->bytes_to_rd == right->bytes_to_rd);
}

static bool app_share_validate_config(const bts2_sbc_cfg *config)
{
    uint32_t expected_pcm_bytes;
    uint32_t packet_length;

    if ((config->sample_freq != APP_PCM_SAMPLE_RATE) ||
            (config->chnls != APP_PCM_CHANNELS) ||
            (config->chnl_mode == SBC_MONO) ||
            (config->blocks == 0U) || (config->subbands == 0U) ||
            (config->bit_pool == 0U) || (config->frmsize == 0U) ||
            (config->frms_per_payload == 0U) ||
            (config->frms_per_payload > APP_SHARE_MAX_FRAMES_PER_PACKET))
    {
        return false;
    }

    expected_pcm_bytes = (uint32_t)config->blocks * config->subbands *
                         config->chnls * sizeof(int16_t) *
                         config->frms_per_payload;
    packet_length = APP_SHARE_PACKET_HEADER_SIZE +
                    (uint32_t)config->frmsize *
                    config->frms_per_payload;
    return (config->bytes_per_encoding ==
            ((uint32_t)config->blocks * config->subbands * config->chnls *
             sizeof(int16_t))) &&
           (config->bytes_to_rd == expected_pcm_bytes) &&
           (expected_pcm_bytes <= APP_SHARE_MAX_PCM_BYTES) &&
           (packet_length <= AV_MTU_SIZE) &&
           (bts2_sbc_calculate_framelen(config->chnl_mode,
                                        config->blocks,
                                        config->subbands,
                                        config->bit_pool) ==
            config->frmsize);
}

static uint32_t app_share_calculate_prefill_target(
    const bts2_sbc_cfg *config)
{
    uint64_t samples_per_packet;
    uint64_t target_samples;
    uint64_t packet_count;
    uint64_t record_size;
    uint64_t target_size;

    samples_per_packet = (uint64_t)config->blocks * config->subbands *
                         config->frms_per_payload;
    target_samples = ((uint64_t)config->sample_freq * APP_SHARE_PREFILL_MS +
                      999U) / 1000U;
    if ((samples_per_packet == 0U) || (target_samples == 0U))
    {
        return 0U;
    }

    packet_count = (target_samples + samples_per_packet - 1U) /
                   samples_per_packet;
    record_size = sizeof(uint16_t) + APP_SHARE_PACKET_HEADER_SIZE +
                  (uint64_t)config->frmsize * config->frms_per_payload;
    target_size = packet_count * record_size;
    if (target_size == 0U)
    {
        return 0U;
    }
    /* Cap at half the ring so the prefill never fills the entire buffer
     * (which would starve the pump and leave no room for the encoder).
     * If the requested target is larger, use the cap instead of failing —
     * a smaller prefill is always better than no audio at all. */
    if (target_size >= (APP_SHARE_RING_SIZE / 2U))
    {
        target_size = APP_SHARE_RING_SIZE / 2U;
    }
    return (uint32_t)target_size;
}

static void app_share_collect_links(app_share_link_snapshot_t *snapshot)
{
    bts2s_av_inst_data *instance;
    bts2_sbc_cfg first_config = {0};
    bool have_config = false;
    bool configs_match = true;
    rt_base_t level;
    uint8_t index;

    memset(snapshot, 0, sizeof(*snapshot));
    instance = bt_av_get_inst_data();
    if (instance == RT_NULL)
    {
        return;
    }

    level = rt_hw_interrupt_disable();
    for (index = 0U; index < MAX_CONNS; index++)
    {
        const bts2_av_conn *connection = &instance->con[index];

        if (bt_avsrc_get_media_stats(index, &snapshot->media_req[index],
                                     &snapshot->media_nomem[index],
                                     &snapshot->media_qos[index],
                                     &snapshot->media_buff[index]))
        {
            if ((connection->cfg == AV_AUDIO_SRC) ||
                    (snapshot->media_req[index] != 0U) ||
                    (snapshot->media_qos[index] != 0U))
            {
                bt_addr_convert(&connection->av_rmt_addr,
                                snapshot->media_mac[index]);
                snapshot->media_con_active |= (uint8_t)(1U << index);
            }
        }
        if ((connection->cfg == AV_AUDIO_SRC) &&
                (connection->st == avconned_streaming))
        {
            snapshot->media_streaming_mask |= (uint8_t)(1U << index);
        }
        if (connection->cfg != AV_AUDIO_SRC)
        {
            continue;
        }
        if (connection->start_pending)
        {
            snapshot->start_pending_count++;
        }
        if (connection->suspend_pending)
        {
            snapshot->suspend_pending_count++;
        }
        if (connection->st <= avconned)
        {
            continue;
        }
        snapshot->connected_count++;
        if (connection->st == avconned_streaming)
        {
            snapshot->streaming_count++;
            /* Only links that are actually streaming contribute encode
             * params: a half-open joiner (stuck between open and
             * streaming) carries an empty act_cfg and used to veto the
             * config for EVERYONE — total silence on all headsets. */
            if (!have_config)
            {
                first_config = connection->act_cfg;
                have_config = true;
            }
            else if (!app_share_config_equal(&first_config,
                                             &connection->act_cfg))
            {
                configs_match = false;
            }
        }
    }
    rt_hw_interrupt_enable(level);

    snapshot->sbc = first_config;
    snapshot->all_streaming =
        (snapshot->connected_count > 0U) &&
        (snapshot->streaming_count == snapshot->connected_count) &&
        (snapshot->start_pending_count == 0U) &&
        (snapshot->suspend_pending_count == 0U);
    /* "Whoever streams gets audio": encoding needs ONE streaming link
     * with a valid config — not every connected link streaming. The
     * all_streaming veto used to silence BOTH headsets whenever a
     * joiner's stream start stalled. */
    snapshot->config_valid = (snapshot->streaming_count > 0U) &&
                             have_config && configs_match &&
                             app_share_validate_config(&first_config);
}

static void app_share_update_link_status(
    const app_share_link_snapshot_t *snapshot)
{
    app_share_lock();
    g_status.connected_count = snapshot->connected_count;
    g_status.streaming_count = snapshot->streaming_count;
    g_status.start_pending_count = snapshot->start_pending_count;
    g_status.suspend_pending_count = snapshot->suspend_pending_count;
    g_status.media_con_count = MAX_CONNS;
    g_status.media_con_active = snapshot->media_con_active;
    /* Sizes come from the source arrays: the snapshot holds
     * MAX_CONNS entries while the published status has room for
     * four; never copy more than the snapshot actually has. */
    memset(&g_status.media_con_mac[MAX_CONNS], 0,
           sizeof(g_status.media_con_mac) -
           sizeof(snapshot->media_mac));
    memcpy(g_status.media_con_mac, snapshot->media_mac,
           sizeof(snapshot->media_mac));
    memcpy(g_status.media_req, snapshot->media_req,
           sizeof(snapshot->media_req));
    memcpy(g_status.media_nomem, snapshot->media_nomem,
           sizeof(snapshot->media_nomem));
    memcpy(g_status.media_qos, snapshot->media_qos,
           sizeof(snapshot->media_qos));
    memcpy(g_status.media_buff, snapshot->media_buff,
           sizeof(snapshot->media_buff));
    app_share_unlock();
}

/* ---- wedged media-path watchdog ---------------------------------
 * The shared sender can wedge one stream's media path inside the AVDTP
 * layer: the app reports the link as streaming (and keeps handing
 * payloads to av_stream_data_req) but the headset never gets audio and
 * never recovers on its own — teardown + reconnect is the proven heal.
 * Detect the stall from the per-connection media counters and bounce
 * that one link (app_bt_bounce_media_link disconnects it and lets the
 * existing auto-reconnect queue bring it back as our own outgoing
 * connect, the path that is proven to deliver audio). */
#define APP_SHARE_WEDGE_CHECK_MS 1000U
#define APP_SHARE_WEDGE_STALL_MS 3000U
#define APP_SHARE_WEDGE_BUFF_FULL 8U
#define APP_SHARE_WEDGE_BUFF_STREAK 2U
#define APP_SHARE_WEDGE_BOUNCE_COOLDOWN_MS 30000U
#define APP_SHARE_WEDGE_BOUNCE_MAX 3U

static rt_tick_t g_wedge_check_tick;
static uint32_t g_wedge_sig[MAX_CONNS];
static rt_tick_t g_wedge_stall_tick[MAX_CONNS];
static uint8_t g_wedge_buff_streak[MAX_CONNS];
static uint8_t g_wedge_bounce_count[MAX_CONNS];
static rt_tick_t g_wedge_bounce_tick[MAX_CONNS];
static bool g_wedge_qos_seen;

static void app_share_wedge_check(const app_share_link_snapshot_t *links)
{
    rt_tick_t now = rt_tick_get();
    bool qos_seen = false;
    bool progress = false;
    uint8_t i;

    if ((now - g_wedge_check_tick) <
            rt_tick_from_millisecond(APP_SHARE_WEDGE_CHECK_MS))
    {
        return;
    }
    g_wedge_check_tick = now;

    for (i = 0U; i < MAX_CONNS; i++)
    {
        if (links->media_qos[i] != 0U)
        {
            qos_seen = true;
            break;
        }
    }
    if (qos_seen != g_wedge_qos_seen)
    {
        /* Signal source switched (QOS appeared): rebuild the baseline
         * so the switch itself cannot look like a stall. */
        g_wedge_qos_seen = qos_seen;
        for (i = 0U; i < MAX_CONNS; i++)
        {
            g_wedge_sig[i] = qos_seen ? links->media_qos[i]
                             : links->media_req[i];
            g_wedge_stall_tick[i] = 0U;
            g_wedge_buff_streak[i] = 0U;
        }
        return;
    }

    for (i = 0U; i < MAX_CONNS; i++)
    {
        if ((links->media_streaming_mask & (uint8_t)(1U << i)) != 0U)
        {
            uint32_t sig = qos_seen ? links->media_qos[i]
                           : links->media_req[i];

            if (sig > g_wedge_sig[i])
            {
                progress = true;
            }
        }
    }

    for (i = 0U; i < MAX_CONNS; i++)
    {
        uint32_t sig;
        bool stalled;
        uint8_t j;

        if ((links->media_streaming_mask & (uint8_t)(1U << i)) == 0U)
        {
            g_wedge_stall_tick[i] = 0U;
            g_wedge_buff_streak[i] = 0U;
            continue;
        }

        sig = qos_seen ? links->media_qos[i] : links->media_req[i];
        stalled = false;

        if (sig > g_wedge_sig[i])
        {
            g_wedge_stall_tick[i] = 0U;
        }
        else if (progress)
        {
            /* Data is being delivered somewhere but not to this
             * stream: the media path of this link is wedged. */
            if (g_wedge_stall_tick[i] == 0U)
            {
                g_wedge_stall_tick[i] = now;
            }
            else if ((now - g_wedge_stall_tick[i]) >=
                     rt_tick_from_millisecond(APP_SHARE_WEDGE_STALL_MS))
            {
                stalled = true;
            }
        }

        /* Queued-not-drained variant: the stream buffer sits near full
         * while another streaming link stays near empty. */
        if (links->media_buff[i] >= APP_SHARE_WEDGE_BUFF_FULL)
        {
            for (j = 0U; j < MAX_CONNS; j++)
            {
                if ((j != i) &&
                        ((links->media_streaming_mask &
                          (uint8_t)(1U << j)) != 0U) &&
                        (links->media_buff[j] <= 5U))
                {
                    break;
                }
            }
            if (j < MAX_CONNS)
            {
                if (g_wedge_buff_streak[i] < 0xFFU)
                {
                    g_wedge_buff_streak[i]++;
                }
                if (g_wedge_buff_streak[i] >= APP_SHARE_WEDGE_BUFF_STREAK)
                {
                    stalled = true;
                }
            }
        }
        else
        {
            g_wedge_buff_streak[i] = 0U;
        }

        if (stalled)
        {
            g_wedge_stall_tick[i] = 0U;
            g_wedge_buff_streak[i] = 0U;
            if ((g_wedge_bounce_count[i] < APP_SHARE_WEDGE_BOUNCE_MAX) &&
                    ((g_wedge_bounce_tick[i] == 0U) ||
                     ((now - g_wedge_bounce_tick[i]) >=
                      rt_tick_from_millisecond(
                          APP_SHARE_WEDGE_BOUNCE_COOLDOWN_MS))))
            {
                uint8_t mac[6];

                memcpy(mac, links->media_mac[i], sizeof(mac));
                g_wedge_bounce_count[i]++;
                g_wedge_bounce_tick[i] = now;
                rt_kprintf("[intercom] wedged media path on link %u "
                           "(req=%u qos=%u buff=%u)\n",
                           i, (unsigned int)links->media_req[i],
                           (unsigned int)links->media_qos[i],
                           links->media_buff[i]);
                app_bt_bounce_media_link(mac);
            }
        }
    }

    for (i = 0U; i < MAX_CONNS; i++)
    {
        g_wedge_sig[i] = qos_seen ? links->media_qos[i]
                         : links->media_req[i];
    }
}

static void app_share_schedule_pump(uint32_t delay_ms)
{
    bool schedule = false;

    app_share_lock();
    if (g_status.initialized && !g_pump_scheduled)
    {
        g_pump_scheduled = true;
        schedule = true;
    }
    app_share_unlock();

    if (schedule)
    {
        bts2_timer_ev_add(delay_ms, app_share_bts_pump, 0U, RT_NULL);
    }
}

static void app_share_fail(int error)
{
    bool notify = false;

    app_share_lock();
    if (g_status.state != APP_A2DP_SHARE_STATE_ERROR)
    {
        g_status.desired = false;
        g_status.state = APP_A2DP_SHARE_STATE_ERROR;
        g_status.last_error = error;
        g_started_callback_pending = false;
        g_stats.config_errors++;
        notify = true;
    }
    app_share_unlock();

    app_i2s_set_rx_enabled(false);
    app_a2dp_share_reconcile_streams();
    if (notify)
    {
        app_share_notify(APP_A2DP_SHARE_EVENT_ERROR, 0U, error);
    }
}

static int app_share_start_producer(
    const app_share_link_snapshot_t *snapshot)
{
    uint32_t generation;
    uint32_t prefill_target;
    uint16_t frame_size;

    if (!snapshot->config_valid)
    {
        return -RT_EINVAL;
    }
    prefill_target = app_share_calculate_prefill_target(&snapshot->sbc);
    if (prefill_target == 0U)
    {
        return -RT_EINVAL;
    }
    app_share_lock();
    generation = g_encoder_generation;
    app_share_unlock();
    /* Shared SBC encoder lock (not rt_enter_critical): encoding must not
     * stop the scheduler or the I2S RX thread cannot drain the driver
     * record pipe and PCM gets dropped mid-stream. */
    bt_avsrc_sbc_lock();
    bts2_sbc_encode_completed();
    frame_size = bts2_sbc_encode_cfg(snapshot->sbc.chnl_mode,
                                     snapshot->sbc.alloc_method,
                                     snapshot->sbc.sample_freq,
                                     snapshot->sbc.blocks,
                                     snapshot->sbc.subbands,
                                     snapshot->sbc.bit_pool);
    bt_avsrc_sbc_unlock();
    if ((frame_size == 0U) || (frame_size != snapshot->sbc.frmsize))
    {
        return -RT_ERROR;
    }

    g_pcm_used = 0U;
    g_active_config = snapshot->sbc;
    app_share_lock();
    if (generation != g_encoder_generation)
    {
        g_encoder_cleanup_pending = true;
        app_share_unlock();
        return -RT_EBUSY;
    }
    g_encoder_cleanup_pending = false;
    app_share_unlock();

    app_i2s_set_rx_enabled(true);
    app_share_lock();
    if (generation != g_encoder_generation)
    {
        g_encoder_cleanup_pending = true;
        app_share_unlock();
        app_i2s_set_rx_enabled(false);
        return -RT_EBUSY;
    }
    g_status.producer_active = true;
    g_status.config_valid = true;
    g_status.state = APP_A2DP_SHARE_STATE_PREFILLING;
    g_status.sample_rate = snapshot->sbc.sample_freq;
    g_status.sbc_frame_size = snapshot->sbc.frmsize;
    g_status.frames_per_packet = snapshot->sbc.frms_per_payload;
    g_status.pcm_bytes_per_packet = (uint16_t)snapshot->sbc.bytes_to_rd;
    g_status.prefill_target = prefill_target;
    g_stats.producer_starts++;
    g_status.kicked = false;
    app_share_unlock();
    g_prefill_start_tick = rt_tick_get();
    return RT_EOK;
}

static void app_share_stop_producer(void)
{
    bool cleanup_pending;
    bool was_active;

    app_share_lock();
    was_active = g_status.producer_active;
    cleanup_pending = g_encoder_cleanup_pending;
    g_status.producer_active = false;
    g_status.config_valid = false;
    g_encoder_cleanup_pending = false;
    g_music_gain_q15 = g_music_gain_target_q15;
    g_music_gain_ramp_frames = 0U;
    g_sync_encode_delay_us = 0;
    if (!g_status.desired &&
            (g_status.state != APP_A2DP_SHARE_STATE_ERROR))
    {
        g_status.state = APP_A2DP_SHARE_STATE_STOPPING;
    }
    app_share_unlock();
    if (!was_active && !cleanup_pending)
    {
        return;
    }

    app_i2s_set_rx_enabled(false);
    g_pcm_used = 0U;
    bt_avsrc_sbc_lock();
    bts2_sbc_encode_completed();
    bt_avsrc_sbc_unlock();
    if (was_active || cleanup_pending)
    {
        app_share_lock();
        g_stats.producer_stops++;
        app_share_unlock();
    }
}

static void app_share_invalidate_encoder(void)
{
    bool disable_rx;

    app_share_lock();
    disable_rx = g_status.producer_active;
    if (disable_rx)
    {
        g_status.producer_active = false;
        g_encoder_cleanup_pending = true;
    }
    g_status.config_valid = false;
    g_encoder_generation++;
    if (g_encoder_generation == 0U)
    {
        g_encoder_generation = 1U;
    }
    app_share_unlock();
    if (disable_rx)
    {
        app_i2s_set_rx_enabled(false);
    }
    app_share_lock();
    g_status.kicked = false;
    app_share_unlock();
}

static int app_share_encode_record(const uint8_t *pcm,
                                   const bts2_sbc_cfg *config,
                                   app_share_record_t *record)
{
    app_share_link_snapshot_t links;
    uint16_t source_offset = 0U;
    uint16_t destination_offset = APP_SHARE_PACKET_HEADER_SIZE;
    uint16_t source_remaining = (uint16_t)config->bytes_to_rd;
    uint16_t destination_remaining =
        (uint16_t)(AV_MTU_SIZE - APP_SHARE_PACKET_HEADER_SIZE);
    uint16_t frames = 0U;

    memset(record->packet, 0, APP_SHARE_PACKET_HEADER_SIZE);
    bt_avsrc_sbc_lock();
    app_share_collect_links(&links);
    if (!links.config_valid || !app_share_config_equal(config, &links.sbc))
    {
        bt_avsrc_sbc_unlock();
        return -RT_EBUSY;
    }
    while (source_remaining > 0U)
    {
        BTS2S_SBC_STREAM stream = {0};

        stream.psrc = (U8 *)&pcm[source_offset];
        stream.src_len = source_remaining;
        stream.pdst = &record->packet[destination_offset];
        stream.dst_len = destination_remaining;
        bts2_sbc_encode(&stream);
        if ((stream.src_len_used == 0U) || (stream.dst_len_used == 0U) ||
                (stream.src_len_used > source_remaining) ||
                (stream.dst_len_used > destination_remaining) ||
                ((stream.dst_len_used % config->frmsize) != 0U))
        {
            bt_avsrc_sbc_unlock();
            return -RT_ERROR;
        }

        source_offset += stream.src_len_used;
        source_remaining -= stream.src_len_used;
        destination_offset += stream.dst_len_used;
        destination_remaining -= stream.dst_len_used;
        frames += stream.dst_len_used / config->frmsize;
    }
    bt_avsrc_sbc_unlock();

    if ((frames == 0U) || (frames != config->frms_per_payload) ||
            (frames > UINT8_MAX))
    {
        return -RT_ERROR;
    }
    record->packet[APP_SHARE_RTP_HEADER_SIZE] = (uint8_t)frames;
    record->packet_length = destination_offset;
    return RT_EOK;
}

static void app_share_complete_stop_if_ready(void)
{
    app_share_link_snapshot_t links;
    app_a2dp_share_callback_t callback = RT_NULL;
    void *user_data = RT_NULL;
    uint32_t sequence = 0U;
    bool can_reset_locally = false;

    app_share_collect_links(&links);
    app_share_update_link_status(&links);

    app_share_lock();
    if (g_stop_callback_pending && !g_status.producer_active &&
            (links.streaming_count == 0U) &&
            (links.start_pending_count == 0U) &&
            (links.suspend_pending_count == 0U) &&
            !g_status.kicked && !g_pump_scheduled &&
            !g_encoder_cleanup_pending)
    {
        can_reset_locally = true;
    }
    app_share_unlock();

    if (can_reset_locally)
    {
        rt_mq_control(&g_record_queue, RT_IPC_CMD_RESET, RT_NULL);
        rt_enter_critical();
        rt_ringbuffer32_reset(&g_share_ring);
        rt_exit_critical();
        app_share_lock();
        g_status.ring_bytes = 0U;
        g_bts_quiesced = true;
        app_share_unlock();
    }

    app_share_lock();
    if (g_stop_callback_pending && g_bts_quiesced &&
            !g_status.producer_active &&
            !g_encoder_cleanup_pending &&
            (links.streaming_count == 0U) &&
            (links.start_pending_count == 0U) &&
            (links.suspend_pending_count == 0U))
    {
        g_stop_callback_pending = false;
        g_status.state = APP_A2DP_SHARE_STATE_IDLE;
        g_status.last_error = RT_EOK;
        g_status.config_valid = false;
        g_status.kicked = false;
        sequence = g_status.stop_sequence;
        callback = g_callback;
        user_data = g_callback_user_data;
    }
    app_share_unlock();

    if (callback != RT_NULL)
    {
        callback(APP_A2DP_SHARE_EVENT_STOPPED, sequence, RT_EOK,
                 user_data);
    }
}

static void app_share_handle_pending_callback(void)
{
    bool notify_started = false;

    app_share_lock();
    if (g_started_callback_pending && !g_started_callback_sent &&
            g_status.desired && g_status.kicked)
    {
        g_started_callback_pending = false;
        g_started_callback_sent = true;
        notify_started = true;
    }
    else if (!g_status.desired)
    {
        g_started_callback_pending = false;
    }
    app_share_unlock();

    if (notify_started)
    {
        app_share_notify(APP_A2DP_SHARE_EVENT_STARTED, 0U, RT_EOK);
    }
}

static void app_share_producer(void *parameter)
{
    (void)parameter;
    while (1)
    {
        app_share_link_snapshot_t links;
        rt_uint32_t events;
        bool desired;
        bool cleanup_pending;
        bool producer_active;
        rt_size_t read_length;
        rt_int32_t control_timeout;

        app_share_lock();
        control_timeout = (g_status.desired && g_status.producer_active) ?
                          0 :
                          rt_tick_from_millisecond(APP_SHARE_PUMP_WAIT_MS);
        app_share_unlock();
        rt_event_recv(&g_control_event, APP_SHARE_CONTROL_EVENT,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      control_timeout, &events);
        app_share_handle_pending_callback();
        app_share_collect_links(&links);
        app_share_update_link_status(&links);

        app_share_lock();
        desired = g_status.desired;
        producer_active = g_status.producer_active;
        cleanup_pending = g_encoder_cleanup_pending;
        app_share_unlock();

        if (!desired)
        {
            if (producer_active || cleanup_pending)
            {
                app_share_stop_producer();
            }
            app_a2dp_share_reconcile_streams();
            app_share_complete_stop_if_ready();
            continue;
        }

        if (!links.all_streaming)
        {
            if (producer_active || cleanup_pending)
            {
                app_share_stop_producer();
            }
            app_share_lock();
            if (g_status.desired &&
                    (g_status.state != APP_A2DP_SHARE_STATE_ERROR))
            {
                g_status.state = APP_A2DP_SHARE_STATE_STARTING;
            }
            app_share_unlock();
            continue;
        }
        if (!links.config_valid)
        {
            if (producer_active || cleanup_pending)
            {
                app_share_stop_producer();
            }
            app_share_fail(-RT_EINVAL);
            continue;
        }
        if (producer_active &&
                !app_share_config_equal(&g_active_config, &links.sbc))
        {
            app_share_stop_producer();
            app_share_fail(-RT_EINVAL);
            continue;
        }
        if (!producer_active)
        {
            int error = app_share_start_producer(&links);

            if (error != RT_EOK)
            {
                if (error == -RT_EBUSY)
                {
                    continue;
                }
                app_share_fail(error);
                continue;
            }
        }

        read_length = app_i2s_read(g_i2s_block, sizeof(g_i2s_block),
                                   APP_SHARE_PCM_READ_TIMEOUT_MS);
        if (read_length == 0U)
        {
            app_share_lock();
            g_stats.pcm_read_timeouts++;
            app_share_unlock();
            continue;
        }
        if ((g_pcm_used + read_length) > sizeof(g_pcm_buffer))
        {
            g_pcm_used = 0U;
            app_share_lock();
            g_stats.encode_errors++;
            app_share_unlock();
            continue;
        }
        app_share_apply_music_volume(g_i2s_block, read_length);
        memcpy(&g_pcm_buffer[g_pcm_used], g_i2s_block, read_length);
        g_pcm_used += read_length;
        app_share_lock();
        g_stats.pcm_bytes += read_length;
        app_share_unlock();

        while (g_pcm_used >= g_active_config.bytes_to_rd)
        {
            /* Two-sink sync pacing: briefly pause encoding to let the
             * slower sink catch up (or skip the pause to speed up). */
            if (g_sync_encode_delay_us > 0)
            {
                rt_thread_mdelay((g_sync_encode_delay_us + 999) / 1000);
            }
            int error = app_share_encode_record(g_pcm_buffer,
                                                &g_active_config,
                                                &g_producer_record);

            if (error != RT_EOK)
            {
                app_share_lock();
                g_stats.encode_errors++;
                app_share_unlock();
                app_share_stop_producer();
                if (error == -RT_EBUSY)
                {
                    break;
                }
                app_share_fail(error);
                break;
            }
            if (rt_mq_send(&g_record_queue, &g_producer_record,
                           sizeof(g_producer_record)) != RT_EOK)
            {
                app_share_lock();
                g_stats.queue_drops++;
                app_share_unlock();
            }
            else
            {
                app_share_lock();
                g_stats.sbc_frames += g_active_config.frms_per_payload;
                g_stats.packets_encoded++;
                g_stats.packet_bytes += g_producer_record.packet_length;
                app_share_unlock();
            }
            /* Move records into the share ring here instead of on the BT
             * task: during connect storms the BT task is busy and the ring
             * drained to "buffer empty" between pump runs. The producer
             * thread is the single ring writer (SPSC); the short critical
             * section in app_share_pump_records makes the put atomic
             * versus the BT task's get. */
            app_share_pump_records();

            g_pcm_used -= g_active_config.bytes_to_rd;
            if (g_pcm_used > 0U)
            {
                memmove(g_pcm_buffer,
                        &g_pcm_buffer[g_active_config.bytes_to_rd],
                        g_pcm_used);
            }
        }
    }
}

static void app_share_discard_queue(void)
{
    while (rt_mq_recv(&g_record_queue, &g_discard_record,
                      sizeof(g_discard_record), 0) == RT_EOK)
    {
        app_share_lock();
        g_stats.ring_drops++;
        app_share_unlock();
    }
}

static void app_share_pump_records(void)
{
    while (rt_ringbuffer32_space_len(&g_share_ring) >=
            sizeof(g_pump_record))
    {
        rt_size_t record_length;

        if (rt_mq_recv(&g_record_queue, &g_pump_record,
                       sizeof(g_pump_record), 0) != RT_EOK)
        {
            break;
        }
        if ((g_pump_record.packet_length < APP_SHARE_PACKET_HEADER_SIZE) ||
                (g_pump_record.packet_length > AV_MTU_SIZE))
        {
            app_share_lock();
            g_stats.ring_drops++;
            app_share_unlock();
            continue;
        }
        record_length = sizeof(g_pump_record.packet_length) +
                        g_pump_record.packet_length;
        /* The BT task reads the ring from a timer callback and can preempt
         * this (lower priority) thread mid-copy; keep the put atomic so the
         * ringbuffer32 wrap bookkeeping is never observed half-updated. */
        {
            rt_size_t put_length;
            rt_enter_critical();
            put_length = rt_ringbuffer32_put(&g_share_ring,
                                             (const uint8_t *)&g_pump_record,
                                             record_length);
            rt_exit_critical();
            if (put_length != record_length)
            {
                app_share_lock();
                g_stats.ring_drops++;
                app_share_unlock();
                break;
            }
        }
        app_share_lock();
        g_stats.records_pumped++;
        app_share_unlock();
    }
}

static void app_share_bts_pump(uint16_t message, void *parameter)
{
    app_share_link_snapshot_t links;
    bool desired;
    bool producer_active;
    bool kicked;
    uint32_t prefill_target;
    bool should_reschedule = false;
    uint32_t next_delay = APP_SHARE_PUMP_WAIT_MS;
    rt_size_t ring_bytes;

    (void)message;
    (void)parameter;
    app_share_lock();
    g_pump_scheduled = false;
    desired = g_status.desired;
    producer_active = g_status.producer_active;
    kicked = g_status.kicked;
    prefill_target = g_status.prefill_target;
    app_share_unlock();

    app_share_collect_links(&links);
    app_share_update_link_status(&links);
    if (desired && links.all_streaming)
    {
        app_share_wedge_check(&links);
        /* Records are pumped by the producer thread now (see
         * app_share_producer); only sample the ring level here. */
        ring_bytes = rt_ringbuffer32_data_len(&g_share_ring);
        app_share_lock();
        g_status.ring_bytes = ring_bytes;
        app_share_unlock();

        /* Forced two-sink resync: temporarily disabled — caused stream
         * state conflicts (suspend/start race) and ring overflow. */
        if (0 && (links.streaming_count >= 2U))
        {
            rt_tick_t now = rt_tick_get();
            if (g_resync_phase == 1U)
            {
                /* Phase 2: resume all streams after the pause. */
                uint8_t idx;
                bts2s_av_inst_data *avinst = bt_av_get_inst_data();

                g_resync_phase = 0U;
                g_resync_last_tick = now;
                if (avinst != RT_NULL)
                {
                    for (idx = 0U; idx < MAX_CONNS; idx++)
                    {
                        if ((avinst->con[idx].cfg == AV_AUDIO_SRC) &&
                                (avinst->con[idx].st == avconned_open))
                        {
                            bt_av_start_stream(idx);
                        }
                    }
                }
            }
            else if ((g_resync_last_tick != 0U) &&
                     ((now - g_resync_last_tick) >=
                      rt_tick_from_millisecond(APP_SHARE_RESYNC_INTERVAL_MS)))
            {
                /* Phase 1: suspend all streams to drain jitter buffers. */
                uint8_t idx;
                bts2s_av_inst_data *avinst = bt_av_get_inst_data();

                g_resync_phase = 1U;
                if (avinst != RT_NULL)
                {
                    for (idx = 0U; idx < MAX_CONNS; idx++)
                    {
                        if ((avinst->con[idx].cfg == AV_AUDIO_SRC) &&
                                (avinst->con[idx].st == avconned_streaming))
                        {
                            bt_av_suspend_stream(idx);
                        }
                    }
                }
                should_reschedule = true;
                next_delay = APP_SHARE_RESYNC_PAUSE_MS;
                goto resync_skip;
            }
        }
        else
        {
            g_resync_phase = 0U;
            g_resync_last_tick = 0U;
        }
resync_skip:

        /* Two-sink sync: steer the encode rate to hold the ring near
         * APP_SHARE_SYNC_RING_TARGET_MS of data. This couples the two
         * sinks' consumption rates and shrinks playback drift. Only
         * active with 2+ streaming sinks (single sink has nothing to
         * desync from). */
        if (links.streaming_count >= 2U)
        {
            uint32_t samples_per_packet =
                (uint32_t)links.sbc.blocks * links.sbc.subbands *
                links.sbc.frms_per_payload;
            uint32_t record_size =
                (uint32_t)sizeof(uint16_t) + APP_SHARE_PACKET_HEADER_SIZE +
                (uint32_t)links.sbc.frmsize * links.sbc.frms_per_payload;
            uint32_t target_bytes;
            uint32_t sample_freq = links.sbc.sample_freq ? links.sbc.sample_freq
                                   : APP_PCM_SAMPLE_RATE;

            if ((samples_per_packet > 0U) && (record_size > 0U))
            {
                uint32_t packets_in_target =
                    (sample_freq * APP_SHARE_SYNC_RING_TARGET_MS / 1000U +
                     samples_per_packet - 1U) / samples_per_packet;
                target_bytes = packets_in_target * record_size;
            }
            else
            {
                target_bytes = prefill_target;
            }
            if (target_bytes > 0U)
            {
                if ((ring_bytes + record_size) < target_bytes)
                {
                    /* Ring too low — the fast sink is draining us; slow
                     * the encoder so the slow sink can catch up. */
                    g_sync_encode_delay_us += (int32_t)APP_SHARE_SYNC_STEP_US;
                    if (g_sync_encode_delay_us > 2000)
                    {
                        g_sync_encode_delay_us = 2000;
                    }
                }
                else if (ring_bytes > (target_bytes + record_size * 4U))
                {
                    /* Ring too high — both sinks lag; speed up. */
                    g_sync_encode_delay_us -= (int32_t)APP_SHARE_SYNC_STEP_US;
                    if (g_sync_encode_delay_us < -1000)
                    {
                        g_sync_encode_delay_us = -1000;
                    }
                }
            }
        }
        else
        {
            g_sync_encode_delay_us = 0;
        }

        if (!kicked && links.config_valid && producer_active &&
                (prefill_target > 0U) &&
                ((ring_bytes >= prefill_target) ||
                 /* Force-kick after timeout: when a second sink joins and
                  * drains the ring, the buffer may never reach the prefill
                  * target — without this the share stays in PREFILLING
                  * forever and there is no audio. */
                 ((g_prefill_start_tick != 0U) &&
                  ((rt_tick_get() - g_prefill_start_tick) >=
                   rt_tick_from_millisecond(APP_SHARE_PREFILL_TIMEOUT_MS)))))
        {
            app_share_lock();
            g_status.kicked = true;
            g_status.state = APP_A2DP_SHARE_STATE_RUNNING;
            g_started_callback_pending = true;
            g_stats.kick_count++;
            app_share_unlock();
            bt_avsrc_sharing(&g_share_ring);
            ring_bytes = rt_ringbuffer32_data_len(&g_share_ring);
            app_share_lock();
            g_status.ring_bytes = ring_bytes;
            app_share_unlock();
            rt_event_send(&g_control_event, APP_SHARE_CONTROL_EVENT);
        }
        should_reschedule = true;
        next_delay = APP_SHARE_PUMP_ACTIVE_MS;
    }
    else if (!desired)
    {
        app_share_discard_queue();
        if (!producer_active && (links.streaming_count == 0U) &&
                (links.start_pending_count == 0U) &&
                (links.suspend_pending_count == 0U))
        {
            rt_tick_t now = rt_tick_get();

            rt_enter_critical();
            rt_ringbuffer32_reset(&g_share_ring);
            rt_exit_critical();
            app_share_lock();
            g_status.ring_bytes = 0U;
            if (g_stop_needs_settle)
            {
                if (g_stop_settle_deadline == 0U)
                {
                    g_stop_settle_deadline = now +
                        rt_tick_from_millisecond(APP_SHARE_STOP_SETTLE_MS);
                }
                if (app_share_tick_reached(now, g_stop_settle_deadline))
                {
                    g_stop_needs_settle = false;
                    g_status.kicked = false;
                    g_bts_quiesced = true;
                }
                else
                {
                    should_reschedule = true;
                }
            }
            else
            {
                g_status.kicked = false;
                g_bts_quiesced = true;
            }
            app_share_unlock();
            rt_event_send(&g_control_event, APP_SHARE_CONTROL_EVENT);
        }
        else
        {
            should_reschedule = true;
        }
    }
    else if (links.connected_count > 0U)
    {
        app_a2dp_share_reconcile_streams();
        should_reschedule = true;
    }
    else if (producer_active || kicked)
    {
        should_reschedule = true;
    }

    if (should_reschedule)
    {
        app_share_schedule_pump(next_delay);
    }
}

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data)
{
    rt_err_t error;

    if (g_status.initialized)
    {
        app_share_lock();
        g_callback = callback;
        g_callback_user_data = user_data;
        app_share_unlock();
        return RT_EOK;
    }
    if (!app_i2s_is_ready())
    {
        return -RT_ENOSYS;
    }

    memset(&g_status, 0, sizeof(g_status));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_active_config, 0, sizeof(g_active_config));
    g_music_volume_level = APP_VOLUME_LEVEL_MAX;
    g_music_gain_q15 = APP_SHARE_GAIN_UNITY_Q15;
    g_music_gain_target_q15 = APP_SHARE_GAIN_UNITY_Q15;
    g_music_gain_ramp_frames = 0U;
    error = rt_mutex_init(&g_share_lock, "a2dpsl", RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        return error;
    }
    error = rt_event_init(&g_control_event, "a2dpse", RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        rt_mutex_detach(&g_share_lock);
        return error;
    }
    error = rt_mq_init(&g_record_queue, "a2dpsq",
                       g_record_queue_pool, sizeof(app_share_record_t),
                       sizeof(g_record_queue_pool), RT_IPC_FLAG_FIFO);
    if (error != RT_EOK)
    {
        rt_event_detach(&g_control_event);
        rt_mutex_detach(&g_share_lock);
        return error;
    }
    rt_ringbuffer32_init(&g_share_ring, g_share_ring_pool,
                         sizeof(g_share_ring_pool));
    /* Priority 11: above the BT task (12) so pairing/connect storms cannot
     * starve the encoder (that starved the share ring and the media cadence)
     * but below the I2S RX thread (10) so pipe draining stays highest. */
    error = rt_thread_init(&g_producer_thread, "a2dps",
                           app_share_producer, RT_NULL,
                           g_producer_stack, sizeof(g_producer_stack),
                           RT_THREAD_PRIORITY_HIGH - 1,
                           RT_THREAD_TICK_DEFAULT);
    if (error != RT_EOK)
    {
        rt_mq_detach(&g_record_queue);
        rt_event_detach(&g_control_event);
        rt_mutex_detach(&g_share_lock);
        return error;
    }

    g_callback = callback;
    g_callback_user_data = user_data;
    g_bts_quiesced = true;
    g_status.initialized = true;
    g_status.state = APP_A2DP_SHARE_STATE_IDLE;
    g_status.ring_capacity = APP_SHARE_RING_SIZE;
    error = rt_thread_startup(&g_producer_thread);
    if (error != RT_EOK)
    {
        g_status.initialized = false;
        rt_thread_detach(&g_producer_thread);
        rt_mq_detach(&g_record_queue);
        rt_event_detach(&g_control_event);
        rt_mutex_detach(&g_share_lock);
        return error;
    }
    return RT_EOK;
}

int app_a2dp_share_start(void)
{
    if (!g_status.initialized)
    {
        return -RT_ENOSYS;
    }

    app_share_lock();
    if (g_status.desired)
    {
        app_share_unlock();
        return RT_EOK;
    }
    if ((g_status.state == APP_A2DP_SHARE_STATE_STOPPING) ||
            g_stop_callback_pending || !g_bts_quiesced)
    {
        app_share_unlock();
        return -RT_EBUSY;
    }
    rt_mq_control(&g_record_queue, RT_IPC_CMD_RESET, RT_NULL);
    rt_enter_critical();
    rt_ringbuffer32_reset(&g_share_ring);
    rt_exit_critical();
    g_status.desired = true;
    g_status.producer_active = false;
    g_status.config_valid = false;
    g_status.kicked = false;
    g_status.state = APP_A2DP_SHARE_STATE_STARTING;
    g_status.last_error = RT_EOK;
    g_status.ring_bytes = 0U;
    g_status.prefill_target = 0U;
    g_started_callback_pending = false;
    g_started_callback_sent = false;
    g_bts_quiesced = false;
    g_stop_needs_settle = false;
    g_stop_settle_deadline = 0U;
    g_stats.start_requests++;
    app_share_unlock();

    app_i2s_set_rx_enabled(false);
    app_a2dp_share_reconcile_streams();
    rt_event_send(&g_control_event, APP_SHARE_CONTROL_EVENT);
    return RT_EOK;
}

int app_a2dp_share_stop(uint32_t sequence)
{
    if (!g_status.initialized)
    {
        return -RT_ENOSYS;
    }

    app_share_lock();
    g_status.desired = false;
    g_status.state = APP_A2DP_SHARE_STATE_STOPPING;
    g_status.stop_sequence = sequence;
    g_started_callback_pending = false;
    g_stop_callback_pending = true;
    g_stop_needs_settle = g_status.kicked;
    g_stop_settle_deadline = 0U;
    g_bts_quiesced = !g_status.kicked && !g_pump_scheduled;
    g_stats.stop_requests++;
    app_share_unlock();

    app_i2s_set_rx_enabled(false);
    app_a2dp_share_reconcile_streams();
    rt_event_send(&g_control_event, APP_SHARE_CONTROL_EVENT);
    return RT_EOK;
}

int app_a2dp_share_set_volume(uint8_t level)
{
    int32_t target_gain;

    if (level > APP_VOLUME_LEVEL_MAX)
    {
        return -RT_EINVAL;
    }
    if (!g_status.initialized)
    {
        return -RT_ENOSYS;
    }

    target_gain = app_share_volume_gain(level);
    app_share_lock();
    g_music_volume_level = level;
    g_music_gain_target_q15 = target_gain;
    if (!g_status.producer_active ||
            (target_gain == g_music_gain_q15))
    {
        g_music_gain_q15 = target_gain;
        g_music_gain_ramp_frames = 0U;
    }
    else
    {
        g_music_gain_ramp_frames = APP_SHARE_VOLUME_RAMP_FRAMES;
    }
    app_share_unlock();
    return RT_EOK;
}

uint8_t app_a2dp_share_get_volume(void)
{
    uint8_t level;

    if (!g_status.initialized)
    {
        return APP_VOLUME_LEVEL_MAX;
    }
    app_share_lock();
    level = g_music_volume_level;
    app_share_unlock();
    return level;
}

void app_a2dp_share_on_a2dp_event(uint16_t event_id)
{
    bool active;
    bool invalidate = false;

    if (!g_status.initialized)
    {
        return;
    }
    switch (event_id)
    {
    case BT_NOTIFY_A2DP_PROFILE_CONNECTED:
    case BT_NOTIFY_A2DP_SUSPEND_IND:
    case BT_NOTIFY_A2DP_SUSPEND_CFM:
        break;
    case BT_NOTIFY_A2DP_PROFILE_DISCONNECTED:
        /* A sink left — the active SBC config may no longer be valid
         * for the remaining sinks, so the encoder must be re-checked. */
        invalidate = true;
        break;
    case BT_NOTIFY_A2DP_START_IND:
    case BT_NOTIFY_A2DP_START_CFM:
        /* Do NOT invalidate: a stream start (second headset joining) does
         * NOT change the SBC config. Invalidating tears down the running
         * producer, flushes the ring, and forces a full prefill restart —
         * killing audio for both headsets. Let the share continue from the
         * current ring position so both get audio without interruption. */
        break;
    default:
        return;
    }

    if (invalidate)
    {
        app_share_invalidate_encoder();
    }

    app_share_lock();
    active = g_status.desired || g_status.producer_active ||
             g_status.kicked || g_stop_callback_pending;
    app_share_unlock();
    if (active)
    {
        /* The notification runs on the BTS task; the timer preserves that
         * context for all sharing-ring writes and the initial sender kick. */
        app_share_schedule_pump(1U);
    }
    rt_event_send(&g_control_event, APP_SHARE_CONTROL_EVENT);
}

void app_a2dp_share_reconcile_streams(void)
{
    bts2s_av_inst_data *instance;
    bool desired;
    uint8_t index;

    if (!g_status.initialized)
    {
        return;
    }
    app_share_lock();
    desired = g_status.desired;
    app_share_unlock();

    instance = bt_av_get_inst_data();
    if (instance == RT_NULL)
    {
        return;
    }
    for (index = 0U; index < MAX_CONNS; index++)
    {
        bts2_av_conn *connection = &instance->con[index];

        if ((connection->cfg != AV_AUDIO_SRC) ||
                (connection->st <= avconned))
        {
            continue;
        }
        /* Never start a stream the application has not registered yet:
         * the AV layer reaches avconned_open before the app's A2DP
         * CONNECTED lands, and a start issued in that gap raced the
         * remote's own AVDTP setup — it then dropped the link (Anyka
         * closed AVDTP 60ms after our premature start) and the drop
         * cascaded into a wedged reconnect. The app kicks
         * app_a2dp_share_reconcile_streams() right after registering, so
         * the link streams a few ms later instead. */
        if (desired && (connection->st >= avconned_open))
        {
            uint8_t mac[6];

            bt_addr_convert(&connection->av_rmt_addr, mac);
            if (!app_bt_is_link_registered(mac))
            {
                continue;
            }
        }
        if (desired)
        {
            if ((connection->st == avconned_open) &&
                    !connection->start_pending &&
                    !connection->suspend_pending)
            {
                rt_tick_t now = rt_tick_get();
                if ((g_start_retry_tick[index] == 0U) ||
                    ((now - g_start_retry_tick[index]) >=
                     rt_tick_from_millisecond(APP_SHARE_START_RETRY_MS)))
                {
                    bt_av_start_stream(index);
                    g_start_retry_tick[index] = now;
                }
            }
            else if ((connection->st == avconned_streaming) &&
                     connection->suspend_pending &&
                     (connection->pending_cmd != A2DP_START_CMD))
            {
                bt_av_start_stream(index);
            }
            if (connection->st == avconned_streaming)
            {
                g_start_retry_tick[index] = 0U;
            }
        }
        else if (((connection->st == avconned_streaming) &&
                  !connection->suspend_pending) ||
                 (connection->start_pending &&
                  (connection->pending_cmd != A2DP_SUSPEND_CMD)))
        {
            bt_av_suspend_stream(index);
        }
    }
}

bool app_a2dp_share_is_stopped(void)
{
    bool stopped;

    if (!g_status.initialized)
    {
        return true;
    }
    app_share_lock();
    stopped = (g_status.state == APP_A2DP_SHARE_STATE_IDLE) &&
              !g_status.desired && !g_status.producer_active &&
              !g_status.kicked && !g_stop_callback_pending;
    app_share_unlock();
    return stopped;
}

void app_a2dp_share_get_status(app_a2dp_share_status_t *status)
{
    if (status == RT_NULL)
    {
        return;
    }
    if (!g_status.initialized)
    {
        memset(status, 0, sizeof(*status));
        status->state = APP_A2DP_SHARE_STATE_UNINITIALIZED;
        return;
    }
    app_share_lock();
    *status = g_status;
    app_share_unlock();
}

void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats)
{
    if (stats == RT_NULL)
    {
        return;
    }
    if (!g_status.initialized)
    {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    app_share_lock();
    *stats = g_stats;
    app_share_unlock();
}

#else

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data)
{
    (void)callback;
    (void)user_data;
    return -RT_ENOSYS;
}

int app_a2dp_share_start(void)
{
    return -RT_ENOSYS;
}

int app_a2dp_share_stop(uint32_t sequence)
{
    (void)sequence;
    return -RT_ENOSYS;
}

int app_a2dp_share_set_volume(uint8_t level)
{
    (void)level;
    return -RT_ENOSYS;
}

uint8_t app_a2dp_share_get_volume(void)
{
    return APP_VOLUME_LEVEL_MAX;
}

void app_a2dp_share_on_a2dp_event(uint16_t event_id)
{
    (void)event_id;
}

void app_a2dp_share_reconcile_streams(void)
{
}

bool app_a2dp_share_is_stopped(void)
{
    return true;
}

void app_a2dp_share_get_status(app_a2dp_share_status_t *status)
{
    if (status != RT_NULL)
    {
        memset(status, 0, sizeof(*status));
        status->state = APP_A2DP_SHARE_STATE_UNINITIALIZED;
    }
}

void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats)
{
    if (stats != RT_NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }
}

#endif

const char *app_a2dp_share_state_name(app_a2dp_share_state_t state)
{
    switch (state)
    {
    case APP_A2DP_SHARE_STATE_UNINITIALIZED:
        return "uninitialized";
    case APP_A2DP_SHARE_STATE_IDLE:
        return "idle";
    case APP_A2DP_SHARE_STATE_STARTING:
        return "starting";
    case APP_A2DP_SHARE_STATE_PREFILLING:
        return "prefilling";
    case APP_A2DP_SHARE_STATE_RUNNING:
        return "running";
    case APP_A2DP_SHARE_STATE_STOPPING:
        return "stopping";
    case APP_A2DP_SHARE_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
