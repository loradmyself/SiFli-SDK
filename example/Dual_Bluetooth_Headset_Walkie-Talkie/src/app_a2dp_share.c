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
#include "app_types.h"

#include "bts2_app_inc.h"
#include "ipc/ringbuffer32.h"

#if defined(CFG_AV_SRC) && defined(CFG_AV_SHARING)

#define APP_SHARE_PACKET_HEADER_SIZE 2U
#define APP_SHARE_MIN_MEDIA_SIZE (AV_FIXED_MEDIA_PKT_HDR_SIZE + 1U)
#define APP_SHARE_RING_SIZE (SINK_DATA_LIST_MAX_THRESHOLD * AV_MTU_SIZE)
#define APP_SHARE_PREFILL_BYTES \
    (SINK_DATA_LIST_START_THRESHOLD * AV_MTU_SIZE)
#define APP_SHARE_START_RETRY_MS 1500U
#define APP_SHARE_STOP_POLL_MS 20U
#define APP_SHARE_WEDGE_CHECK_MS 1000U
#define APP_SHARE_WEDGE_STALL_MS 3000U
#define APP_SHARE_WEDGE_BOUNCE_COOLDOWN_MS 30000U
#define APP_SHARE_WEDGE_BOUNCE_MAX 3U

typedef struct
{
    uint8_t connected_count;
    uint8_t streaming_count;
    uint8_t start_pending_count;
    uint8_t suspend_pending_count;
    bool config_valid;
    uint8_t media_con_active;
    uint8_t media_mac[MAX_CONNS][6];
    uint32_t media_req[MAX_CONNS];
    uint32_t media_nomem[MAX_CONNS];
    uint32_t media_qos[MAX_CONNS];
    uint8_t media_buff[MAX_CONNS];
} app_share_link_snapshot_t;

static struct rt_ringbuffer32 g_share_ring;
static rt_tick_t g_start_retry_tick[MAX_CONNS];
static rt_tick_t g_wedge_check_tick;
static rt_tick_t g_wedge_stall_tick[MAX_CONNS];
static rt_tick_t g_wedge_bounce_tick[MAX_CONNS];
static uint32_t g_wedge_sig[MAX_CONNS];
static uint8_t g_wedge_bounce_count[MAX_CONNS];
static bool g_wedge_qos_seen;
static bool g_stop_timer_scheduled;

ALIGN(4) static uint8_t g_share_ring_pool[APP_SHARE_RING_SIZE];
static app_a2dp_share_status_t g_status;
static app_a2dp_share_stats_t g_stats;
static app_a2dp_share_callback_t g_callback;
static void *g_callback_user_data;
static uint8_t g_music_volume_level = APP_VOLUME_LEVEL_MAX;

static void app_share_stop_timer(uint16_t message, void *parameter);
static void app_share_reconcile_links(void);
static void app_share_check_wedge(const app_share_link_snapshot_t *links);

static void app_share_notify(app_a2dp_share_event_t event,
                             uint32_t sequence, int result)
{
    app_a2dp_share_callback_t callback;
    void *user_data;

    rt_enter_critical();
    callback = g_callback;
    user_data = g_callback_user_data;
    rt_exit_critical();

    if (callback != RT_NULL)
    {
        callback(event, sequence, result, user_data);
    }
}

static void app_share_clear_ring(void)
{
    rt_enter_critical();
    rt_ringbuffer32_reset(&g_share_ring);
    rt_exit_critical();
    g_status.ring_bytes = 0U;
    g_status.kicked = false;
}

static bool app_share_tick_reached(rt_tick_t now, rt_tick_t deadline)
{
    return (rt_int32_t)(now - deadline) >= 0;
}

static void app_share_collect_links(app_share_link_snapshot_t *snapshot)
{
    bts2s_av_inst_data *instance;
    uint8_t first_config_valid = false;
    bts2_sbc_cfg first_config = {0};
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

        (void)bt_avsrc_get_media_stats(index,
                                      &snapshot->media_req[index],
                                      &snapshot->media_nomem[index],
                                      &snapshot->media_qos[index],
                                      &snapshot->media_buff[index]);
        if ((connection->cfg == AV_AUDIO_SRC) ||
                (snapshot->media_req[index] != 0U) ||
                (snapshot->media_qos[index] != 0U))
        {
            bt_addr_convert(&connection->av_rmt_addr,
                            snapshot->media_mac[index]);
            snapshot->media_con_active |= (uint8_t)(1U << index);
        }

        if (!connection->in_use || (connection->cfg != AV_AUDIO_SRC))
        {
            continue;
        }

        snapshot->connected_count++;
        if (connection->start_pending)
        {
            snapshot->start_pending_count++;
        }
        if (connection->suspend_pending)
        {
            snapshot->suspend_pending_count++;
        }
        if (connection->st == avconned_streaming)
        {
            snapshot->streaming_count++;
            if (!first_config_valid)
            {
                first_config = connection->act_cfg;
                first_config_valid = true;
            }
            else if (memcmp(&first_config, &connection->act_cfg,
                            sizeof(first_config)) != 0)
            {
                configs_match = false;
            }
        }
    }
    rt_hw_interrupt_enable(level);

    snapshot->config_valid = (snapshot->streaming_count > 0U) &&
                             first_config_valid && configs_match &&
                             (first_config.frmsize > 0U) &&
                             (first_config.frms_per_payload > 0U);
}

static void app_share_update_status(const app_share_link_snapshot_t *links)
{
    uint8_t copy_count;

    rt_enter_critical();
    g_status.connected_count = links->connected_count;
    g_status.streaming_count = links->streaming_count;
    g_status.start_pending_count = links->start_pending_count;
    g_status.suspend_pending_count = links->suspend_pending_count;
    g_status.config_valid = links->config_valid;
    g_status.media_con_count = MAX_CONNS > 4U ? 4U : MAX_CONNS;
    g_status.media_con_active = links->media_con_active;
    copy_count = g_status.media_con_count;
    memset(g_status.media_con_mac, 0, sizeof(g_status.media_con_mac));
    memcpy(g_status.media_con_mac, links->media_mac,
           sizeof(g_status.media_con_mac[0]) * copy_count);
    memcpy(g_status.media_req, links->media_req,
           sizeof(g_status.media_req[0]) * copy_count);
    memcpy(g_status.media_nomem, links->media_nomem,
           sizeof(g_status.media_nomem[0]) * copy_count);
    memcpy(g_status.media_qos, links->media_qos,
           sizeof(g_status.media_qos[0]) * copy_count);
    memcpy(g_status.media_buff, links->media_buff,
           sizeof(g_status.media_buff[0]) * copy_count);
    g_status.ring_bytes = rt_ringbuffer32_data_len(&g_share_ring);
    rt_exit_critical();
}

static void app_share_schedule_stop_poll(void)
{
    if (!g_stop_timer_scheduled)
    {
        g_stop_timer_scheduled = true;
        bts2_timer_ev_add(APP_SHARE_STOP_POLL_MS, app_share_stop_timer,
                          0U, RT_NULL);
    }
}

static void app_share_complete_stop_if_ready(void)
{
    app_share_link_snapshot_t links;
    bool complete = false;
    uint32_t sequence;

    app_share_collect_links(&links);
    app_share_update_status(&links);

    rt_enter_critical();
    if (!g_status.desired && (links.streaming_count == 0U) &&
            (links.start_pending_count == 0U) &&
            (links.suspend_pending_count == 0U))
    {
        complete = true;
    }
    rt_exit_critical();

    if (!complete)
    {
        app_share_schedule_stop_poll();
        return;
    }

    app_share_clear_ring();
    rt_enter_critical();
    g_status.producer_active = false;
    g_status.state = APP_A2DP_SHARE_STATE_IDLE;
    g_status.last_error = RT_EOK;
    sequence = g_status.stop_sequence;
    rt_exit_critical();
    app_share_notify(APP_A2DP_SHARE_EVENT_STOPPED, sequence, RT_EOK);
}

static void app_share_stop_timer(uint16_t message, void *parameter)
{
    (void)message;
    (void)parameter;
    g_stop_timer_scheduled = false;
    app_share_complete_stop_if_ready();
}

static void app_share_reconcile_links(void)
{
    app_share_link_snapshot_t links;
    bts2s_av_inst_data *instance;
    rt_tick_t now = rt_tick_get();
    uint8_t index;

    app_share_collect_links(&links);
    app_share_update_status(&links);
    instance = bt_av_get_inst_data();
    if (instance == RT_NULL)
    {
        return;
    }

    for (index = 0U; index < MAX_CONNS; index++)
    {
        bts2_av_conn *connection = &instance->con[index];
        bt_notify_device_mac_t mac = {0};
        bool desired;

        if (!connection->in_use || (connection->cfg != AV_AUDIO_SRC))
        {
            continue;
        }

        (void)bt_addr_convert(&connection->av_rmt_addr, mac.addr);
        rt_enter_critical();
        desired = g_status.desired && g_status.phone_connected;
        rt_exit_critical();

        if (desired && (connection->st == avconned_open) &&
                app_bt_is_link_registered(mac.addr) &&
                ((g_start_retry_tick[index] == 0U) ||
                 app_share_tick_reached(
                     now, g_start_retry_tick[index] +
                     rt_tick_from_millisecond(APP_SHARE_START_RETRY_MS))))
        {
            g_start_retry_tick[index] = now;
            bt_av_start_stream(index);
            rt_enter_critical();
            g_stats.start_requests++;
            rt_exit_critical();
        }
        else if (!desired && (connection->st == avconned_streaming) &&
                 !connection->suspend_pending &&
                 (connection->pending_cmd != A2DP_SUSPEND_CMD))
        {
            (void)bt_av_suspend_stream(index);
        }

        if (connection->st == avconned_streaming)
        {
            g_start_retry_tick[index] = 0U;
        }
    }

    rt_enter_critical();
    if (g_status.desired && g_status.phone_connected &&
            (g_status.streaming_count > 0U) &&
            (rt_ringbuffer32_data_len(&g_share_ring) >=
             APP_SHARE_PREFILL_BYTES) &&
            !g_status.kicked)
    {
        g_status.kicked = true;
        g_status.state = APP_A2DP_SHARE_STATE_RUNNING;
        g_stats.kick_count++;
        rt_exit_critical();
        (void)bt_avsrc_sharing(&g_share_ring);
    }
    else
    {
        rt_exit_critical();
    }
}

static void app_share_check_wedge(const app_share_link_snapshot_t *links)
{
    rt_tick_t now = rt_tick_get();
    bool progress = false;
    bool qos_seen = false;
    uint8_t index;

    if ((now - g_wedge_check_tick) <
            rt_tick_from_millisecond(APP_SHARE_WEDGE_CHECK_MS))
    {
        return;
    }
    g_wedge_check_tick = now;

    for (index = 0U; index < MAX_CONNS; index++)
    {
        if (links->media_qos[index] != 0U)
        {
            qos_seen = true;
            break;
        }
    }
    if (qos_seen != g_wedge_qos_seen)
    {
        g_wedge_qos_seen = qos_seen;
        for (index = 0U; index < MAX_CONNS; index++)
        {
            g_wedge_sig[index] = qos_seen ? links->media_qos[index] :
                                 links->media_req[index];
            g_wedge_stall_tick[index] = 0U;
        }
        return;
    }

    for (index = 0U; index < MAX_CONNS; index++)
    {
        uint32_t signature = qos_seen ? links->media_qos[index] :
                             links->media_req[index];

        if ((links->media_con_active & (uint8_t)(1U << index)) != 0U &&
                (signature > g_wedge_sig[index]))
        {
            progress = true;
        }
    }

    for (index = 0U; index < MAX_CONNS; index++)
    {
        uint32_t signature = qos_seen ? links->media_qos[index] :
                             links->media_req[index];
        bool stalled = false;

        if ((links->media_con_active & (uint8_t)(1U << index)) == 0U)
        {
            g_wedge_stall_tick[index] = 0U;
            continue;
        }
        if (signature > g_wedge_sig[index])
        {
            g_wedge_stall_tick[index] = 0U;
        }
        else if (progress)
        {
            if (g_wedge_stall_tick[index] == 0U)
            {
                g_wedge_stall_tick[index] = now;
            }
            else if ((now - g_wedge_stall_tick[index]) >=
                     rt_tick_from_millisecond(APP_SHARE_WEDGE_STALL_MS))
            {
                stalled = true;
            }
        }

        if (stalled && (g_wedge_bounce_count[index] <
                        APP_SHARE_WEDGE_BOUNCE_MAX) &&
                ((g_wedge_bounce_tick[index] == 0U) ||
                 ((now - g_wedge_bounce_tick[index]) >=
                  rt_tick_from_millisecond(
                      APP_SHARE_WEDGE_BOUNCE_COOLDOWN_MS))))
        {
            uint8_t mac[6];

            memcpy(mac, links->media_mac[index], sizeof(mac));
            g_wedge_bounce_count[index]++;
            g_wedge_bounce_tick[index] = now;
            g_wedge_stall_tick[index] = 0U;
            rt_enter_critical();
            g_stats.bounce_count++;
            rt_exit_critical();
            rt_kprintf("[intercom] wedged A2DP forwarding link %u\n", index);
            (void)app_bt_bounce_media_link(mac);
        }
    }

    for (index = 0U; index < MAX_CONNS; index++)
    {
        g_wedge_sig[index] = qos_seen ? links->media_qos[index] :
                             links->media_req[index];
    }
}

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data)
{
    if (g_status.initialized)
    {
        return RT_EOK;
    }

    memset(&g_status, 0, sizeof(g_status));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_start_retry_tick, 0, sizeof(g_start_retry_tick));
    rt_ringbuffer32_init(&g_share_ring, g_share_ring_pool,
                         sizeof(g_share_ring_pool));
    g_callback = callback;
    g_callback_user_data = user_data;
    g_status.initialized = true;
    g_status.state = APP_A2DP_SHARE_STATE_IDLE;
    g_status.ring_capacity = sizeof(g_share_ring_pool);
    g_status.prefill_target = APP_SHARE_PREFILL_BYTES;
    return RT_EOK;
}

int app_a2dp_share_start(void)
{
    if (!g_status.initialized)
    {
        return -RT_ENOSYS;
    }

    rt_enter_critical();
    g_stats.start_requests++;
    if (!g_status.desired)
    {
        g_status.desired = true;
        g_status.state = APP_A2DP_SHARE_STATE_PREFILLING;
        g_status.last_error = RT_EOK;
    }
    rt_exit_critical();

    app_a2dp_share_reconcile_streams();
    return RT_EOK;
}

int app_a2dp_share_stop(uint32_t sequence)
{
    bool notify_now;

    if (!g_status.initialized)
    {
        return -RT_ENOSYS;
    }

    rt_enter_critical();
    g_stats.stop_requests++;
    g_status.desired = false;
    g_status.kicked = false;
    g_status.stop_sequence = sequence;
    g_status.state = APP_A2DP_SHARE_STATE_STOPPING;
    rt_exit_critical();

    app_share_clear_ring();
    app_a2dp_share_reconcile_streams();

    notify_now = true;
    for (uint8_t index = 0U; index < MAX_CONNS; index++)
    {
        bts2s_av_inst_data *instance = bt_av_get_inst_data();

        if ((instance != RT_NULL) &&
                instance->con[index].in_use &&
                (instance->con[index].cfg == AV_AUDIO_SRC) &&
                ((instance->con[index].st == avconned_streaming) ||
                 instance->con[index].start_pending))
        {
            notify_now = false;
            break;
        }
    }

    if (notify_now)
    {
        rt_enter_critical();
        g_status.producer_active = false;
        g_status.state = APP_A2DP_SHARE_STATE_IDLE;
        rt_exit_critical();
        app_share_notify(APP_A2DP_SHARE_EVENT_STOPPED, sequence, RT_EOK);
    }
    else
    {
        app_share_schedule_stop_poll();
    }
    return RT_EOK;
}

int app_a2dp_share_set_volume(uint8_t level)
{
    if (level > APP_VOLUME_LEVEL_MAX)
    {
        return -RT_EINVAL;
    }
    g_music_volume_level = level;
    return RT_EOK;
}

uint8_t app_a2dp_share_get_volume(void)
{
    return g_music_volume_level;
}

void app_a2dp_share_set_phone_connected(bool connected)
{
    bool changed;

    rt_enter_critical();
    changed = (g_status.phone_connected != connected);
    g_status.phone_connected = connected;
    if (!connected)
    {
        g_status.phone_streaming = false;
    }
    rt_exit_critical();

    if (changed && !connected)
    {
        app_share_clear_ring();
        app_a2dp_share_reconcile_streams();
    }
}

void app_a2dp_share_set_phone_streaming(bool streaming)
{
    rt_enter_critical();
    g_status.phone_streaming = streaming;
    if (!streaming)
    {
        g_status.kicked = false;
    }
    rt_exit_critical();

    if (!streaming)
    {
        app_share_clear_ring();
    }
    app_a2dp_share_reconcile_streams();
}

void app_a2dp_share_on_media_data(const bt_notify_a2dp_data_t *media)
{
    app_share_link_snapshot_t links;
    uint16_t packet_length;
    rt_size_t put_length;
    bool relay = false;

    if ((media == RT_NULL) || (media->data == RT_NULL) ||
            (media->data_length < APP_SHARE_MIN_MEDIA_SIZE) ||
            (media->data_length > AV_MTU_SIZE))
    {
        rt_enter_critical();
        g_stats.invalid_packets++;
        rt_exit_critical();
        return;
    }

    app_share_collect_links(&links);
    app_share_update_status(&links);
    app_share_check_wedge(&links);

    rt_enter_critical();
    if (!g_status.initialized || !g_status.desired ||
            !g_status.phone_connected || !g_status.phone_streaming ||
            (links.streaming_count == 0U))
    {
        rt_exit_critical();
        return;
    }
    if (rt_ringbuffer32_space_len(&g_share_ring) <
            (sizeof(packet_length) + media->data_length))
    {
        g_stats.ring_drops++;
        rt_exit_critical();
        return;
    }

    packet_length = media->data_length;
    (void)rt_ringbuffer32_put(&g_share_ring,
                             (const uint8_t *)&packet_length,
                             sizeof(packet_length));
    put_length = rt_ringbuffer32_put(&g_share_ring, media->data,
                                     media->data_length);
    if (put_length != media->data_length)
    {
        g_stats.ring_drops++;
        rt_exit_critical();
        return;
    }

    g_stats.phone_packets++;
    g_stats.packet_bytes += media->data_length;
    g_status.ring_bytes = rt_ringbuffer32_data_len(&g_share_ring);
    g_status.producer_active = true;
    if (!g_status.kicked &&
            (g_status.ring_bytes >= APP_SHARE_PREFILL_BYTES))
    {
        g_status.kicked = true;
        g_status.state = APP_A2DP_SHARE_STATE_RUNNING;
        g_stats.kick_count++;
        relay = true;
    }
    rt_exit_critical();

    if (relay)
    {
        (void)bt_avsrc_sharing(&g_share_ring);
    }
}

void app_a2dp_share_on_a2dp_event(uint16_t event_id)
{
    switch (event_id)
    {
    case BT_NOTIFY_A2DP_PROFILE_CONNECTED:
    case BT_NOTIFY_A2DP_START_IND:
    case BT_NOTIFY_A2DP_START_CFM:
        app_a2dp_share_reconcile_streams();
        break;
    case BT_NOTIFY_A2DP_SUSPEND_IND:
    case BT_NOTIFY_A2DP_SUSPEND_CFM:
    case BT_NOTIFY_A2DP_PROFILE_DISCONNECTED:
        app_share_clear_ring();
        app_a2dp_share_reconcile_streams();
        break;
    default:
        break;
    }
}

void app_a2dp_share_reconcile_streams(void)
{
    if (!g_status.initialized)
    {
        return;
    }
    app_share_reconcile_links();
}

bool app_a2dp_share_is_stopped(void)
{
    bool stopped;

    rt_enter_critical();
    stopped = !g_status.desired && !g_status.producer_active &&
              (g_status.state != APP_A2DP_SHARE_STATE_STOPPING);
    rt_exit_critical();
    return stopped;
}

void app_a2dp_share_get_status(app_a2dp_share_status_t *status)
{
    if (status == RT_NULL)
    {
        return;
    }
    rt_enter_critical();
    *status = g_status;
    status->ring_bytes = rt_ringbuffer32_data_len(&g_share_ring);
    rt_exit_critical();
}

void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats)
{
    if (stats == RT_NULL)
    {
        return;
    }
    rt_enter_critical();
    *stats = g_stats;
    rt_exit_critical();
}

const char *app_a2dp_share_state_name(app_a2dp_share_state_t state)
{
    switch (state)
    {
    case APP_A2DP_SHARE_STATE_UNINITIALIZED:
        return "uninitialized";
    case APP_A2DP_SHARE_STATE_IDLE:
        return "idle";
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

#else

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data)
{
    (void)callback;
    (void)user_data;
    return -RT_ENOSYS;
}
int app_a2dp_share_start(void) { return -RT_ENOSYS; }
int app_a2dp_share_stop(uint32_t sequence) { (void)sequence; return -RT_ENOSYS; }
int app_a2dp_share_set_volume(uint8_t level) { (void)level; return -RT_ENOSYS; }
uint8_t app_a2dp_share_get_volume(void) { return 0U; }
void app_a2dp_share_set_phone_connected(bool connected) { (void)connected; }
void app_a2dp_share_set_phone_streaming(bool streaming) { (void)streaming; }
void app_a2dp_share_on_media_data(const bt_notify_a2dp_data_t *media) { (void)media; }
void app_a2dp_share_on_a2dp_event(uint16_t event_id) { (void)event_id; }
void app_a2dp_share_reconcile_streams(void) { }
bool app_a2dp_share_is_stopped(void) { return true; }
void app_a2dp_share_get_status(app_a2dp_share_status_t *status)
{
    if (status != RT_NULL) memset(status, 0, sizeof(*status));
}
void app_a2dp_share_get_stats(app_a2dp_share_stats_t *stats)
{
    if (stats != RT_NULL) memset(stats, 0, sizeof(*stats));
}
const char *app_a2dp_share_state_name(app_a2dp_share_state_t state)
{
    (void)state;
    return "disabled";
}

#endif
