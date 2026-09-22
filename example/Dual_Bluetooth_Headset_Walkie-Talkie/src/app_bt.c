/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <rtthread.h>

#include "bf0_sibles.h"
#include "bt_connection_manager.h"
#include "bts2_app_hfp_ag.h"
#include "bts2_app_inc.h"
#include "hfp_audio_api.h"

#include "app_a2dp_share.h"
#include "app_board.h"
#include "app_bt.h"
#include "app_i2s.h"
#include "app_state.h"

#define APP_BT_MAX_DISCOVERED 20U
#define APP_BT_NAME_LENGTH 61U
#define APP_BT_EVENT_QUEUE_DEPTH 48U
#define APP_BT_READY_TIMEOUT_MS 8000U
#define APP_BT_CONNECT_TIMEOUT_MS 10000U
#define APP_BT_DISCONNECT_TIMEOUT_MS 3000U
#define APP_BT_DISCONNECT_MAX_ATTEMPTS 2U
#define APP_BT_MEDIA_CLOSE_TIMEOUT_MS 7000U
#define APP_BT_TALK_START_TIMEOUT_MS 5000U
#define APP_BT_TALK_STOP_TIMEOUT_MS 3000U
#define APP_BT_SCO_EVENT_SETTLE_MS 100U
#define APP_BT_INTERNAL_EVENT_TYPE 0xFFFFU
#define APP_HFP_CVSD_AIR_MODE 2U
#define APP_HFP_MSBC_AIR_MODE 3U
#define APP_HFP_RELAY_PACKET_SIZE 60U
#define APP_BT_SCO_RETIRED_HANDLE_COUNT 8U
#define APP_A2DP_STREAM_PLAYING 1U
#define APP_BT_DEFAULT_TALK_VOLUME 8U
#define APP_BT_AVRCP_VOLUME_RETRY_MS 100U
#define APP_BT_AVRCP_VOLUME_MAX_RETRIES 20U

typedef enum
{
    APP_BT_CMD_SCAN = 1,
    APP_BT_CMD_CONNECT,
    APP_BT_CMD_DISCONNECT,
    APP_BT_CMD_TALK_START,
    APP_BT_CMD_TALK_STOP,
    APP_BT_CMD_SET_VOLUME,
    APP_BT_CMD_TALK_TIMEOUT,
    APP_BT_CMD_CONNECT_TIMEOUT,
    APP_BT_CMD_DISCONNECT_TIMEOUT,
    APP_BT_CMD_AVRCP_VOLUME_RETRY,
    APP_BT_EVENT_MEDIA_CLOSED,
    APP_BT_EVENT_MEDIA_ERROR,
} app_bt_internal_event_id_t;

typedef enum
{
    APP_BT_CONN_IDLE = 0,
    APP_BT_CONN_MEDIA_STOP_PENDING,
    APP_BT_CONN_A2DP_PENDING,
    APP_BT_CONN_HFP_PENDING,
    APP_BT_CONN_READY,
    APP_BT_CONN_ABORTING,
} app_bt_connection_stage_t;

typedef enum
{
    APP_BT_SCO_IDLE = 0,
    APP_BT_SCO_CONNECT_PENDING,
    APP_BT_SCO_CANCEL_PENDING,
    APP_BT_SCO_ACTIVE,
    APP_BT_SCO_CLOSE_PENDING,
} app_bt_sco_phase_t;

typedef struct
{
    bool present;
    bool relay_registered;
    bt_device_sco_conn_para_t para;
} app_bt_sco_link_t;

typedef struct
{
    int8_t slot;
    uint32_t generation;
} app_bt_timer_identity_t;

typedef struct
{
    bt_notify_device_mac_t mac;
    uint32_t dev_cls;
    int rssi;
    char name[APP_BT_NAME_LENGTH];
} app_bt_discovery_t;

typedef struct
{
    bool used;
    bool release_pending;
    bool disconnect_watchdog_active;
    bool disconnect_blocks_media;
    bool profile_cleanup_pending;
    bool acl_connected;
    bool a2dp_connected;
    bool hfp_connected;
    bool avrcp_connected;
    bool avrcp_connect_requested;
    bt_notify_device_mac_t mac;
    char name[APP_BT_NAME_LENGTH];
    uint16_t hfp_channel;
    bt_err_t avrcp_role_result;
    uint8_t talk_volume_level;
    uint8_t remote_music_volume_level;
    uint8_t remote_music_volume_desired;
    uint8_t remote_music_volume_retries;
    bool remote_music_volume_valid;
    bool remote_music_volume_pending;
    uint8_t disconnect_attempts;
    app_bt_connection_stage_t connection_stage;
    uint32_t connection_generation;
    uint32_t hfp_session_generation;
    uint32_t media_stop_sequence;
    bool restore_media_after_connect;
    volatile uint32_t disconnect_generation;
    app_bt_sco_phase_t sco_phase;
    bool sco_requires_acl_barrier;
    uint32_t sco_generation;
    uint32_t sco_connection_generation;
    uint32_t sco_hfp_session_generation;
    uint16_t sco_request_channel;
    app_bt_sco_link_t sco_primary;
    app_bt_sco_link_t sco_orphan;
    uint16_t retired_sco_handles[APP_BT_SCO_RETIRED_HANDLE_COUNT];
    uint8_t retired_sco_cursor;
} app_bt_slot_t;

typedef struct
{
    uint16_t type;
    uint16_t id;
    union
    {
        app_bt_discovery_t discovery;
        bt_notify_device_acl_conn_info_t acl_connected;
        bt_notify_device_base_info_t acl_disconnected;
        bt_notify_profile_state_info_t profile;
        bt_notify_device_sco_info_t sco;
        struct
        {
            bt_notify_device_mac_t mac;
            char name[APP_BT_NAME_LENGTH];
        } command_device;
        struct
        {
            app_volume_target_t target;
            uint8_t level;
            bt_notify_device_mac_t mac;
            uint32_t connection_generation;
        } volume;
        struct
        {
            bt_notify_profile_state_info_t profile;
            bt_err_t role_result;
        } avrcp;
        uint8_t avrcp_reported_volume;
        uint32_t token;
        app_bt_timer_identity_t timer;
    } data;
} app_bt_event_t;

static rt_mq_t g_bt_event_queue;
static rt_mailbox_t g_bt_ready_mailbox;
static rt_mutex_t g_bt_lock;
static rt_timer_t g_talk_timer;
static rt_timer_t g_connect_timer;
static rt_timer_t g_avrcp_volume_timer;
static rt_timer_t g_disconnect_timers[APP_HEADSET_COUNT];
static uint8_t g_disconnect_timer_slot_ids[APP_HEADSET_COUNT];
static app_bt_discovery_t g_discovered[APP_BT_MAX_DISCOVERED];
static app_bt_slot_t g_slots[APP_HEADSET_COUNT];
static uint8_t g_discovered_count;
static int8_t g_connecting_slot = -1;
static volatile app_bt_timer_identity_t g_connect_timer_arm = {-1, 0U};
static uint32_t g_next_connection_generation;
static uint32_t g_next_sco_generation;
static volatile bool g_bt_ready;
static bool g_avrcp_ready;
static volatile bool g_bt_initialized;
static volatile bool g_scanning;
static bool g_restore_media;
static bool g_talk_waiting_for_media;
static bool g_talk_stop_settling;
static bool g_talk_recovery_attempted;
static bool g_error_cleanup_reported;
static bool g_pending_disconnect;
static bt_notify_device_mac_t g_pending_disconnect_mac;
static volatile uint32_t g_talk_timer_token;
static uint32_t g_talk_media_sequence;
static volatile uint32_t g_media_stop_sequence;
static volatile bool g_media_desired;
static uint8_t g_talk_volume_level;
static bool g_avrcp_reported_volume_valid;
static bool g_avrcp_reported_source_known;
static uint8_t g_avrcp_reported_volume_level;
static bt_notify_device_mac_t g_avrcp_reported_mac;

static int app_bt_post_simple_event(uint16_t event_id, uint32_t token);
static int app_bt_post_timer_event(uint16_t event_id, int8_t slot,
                                   uint32_t generation);
static int app_bt_post_command(uint16_t command,
                               const bt_notify_device_mac_t *mac,
                               const char *name);
static int app_bt_post_volume_command(app_volume_target_t target,
                                      const bt_notify_device_mac_t *mac,
                                      uint32_t connection_generation,
                                      uint8_t level);
static void app_bt_abort_connection(int slot_index);

static bool app_bt_mac_equal(const bt_notify_device_mac_t *left,
                             const bt_notify_device_mac_t *right)
{
    return memcmp(left->addr, right->addr, sizeof(left->addr)) == 0;
}

static void app_bt_print_mac(const bt_notify_device_mac_t *mac)
{
    rt_kprintf("%02X:%02X:%02X:%02X:%02X:%02X",
               mac->addr[0], mac->addr[1], mac->addr[2],
               mac->addr[3], mac->addr[4], mac->addr[5]);
}

static const char *app_bt_sco_codec_name(uint8_t air_mode)
{
    switch (air_mode)
    {
    case APP_HFP_CVSD_AIR_MODE:
        return "CVSD";
    case APP_HFP_MSBC_AIR_MODE:
        return "mSBC";
    default:
        return "unknown";
    }
}

static uint32_t app_bt_sco_sample_rate(uint8_t air_mode)
{
    return (air_mode == APP_HFP_MSBC_AIR_MODE) ? 16000U : 8000U;
}

static const char *app_bt_sco_phase_name(app_bt_sco_phase_t phase)
{
    switch (phase)
    {
    case APP_BT_SCO_IDLE:
        return "idle";
    case APP_BT_SCO_CONNECT_PENDING:
        return "connect-pending";
    case APP_BT_SCO_CANCEL_PENDING:
        return "cancel-pending";
    case APP_BT_SCO_ACTIVE:
        return "active";
    case APP_BT_SCO_CLOSE_PENDING:
        return "close-pending";
    default:
        return "unknown";
    }
}

static bool app_bt_sco_has_handle(const app_bt_slot_t *slot)
{
    return slot->sco_primary.present;
}

static bool app_bt_sco_has_live_handle(const app_bt_slot_t *slot)
{
    return slot->sco_primary.present || slot->sco_orphan.present;
}

static bool app_bt_sco_is_pending(const app_bt_slot_t *slot)
{
    return (slot->sco_phase != APP_BT_SCO_IDLE) ||
           app_bt_sco_has_live_handle(slot) ||
           slot->sco_requires_acl_barrier;
}

static bool app_bt_all_sco_quiescent(void)
{
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (app_bt_sco_is_pending(&g_slots[index]) ||
                g_slots[index].sco_primary.relay_registered ||
                g_slots[index].sco_orphan.relay_registered)
        {
            return false;
        }
    }
    return true;
}

static uint32_t app_bt_next_sco_generation(void)
{
    g_next_sco_generation++;
    if (g_next_sco_generation == 0U)
    {
        g_next_sco_generation++;
    }
    return g_next_sco_generation;
}

static bool app_bt_sco_handle_is_retired(const app_bt_slot_t *slot,
                                         uint16_t handle)
{
    uint8_t index;

    if (handle == 0U)
    {
        return false;
    }
    for (index = 0U; index < APP_BT_SCO_RETIRED_HANDLE_COUNT; index++)
    {
        if (slot->retired_sco_handles[index] == handle)
        {
            return true;
        }
    }
    return false;
}

static void app_bt_sco_unretire_handle(app_bt_slot_t *slot, uint16_t handle)
{
    uint8_t index;

    for (index = 0U; index < APP_BT_SCO_RETIRED_HANDLE_COUNT; index++)
    {
        if (slot->retired_sco_handles[index] == handle)
        {
            slot->retired_sco_handles[index] = 0U;
        }
    }
}

static void app_bt_sco_retire_handle(app_bt_slot_t *slot, uint16_t handle)
{
    if ((handle == 0U) || app_bt_sco_handle_is_retired(slot, handle))
    {
        return;
    }

    slot->retired_sco_handles[slot->retired_sco_cursor] = handle;
    slot->retired_sco_cursor =
        (uint8_t)((slot->retired_sco_cursor + 1U) %
                  APP_BT_SCO_RETIRED_HANDLE_COUNT);
}

static void app_bt_sco_mark_idle(app_bt_slot_t *slot, uint16_t closed_handle)
{
    app_bt_sco_retire_handle(slot, closed_handle);
    if (slot->sco_primary.present)
    {
        app_bt_sco_retire_handle(slot, slot->sco_primary.para.sco_hdl);
    }
    if (slot->sco_orphan.present)
    {
        app_bt_sco_retire_handle(slot, slot->sco_orphan.para.sco_hdl);
    }
    slot->sco_phase = APP_BT_SCO_IDLE;
    slot->sco_requires_acl_barrier = false;
    slot->sco_request_channel = 0U;
    slot->sco_connection_generation = 0U;
    slot->sco_hfp_session_generation = 0U;
    memset(&slot->sco_primary, 0, sizeof(slot->sco_primary));
    memset(&slot->sco_orphan, 0, sizeof(slot->sco_orphan));
}

static int app_bt_sco_relay_path(const bt_device_sco_conn_para_t *parameter)
{
    uint8_t path_id = (uint8_t)(parameter->sco_hdl >> 8);
    uint8_t transport_tag = (uint8_t)((parameter->sco_hdl >> 4) & 0x0FU);
    uint8_t link_id = (uint8_t)(parameter->sco_hdl & 0x0FU);

    if ((path_id == 0U) || (path_id > APP_HEADSET_COUNT) ||
            (transport_tag != 8U) || (link_id > 4U))
    {
        return -1;
    }
    return (int)path_id - 1;
}

static bool app_bt_sco_link_supported(uint8_t index)
{
    const bt_device_sco_conn_para_t *parameter =
        &g_slots[index].sco_primary.para;

    if ((parameter->air_mode != APP_HFP_CVSD_AIR_MODE) &&
            (parameter->air_mode != APP_HFP_MSBC_AIR_MODE))
    {
        rt_kprintf("[intercom] headset %u SCO rejected: codec=%s air=%u is not supported\n",
                   index + 1U, app_bt_sco_codec_name(parameter->air_mode),
                   parameter->air_mode);
        return false;
    }
    if ((parameter->rx_pkt_len != APP_HFP_RELAY_PACKET_SIZE) ||
            (parameter->tx_pkt_len != APP_HFP_RELAY_PACKET_SIZE))
    {
        rt_kprintf("[intercom] headset %u SCO rejected: rx=%u tx=%u; relay requires 60/60 bytes\n",
                   index + 1U, parameter->rx_pkt_len, parameter->tx_pkt_len);
        return false;
    }
    if (app_bt_sco_relay_path(parameter) < 0)
    {
        rt_kprintf("[intercom] headset %u SCO rejected: handle=0x%04X has no relay path\n",
                   index + 1U, parameter->sco_hdl);
        return false;
    }
    return true;
}

static bool app_bt_sco_pair_supported(void)
{
    const bt_device_sco_conn_para_t *first = &g_slots[0].sco_primary.para;
    const bt_device_sco_conn_para_t *second = &g_slots[1].sco_primary.para;
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (!app_bt_sco_link_supported(index))
        {
            return false;
        }
    }
    /* The SDK relay has no 8 kHz/16 kHz resampler between SCO paths. */
    if (first->air_mode != second->air_mode)
    {
        rt_kprintf("[intercom] SCO pair rejected: headset1=%s/%u headset2=%s/%u; mixed sample rates are unsupported\n",
                   app_bt_sco_codec_name(first->air_mode), first->air_mode,
                   app_bt_sco_codec_name(second->air_mode), second->air_mode);
        return false;
    }
    if (first->tx_intvl != second->tx_intvl)
    {
        rt_kprintf("[intercom] SCO pair rejected: headset1 interval=%u headset2 interval=%u; relay requires equal packet pacing\n",
                   first->tx_intvl, second->tx_intvl);
        return false;
    }
    if (app_bt_sco_relay_path(first) == app_bt_sco_relay_path(second))
    {
        rt_kprintf("[intercom] SCO pair rejected: handles 0x%04X and 0x%04X map to the same relay path\n",
                   first->sco_hdl, second->sco_hdl);
        return false;
    }
    return true;
}

static int app_bt_hex_value(char value)
{
    if ((value >= '0') && (value <= '9'))
    {
        return value - '0';
    }
    if ((value >= 'a') && (value <= 'f'))
    {
        return value - 'a' + 10;
    }
    if ((value >= 'A') && (value <= 'F'))
    {
        return value - 'A' + 10;
    }
    return -1;
}

static bool app_bt_parse_mac(const char *text, bt_notify_device_mac_t *mac)
{
    uint8_t index;

    if ((text == RT_NULL) || (mac == RT_NULL) || (strlen(text) != 17U))
    {
        return false;
    }

    for (index = 0; index < 6U; index++)
    {
        int high = app_bt_hex_value(text[index * 3U]);
        int low = app_bt_hex_value(text[index * 3U + 1U]);

        if ((high < 0) || (low < 0))
        {
            return false;
        }
        if ((index < 5U) && (text[index * 3U + 2U] != ':') &&
                (text[index * 3U + 2U] != '-'))
        {
            return false;
        }
        mac->addr[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool app_bt_parse_index(const char *text, uint8_t *index)
{
    uint32_t value = 0U;

    if ((text == RT_NULL) || (*text == '\0'))
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
        if (value > 255U)
        {
            return false;
        }
        text++;
    }

    if (value == 0U)
    {
        return false;
    }
    *index = (uint8_t)value;
    return true;
}

static int app_bt_find_slot_by_mac(const bt_notify_device_mac_t *mac)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].used && app_bt_mac_equal(&g_slots[index].mac, mac))
        {
            return index;
        }
    }
    return -1;
}

static int app_bt_find_slot_by_channel(uint16_t channel)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].used && g_slots[index].hfp_connected &&
                (g_slots[index].hfp_channel == channel))
        {
            return index;
        }
    }
    return -1;
}

static int app_bt_find_free_slot(void)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (!g_slots[index].used)
        {
            return index;
        }
    }
    return -1;
}

static uint8_t app_bt_a2dp_count(void)
{
    uint8_t index;
    uint8_t count = 0U;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].a2dp_connected)
        {
            count++;
        }
    }
    return count;
}

static uint8_t app_bt_sco_count(void)
{
    uint8_t index;
    uint8_t count = 0U;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if ((g_slots[index].sco_phase == APP_BT_SCO_ACTIVE) &&
                g_slots[index].sco_primary.present)
        {
            count++;
        }
    }
    return count;
}

static bool app_bt_is_talk_mode(app_mode_t mode)
{
    return (mode == APP_MODE_TALK_STARTING) ||
           (mode == APP_MODE_TALKING) ||
           (mode == APP_MODE_TALK_STOPPING);
}

static bool app_bt_connection_pending(const app_bt_slot_t *slot)
{
    return (slot->connection_stage == APP_BT_CONN_MEDIA_STOP_PENDING) ||
           (slot->connection_stage == APP_BT_CONN_A2DP_PENDING) ||
           (slot->connection_stage == APP_BT_CONN_HFP_PENDING);
}

static uint32_t app_bt_next_connection_generation(void)
{
    g_next_connection_generation++;
    if (g_next_connection_generation == 0U)
    {
        g_next_connection_generation++;
    }
    return g_next_connection_generation;
}

static bool app_bt_slot_has_link(const app_bt_slot_t *slot)
{
    return slot->acl_connected || slot->a2dp_connected ||
           slot->hfp_connected || slot->avrcp_connected ||
           app_bt_sco_is_pending(slot);
}

static void app_bt_clear_slot(int slot_index)
{
    uint32_t disconnect_generation =
        g_slots[slot_index].disconnect_generation;

    memset(&g_slots[slot_index], 0, sizeof(g_slots[slot_index]));
    g_slots[slot_index].disconnect_generation = disconnect_generation;
    g_slots[slot_index].talk_volume_level = g_talk_volume_level;
    g_slots[slot_index].remote_music_volume_level = APP_VOLUME_LEVEL_MAX;
    g_slots[slot_index].avrcp_role_result = BT_ERROR_DISCONNECTED;
}

static bool app_bt_has_connecting_slot(void)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (app_bt_connection_pending(&g_slots[index]))
        {
            return true;
        }
    }
    return false;
}

static void app_bt_update_activity_state(void)
{
    app_mode_t mode = app_state_get_mode();

    if (app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR))
    {
        return;
    }
    if (g_scanning)
    {
        app_state_set_mode(APP_MODE_SCANNING);
    }
    else if (app_bt_has_connecting_slot())
    {
        app_state_set_mode(APP_MODE_CONNECTING);
    }
    else if (app_bt_a2dp_count() != 0U)
    {
        app_state_set_mode(APP_MODE_MEDIA);
    }
    else
    {
        app_state_set_mode(APP_MODE_IDLE);
    }
}

static void app_bt_refresh_leds(void)
{
    uint8_t index;
    bool connected[APP_HEADSET_COUNT];

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        connected[index] = g_slots[index].a2dp_connected &&
                           g_slots[index].hfp_connected;
    }

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        app_board_set_headset_connected(index, connected[index]);
    }
}

static void app_bt_set_media_desired(bool desired)
{
    int result;

    g_media_desired = desired;
    result = desired ? app_a2dp_share_start() :
             app_a2dp_share_stop(g_media_stop_sequence);
    if ((result != RT_EOK) && (result != -RT_EBUSY))
    {
        if (desired)
        {
            g_media_desired = false;
        }
        rt_kprintf("[intercom] A2DP sharing %s request failed: %d\n",
                   desired ? "start" : "stop", result);
    }
}

static void app_bt_update_media_for_links(void)
{
    app_mode_t mode = app_state_get_mode();
    uint8_t index;

    if ((mode == APP_MODE_TALK_STARTING) || (mode == APP_MODE_TALKING) ||
            (mode == APP_MODE_TALK_STOPPING))
    {
        return;
    }
    if (mode == APP_MODE_ERROR)
    {
        app_bt_set_media_desired(false);
        return;
    }
    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].restore_media_after_connect &&
                app_bt_connection_pending(&g_slots[index]))
        {
            app_bt_set_media_desired(false);
            return;
        }
        if (g_slots[index].disconnect_blocks_media)
        {
            app_bt_set_media_desired(false);
            return;
        }
    }
    app_bt_set_media_desired(app_bt_a2dp_count() != 0U);
    app_bt_update_activity_state();
}

static uint32_t app_bt_request_media_stop(void)
{
    g_media_stop_sequence++;
    if (g_media_stop_sequence == 0U)
    {
        g_media_stop_sequence++;
    }
    app_bt_set_media_desired(false);
    return g_media_stop_sequence;
}

static const char *app_bt_media_state_name(void)
{
    app_a2dp_share_status_t status;

    app_a2dp_share_get_status(&status);
    return app_a2dp_share_state_name(status.state);
}

static bool app_bt_media_active(void)
{
    return g_media_desired || !app_a2dp_share_is_stopped();
}

static void app_bt_share_callback(app_a2dp_share_event_t event,
                                  uint32_t sequence, int result,
                                  void *user_data)
{
    (void)user_data;
    if (event == APP_A2DP_SHARE_EVENT_STOPPED)
    {
        if (app_bt_post_simple_event(APP_BT_EVENT_MEDIA_CLOSED,
                                     sequence) != RT_EOK)
        {
            rt_kprintf("[intercom] failed to queue A2DP sharing stop\n");
        }
    }
    else if (event == APP_A2DP_SHARE_EVENT_ERROR)
    {
        if (app_bt_post_simple_event(APP_BT_EVENT_MEDIA_ERROR,
                                     (uint32_t)result) != RT_EOK)
        {
            rt_kprintf("[intercom] failed to queue A2DP sharing error: %d\n",
                       result);
        }
    }
}

static void app_bt_set_call_state(const app_bt_slot_t *slot, bool active)
{
    HFP_CALL_INFO_T call = {0};

    call.mux_id = (uint8_t)slot->hfp_channel;
    call.num_active = active ? 1U : 0U;
    call.num_held = 0U;
    call.callsetup_state = 0U;
    bt_interface_phone_state_changed(&call);
}

static bt_err_t app_bt_switch_sco_for_mac(const bt_notify_device_mac_t *mac,
                                          bool connect)
{
    BTS2S_BD_ADDR address;

    if (bt_addr_convert_to_bts((bd_addr_t *)mac, &address) != 0U)
    {
        return BT_ERROR_INPARAM;
    }

    if (connect)
    {
        bt_hfp_connect_audio(&address);
    }
    else
    {
        bt_hfp_disconnect_audio(&address);
    }
    return BT_EOK;
}

static bt_err_t app_bt_switch_sco(const app_bt_slot_t *slot, bool connect)
{
    return app_bt_switch_sco_for_mac(&slot->mac, connect);
}

static void app_bt_talk_timer_start(uint32_t timeout_ms)
{
    rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);

    rt_timer_stop(g_talk_timer);
    g_talk_timer_token++;
    rt_timer_control(g_talk_timer, RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(g_talk_timer);
}

static void app_bt_talk_timer_cancel(void)
{
    rt_timer_stop(g_talk_timer);
    g_talk_timer_token++;
}

static void app_bt_connect_timer_start(int slot_index, uint32_t timeout_ms)
{
    rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);

    rt_timer_stop(g_connect_timer);
    g_connect_timer_arm.slot = (int8_t)slot_index;
    g_connect_timer_arm.generation =
        g_slots[slot_index].connection_generation;
    g_connecting_slot = (int8_t)slot_index;
    rt_timer_control(g_connect_timer, RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(g_connect_timer);
}

static void app_bt_connect_timer_stop(int slot_index)
{
    if (g_connecting_slot == slot_index)
    {
        rt_timer_stop(g_connect_timer);
        g_connecting_slot = -1;
        g_connect_timer_arm.slot = -1;
        g_connect_timer_arm.generation = 0U;
    }
}

static void app_bt_cancel_deferred_connect(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bool restore_media = slot->restore_media_after_connect;

    app_bt_connect_timer_stop(slot_index);
    if (restore_media && app_bt_is_talk_mode(app_state_get_mode()))
    {
        g_restore_media = true;
    }
    if (app_bt_slot_has_link(slot))
    {
        slot->restore_media_after_connect = false;
        slot->connection_stage = APP_BT_CONN_ABORTING;
        slot->release_pending = true;
        slot->disconnect_blocks_media = restore_media;
        if (app_bt_is_talk_mode(app_state_get_mode()))
        {
            slot->profile_cleanup_pending = true;
        }
        app_bt_abort_connection(slot_index);
        return;
    }
    app_bt_clear_slot(slot_index);
    app_bt_refresh_leds();
    if (restore_media)
    {
        app_bt_update_media_for_links();
    }
    else
    {
        app_bt_update_activity_state();
    }
}

static int app_bt_begin_a2dp_connect(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bt_err_t error;

    app_bt_connect_timer_stop(slot_index);
    slot->connection_generation = app_bt_next_connection_generation();
    slot->connection_stage = APP_BT_CONN_A2DP_PENDING;
    bt_interface_set_scan_mode(0U, 0U);
    error = bt_interface_conn_to_source_ext(slot->mac.addr, BT_PROFILE_A2DP);
    if (error != BT_EOK)
    {
        rt_kprintf("[intercom] A2DP connect request failed: %d\n", (int)error);
        app_bt_cancel_deferred_connect(slot_index);
        return -RT_ERROR;
    }

    app_bt_connect_timer_start(slot_index, APP_BT_CONNECT_TIMEOUT_MS);
    app_state_set_mode(APP_MODE_CONNECTING);
    rt_kprintf("[intercom] headset %u A2DP connect requested: ",
               slot_index + 1U);
    app_bt_print_mac(&slot->mac);
    rt_kprintf("\n");
    return RT_EOK;
}

static void app_bt_disconnect_timer_start(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    rt_tick_t ticks = rt_tick_from_millisecond(APP_BT_DISCONNECT_TIMEOUT_MS);

    rt_timer_stop(g_disconnect_timers[slot_index]);
    slot->disconnect_generation++;
    if (slot->disconnect_generation == 0U)
    {
        slot->disconnect_generation++;
    }
    slot->disconnect_watchdog_active = true;
    rt_timer_control(g_disconnect_timers[slot_index], RT_TIMER_CTRL_SET_TIME,
                     &ticks);
    rt_timer_start(g_disconnect_timers[slot_index]);
}

static void app_bt_disconnect_timer_cancel(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];

    rt_timer_stop(g_disconnect_timers[slot_index]);
    slot->disconnect_watchdog_active = false;
    slot->disconnect_attempts = 0U;
    slot->disconnect_generation++;
}

static int app_bt_issue_disconnect(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    int error;

    slot->disconnect_attempts++;
    error = bt_interface_disconnect_req(slot->mac.addr);
    app_bt_disconnect_timer_start(slot_index);
    return error;
}

static void app_bt_abort_connection(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];

    if (slot->connection_stage == APP_BT_CONN_MEDIA_STOP_PENDING)
    {
        app_bt_cancel_deferred_connect(slot_index);
        return;
    }

    slot->connection_stage = APP_BT_CONN_ABORTING;
    app_bt_connect_timer_stop(slot_index);
    if (app_bt_slot_has_link(slot))
    {
        if (!slot->disconnect_watchdog_active ||
                (slot->disconnect_attempts == 0U))
        {
            if (slot->disconnect_watchdog_active)
            {
                app_bt_disconnect_timer_cancel(slot_index);
            }
            slot->disconnect_attempts = 0U;
            if (app_bt_issue_disconnect(slot_index) != 0)
            {
                rt_kprintf("[intercom] headset %u rollback request failed; retry scheduled\n",
                           slot_index + 1U);
            }
        }
    }
    else
    {
        slot->release_pending = true;
        if (bt_interface_cancel_connect_req(slot->mac.addr) != 0)
        {
            rt_kprintf("[intercom] headset %u connect cancel request failed\n",
                       slot_index + 1U);
        }
        slot->disconnect_attempts = 0U;
        app_bt_disconnect_timer_start(slot_index);
    }
    app_bt_update_activity_state();
}

static void app_bt_unregister_slot_relay(app_bt_slot_t *slot)
{
    if (slot->sco_primary.relay_registered)
    {
        hfp_audio_relay_option(&slot->sco_primary.para, 0);
        slot->sco_primary.relay_registered = false;
    }
    if (slot->sco_orphan.relay_registered)
    {
        hfp_audio_relay_option(&slot->sco_orphan.para, 0);
        slot->sco_orphan.relay_registered = false;
    }
}

static void app_bt_unregister_relays(void)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_unregister_slot_relay(&g_slots[index]);
    }
}

static void app_bt_report_error_cleanup(void)
{
    app_bt_set_media_desired(false);
    if (app_bt_all_sco_quiescent() && !g_error_cleanup_reported)
    {
        g_error_cleanup_reported = true;
        rt_kprintf("[intercom] error cleanup complete; reset is required\n");
    }
}

static void app_bt_complete_talk_stop(void)
{
    bool restore;
    bool disconnect_pending;
    bool cleanup_slots[APP_HEADSET_COUNT] = {false};
    int disconnect_slot = -1;
    bt_notify_device_mac_t disconnect_mac = {0};
    uint8_t index;

    if ((app_state_get_mode() != APP_MODE_TALK_STOPPING) ||
            !app_bt_all_sco_quiescent())
    {
        return;
    }

    app_bt_talk_timer_cancel();
    g_talk_waiting_for_media = false;
    g_talk_stop_settling = false;
    g_talk_recovery_attempted = false;
    restore = g_restore_media && (app_bt_a2dp_count() != 0U);
    g_restore_media = false;
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        cleanup_slots[index] = g_slots[index].profile_cleanup_pending;
        if (cleanup_slots[index])
        {
            restore = false;
        }
    }
    disconnect_pending = g_pending_disconnect;
    if (disconnect_pending)
    {
        disconnect_mac = g_pending_disconnect_mac;
        g_pending_disconnect = false;
        restore = false;
        disconnect_slot = app_bt_find_slot_by_mac(&disconnect_mac);
        if (disconnect_slot >= 0)
        {
            g_slots[disconnect_slot].disconnect_blocks_media = true;
        }
    }

    app_bt_set_media_desired(restore);
    app_state_set_mode(APP_MODE_IDLE);
    app_bt_update_activity_state();

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (!cleanup_slots[index])
        {
            continue;
        }
        g_slots[index].profile_cleanup_pending = false;
        if (app_bt_slot_has_link(&g_slots[index]))
        {
            app_bt_abort_connection(index);
        }
        else
        {
            g_slots[index].connection_stage = APP_BT_CONN_IDLE;
            g_slots[index].disconnect_blocks_media = false;
        }
    }

    if (disconnect_pending && (disconnect_slot < 0))
    {
        app_bt_update_media_for_links();
    }
    else if (disconnect_pending &&
             (app_bt_post_command(APP_BT_CMD_DISCONNECT,
                                  &disconnect_mac, RT_NULL) != RT_EOK))
    {
        rt_kprintf("[intercom] failed to queue deferred disconnect\n");
        if (!cleanup_slots[disconnect_slot])
        {
            g_slots[disconnect_slot].disconnect_blocks_media = false;
        }
        app_bt_update_media_for_links();
    }
    else if (cleanup_slots[0] || cleanup_slots[1])
    {
        app_bt_update_media_for_links();
    }
}

static void app_bt_schedule_talk_stop_completion(void)
{
    if ((app_state_get_mode() != APP_MODE_TALK_STOPPING) ||
            !app_bt_all_sco_quiescent())
    {
        return;
    }

    /* Drain duplicate SCO notifications before a new talk transaction starts. */
    g_talk_stop_settling = true;
    app_bt_talk_timer_start(APP_BT_SCO_EVENT_SETTLE_MS);
}

static void app_bt_begin_talk_stop(void)
{
    uint8_t index;

    if ((app_state_get_mode() == APP_MODE_TALK_STOPPING) ||
            (app_state_get_mode() == APP_MODE_ERROR))
    {
        return;
    }

    app_state_set_mode(APP_MODE_TALK_STOPPING);
    app_bt_talk_timer_cancel();
    g_talk_waiting_for_media = false;
    g_talk_stop_settling = false;
    g_talk_recovery_attempted = false;
    app_bt_unregister_relays();

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (!g_slots[index].used)
        {
            continue;
        }
        if (g_slots[index].connection_stage ==
                APP_BT_CONN_MEDIA_STOP_PENDING)
        {
            rt_kprintf("[intercom] cancelling deferred headset %u connection before SCO cleanup\n",
                       index + 1U);
            app_bt_cancel_deferred_connect(index);
        }
        else if (app_bt_connection_pending(&g_slots[index]))
        {
            app_bt_connect_timer_stop(index);
            g_slots[index].connection_stage = APP_BT_CONN_ABORTING;
            g_slots[index].profile_cleanup_pending = true;
            g_slots[index].disconnect_blocks_media = true;
        }
    }

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].hfp_connected)
        {
            app_bt_set_call_state(&g_slots[index], false);
        }
        if (!g_slots[index].used)
        {
            continue;
        }

        if (g_slots[index].sco_phase == APP_BT_SCO_CONNECT_PENDING)
        {
            g_slots[index].sco_phase = APP_BT_SCO_CANCEL_PENDING;
        }
        else if (g_slots[index].sco_phase == APP_BT_SCO_ACTIVE)
        {
            g_slots[index].sco_phase = APP_BT_SCO_CLOSE_PENDING;
        }

        if ((g_slots[index].sco_phase == APP_BT_SCO_CANCEL_PENDING) ||
                (g_slots[index].sco_phase == APP_BT_SCO_CLOSE_PENDING))
        {
            bt_err_t error = app_bt_switch_sco(&g_slots[index], false);

            if (error != BT_EOK)
            {
                rt_kprintf("[intercom] SCO close request failed for headset %u: %d\n",
                           index + 1U, (int)error);
            }
        }
    }

    if (app_bt_all_sco_quiescent())
    {
        app_bt_schedule_talk_stop_completion();
    }
    else
    {
        app_bt_talk_timer_start(APP_BT_TALK_STOP_TIMEOUT_MS);
    }
}

static void app_bt_request_sco_start(void)
{
    uint8_t index;

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_slot_t *slot = &g_slots[index];
        bt_err_t error;

        slot->sco_phase = APP_BT_SCO_CONNECT_PENDING;
        slot->sco_generation = app_bt_next_sco_generation();
        slot->sco_connection_generation = slot->connection_generation;
        slot->sco_hfp_session_generation = slot->hfp_session_generation;
        slot->sco_request_channel = slot->hfp_channel;
        slot->sco_requires_acl_barrier = false;
        memset(&slot->sco_primary, 0, sizeof(slot->sco_primary));
        memset(&slot->sco_orphan, 0, sizeof(slot->sco_orphan));

        app_bt_set_call_state(slot, true);
        bt_interface_spk_vol_change_req(slot->hfp_channel,
                                        slot->talk_volume_level);
        error = app_bt_switch_sco(slot, true);
        if (error != BT_EOK)
        {
            rt_kprintf("[intercom] SCO request failed for headset %u: %d\n",
                       index + 1U, (int)error);
            app_bt_sco_mark_idle(slot, 0U);
            app_bt_begin_talk_stop();
            return;
        }
    }

    rt_kprintf("[intercom] two SCO links requested; waiting for HFP codec negotiation\n");
    app_bt_talk_timer_start(APP_BT_TALK_START_TIMEOUT_MS);
}

static void app_bt_start_talk(void)
{
    uint8_t index;
    bool ready = true;
    app_mode_t mode = app_state_get_mode();

    if ((mode != APP_MODE_MEDIA) || g_scanning ||
            (g_connecting_slot >= 0))
    {
        rt_kprintf("[intercom] talk command rejected in state %s\n",
                   app_state_mode_name(mode));
        return;
    }

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (!g_slots[index].used || !g_slots[index].a2dp_connected ||
                !g_slots[index].hfp_connected ||
                app_bt_sco_is_pending(&g_slots[index]) ||
                (g_slots[index].connection_stage != APP_BT_CONN_READY))
        {
            ready = false;
        }
    }
    if (!ready || !app_bt_all_sco_quiescent())
    {
        rt_kprintf("[intercom] talk requires two A2DP + HFP headsets with no active SCO\n");
        return;
    }

    g_restore_media = g_media_desired || app_bt_media_active();
    g_talk_recovery_attempted = false;
    g_error_cleanup_reported = false;
    g_talk_stop_settling = false;
    app_state_set_mode(APP_MODE_TALK_STARTING);
    g_talk_waiting_for_media = true;
    g_talk_media_sequence = app_bt_request_media_stop();
    app_bt_talk_timer_start(APP_BT_MEDIA_CLOSE_TIMEOUT_MS);
    rt_kprintf("[intercom] waiting for both A2DP streams to stop\n");
}

static void app_bt_handle_media_closed(uint32_t sequence)
{
    uint8_t slot_index;

    for (slot_index = 0U; slot_index < APP_HEADSET_COUNT; slot_index++)
    {
        app_bt_slot_t *slot = &g_slots[slot_index];

        if (slot->used &&
                (slot->connection_stage == APP_BT_CONN_MEDIA_STOP_PENDING) &&
                (slot->media_stop_sequence == sequence))
        {
            app_mode_t mode = app_state_get_mode();

            if ((mode != APP_MODE_CONNECTING) ||
                    (g_connecting_slot != (int8_t)slot_index) ||
                    !app_bt_all_sco_quiescent() || g_media_desired ||
                    !app_a2dp_share_is_stopped())
            {
                rt_kprintf("[intercom] deferred headset %u connection cancelled in state %s\n",
                           slot_index + 1U, app_state_mode_name(mode));
                app_bt_cancel_deferred_connect(slot_index);
                return;
            }
            slot->media_stop_sequence = 0U;
            app_bt_begin_a2dp_connect(slot_index);
            return;
        }
    }

    if ((app_state_get_mode() == APP_MODE_TALK_STARTING) &&
            g_talk_waiting_for_media &&
            (sequence == g_talk_media_sequence))
    {
        uint8_t index;

        app_bt_talk_timer_cancel();
        g_talk_waiting_for_media = false;
        for (index = 0; index < APP_HEADSET_COUNT; index++)
        {
            if (g_slots[index].a2dp_connected &&
                    (bt_interface_get_a2dp_stream_state(&g_slots[index].mac) ==
                     APP_A2DP_STREAM_PLAYING))
            {
                rt_kprintf("[intercom] headset %u did not confirm A2DP suspend; talk aborted\n",
                           index + 1U);
                app_bt_set_media_desired(g_restore_media);
                g_restore_media = false;
                app_state_set_mode(APP_MODE_IDLE);
                app_bt_update_activity_state();
                return;
            }
        }
        app_bt_request_sco_start();
        return;
    }

    if (g_media_desired)
    {
        /* A start requested during STOPPING returns EBUSY; retry on STOPPED. */
        app_bt_set_media_desired(true);
    }
}

static void app_bt_recover_stuck_sco(void)
{
    uint8_t index;

    g_talk_stop_settling = false;
    app_bt_unregister_relays();
    app_bt_set_media_desired(false);
    if (app_bt_all_sco_quiescent())
    {
        app_bt_schedule_talk_stop_completion();
        return;
    }
    if (!g_talk_recovery_attempted)
    {
        g_talk_recovery_attempted = true;
        rt_kprintf("[intercom] SCO close timeout; disconnecting affected ACL links\n");
        for (index = 0; index < APP_HEADSET_COUNT; index++)
        {
            if (g_slots[index].used &&
                    app_bt_sco_is_pending(&g_slots[index]))
            {
                g_slots[index].sco_requires_acl_barrier =
                    g_slots[index].acl_connected;
                if (g_slots[index].hfp_connected)
                {
                    app_bt_set_call_state(&g_slots[index], false);
                }
                if (bt_interface_disconnect_req(g_slots[index].mac.addr) != 0)
                {
                    rt_kprintf("[intercom] ACL recovery request failed for headset %u\n",
                               index + 1U);
                }
            }
        }
        app_bt_talk_timer_start(APP_BT_TALK_STOP_TIMEOUT_MS);
        return;
    }

    app_bt_talk_timer_cancel();
    g_restore_media = false;
    if (g_pending_disconnect)
    {
        g_pending_disconnect = false;
        rt_kprintf("[intercom] deferred disconnect cancelled by terminal SCO error\n");
    }
    g_error_cleanup_reported = false;
    app_state_set_mode(APP_MODE_ERROR);
    rt_kprintf("[intercom] SCO state is still unresolved; media remains stopped\n");
    app_bt_report_error_cleanup();
}

static void app_bt_try_start_relay(void)
{
    uint8_t index;

    if ((app_state_get_mode() != APP_MODE_TALK_STARTING) ||
            (app_bt_sco_count() != APP_HEADSET_COUNT))
    {
        return;
    }

    if (!app_bt_sco_pair_supported())
    {
        rt_kprintf("[intercom] talk setup cannot relay the negotiated SCO pair; closing SCO and restoring media\n");
        app_bt_begin_talk_stop();
        return;
    }

    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        if (hfp_audio_relay_option(&g_slots[index].sco_primary.para, 1) != 0)
        {
            rt_kprintf("[intercom] failed to register SCO%u with voice relay; rolling back talk\n",
                       index + 1U);
            app_bt_begin_talk_stop();
            return;
        }
        g_slots[index].sco_primary.relay_registered = true;
    }

    app_bt_talk_timer_cancel();
    app_state_set_mode(APP_MODE_TALKING);
    rt_kprintf("[intercom] two SCO links are relaying: codec=%s rate=%u packet=%u interval=%u\n",
               app_bt_sco_codec_name(g_slots[0].sco_primary.para.air_mode),
               app_bt_sco_sample_rate(g_slots[0].sco_primary.para.air_mode),
               APP_HFP_RELAY_PACKET_SIZE,
               g_slots[0].sco_primary.para.tx_intvl);
}

static void app_bt_dump_discovery(void)
{
    uint8_t index;

    rt_kprintf("[intercom] inquiry complete: %u device(s)\n", g_discovered_count);
    for (index = 0; index < g_discovered_count; index++)
    {
        rt_kprintf("  %u. ", index + 1U);
        app_bt_print_mac(&g_discovered[index].mac);
        rt_kprintf("  RSSI=%d  COD=0x%06X  %s\n",
                   g_discovered[index].rssi,
                   (unsigned int)g_discovered[index].dev_cls,
                   g_discovered[index].name[0] ? g_discovered[index].name : "<unknown>");
    }
}

static void app_bt_handle_discovery(const app_bt_discovery_t *device)
{
    uint8_t index;

    for (index = 0; index < g_discovered_count; index++)
    {
        if (app_bt_mac_equal(&g_discovered[index].mac, &device->mac))
        {
            g_discovered[index] = *device;
            return;
        }
    }

    if (g_discovered_count < APP_BT_MAX_DISCOVERED)
    {
        index = g_discovered_count++;
        g_discovered[index] = *device;
        rt_kprintf("[intercom] found %u: ", index + 1U);
        app_bt_print_mac(&device->mac);
        rt_kprintf("  RSSI=%d  %s\n", device->rssi,
                   device->name[0] ? device->name : "<unknown>");
    }
}

static void app_bt_handle_scan_command(void)
{
    bt_start_inquiry_ex_t inquiry = {0};
    app_mode_t mode = app_state_get_mode();

    if (g_scanning)
    {
        rt_kprintf("[intercom] inquiry is already running\n");
        return;
    }
    if ((mode != APP_MODE_IDLE) && (mode != APP_MODE_MEDIA))
    {
        rt_kprintf("[intercom] scan rejected in state %s\n",
                   app_state_mode_name(mode));
        return;
    }

    memset(g_discovered, 0, sizeof(g_discovered));
    g_discovered_count = 0U;

    inquiry.dev_cls_mask = 0U;
    inquiry.max_timeout = 10U;
    inquiry.max_rsp = APP_BT_MAX_DISCOVERED;
    if (bt_interface_start_inquiry_ex(&inquiry) != 0)
    {
        rt_kprintf("[intercom] failed to start inquiry\n");
        return;
    }

    g_scanning = true;
    app_state_set_mode(APP_MODE_SCANNING);
    rt_kprintf("[intercom] inquiry started for about 10 seconds\n");
}

static void app_bt_handle_connect_command(const app_bt_event_t *event)
{
    int slot_index;
    app_bt_slot_t *slot;
    app_mode_t mode = app_state_get_mode();

    if (app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR))
    {
        rt_kprintf("[intercom] connect rejected in state %s\n",
                   app_state_mode_name(mode));
        return;
    }
    if (g_connecting_slot >= 0)
    {
        rt_kprintf("[intercom] another headset connection is still pending\n");
        return;
    }

    if (g_scanning)
    {
        bt_interface_stop_inquiry();
        g_scanning = false;
    }

    slot_index = app_bt_find_slot_by_mac(&event->data.command_device.mac);
    if (slot_index < 0)
    {
        slot_index = app_bt_find_free_slot();
    }
    if (slot_index < 0)
    {
        rt_kprintf("[intercom] both headset slots are assigned; disconnect one first\n");
        return;
    }

    slot = &g_slots[slot_index];
    if (slot->used && ((slot->connection_stage != APP_BT_CONN_IDLE) ||
                       app_bt_slot_has_link(slot) || slot->release_pending))
    {
        rt_kprintf("[intercom] headset %u is already connecting/connected\n",
                   slot_index + 1U);
        return;
    }

    app_bt_clear_slot(slot_index);
    slot->used = true;
    slot->mac = event->data.command_device.mac;
    strncpy(slot->name, event->data.command_device.name, sizeof(slot->name) - 1U);

    if ((app_bt_a2dp_count() != 0U) && app_bt_media_active())
    {
        slot->connection_stage = APP_BT_CONN_MEDIA_STOP_PENDING;
        slot->connection_generation = app_bt_next_connection_generation();
        slot->restore_media_after_connect = true;
        slot->media_stop_sequence = app_bt_request_media_stop();
        app_bt_connect_timer_start(slot_index, APP_BT_MEDIA_CLOSE_TIMEOUT_MS);
        app_state_set_mode(APP_MODE_CONNECTING);
        rt_kprintf("[intercom] waiting for A2DP sharing to suspend before connecting headset %u\n",
                   slot_index + 1U);
        return;
    }

    app_bt_begin_a2dp_connect(slot_index);
}

static void app_bt_handle_disconnect_command(const app_bt_event_t *event)
{
    int slot_index = app_bt_find_slot_by_mac(&event->data.command_device.mac);

    if (slot_index < 0)
    {
        rt_kprintf("[intercom] device is not assigned to a headset slot\n");
        return;
    }

    if (app_bt_is_talk_mode(app_state_get_mode()))
    {
        if (g_pending_disconnect)
        {
            rt_kprintf("[intercom] another disconnect is already pending\n");
            return;
        }
        g_pending_disconnect = true;
        g_pending_disconnect_mac = event->data.command_device.mac;
        rt_kprintf("[intercom] stopping talk before disconnecting headset %u\n",
                   slot_index + 1U);
        app_bt_begin_talk_stop();
        return;
    }

    if (!g_slots[slot_index].acl_connected &&
            !g_slots[slot_index].a2dp_connected &&
            !g_slots[slot_index].hfp_connected &&
            !app_bt_sco_is_pending(&g_slots[slot_index]))
    {
        if (app_bt_connection_pending(&g_slots[slot_index]))
        {
            g_slots[slot_index].release_pending = true;
            app_bt_abort_connection(slot_index);
            rt_kprintf("[intercom] cancelling pending connection for headset %u\n",
                       slot_index + 1U);
            return;
        }
        app_bt_connect_timer_stop(slot_index);
        app_bt_disconnect_timer_cancel(slot_index);
        app_bt_clear_slot(slot_index);
        app_bt_refresh_leds();
        app_bt_update_media_for_links();
        rt_kprintf("[intercom] released inactive headset slot %u\n",
                   slot_index + 1U);
        return;
    }

    app_bt_connect_timer_stop(slot_index);
    g_slots[slot_index].connection_stage = APP_BT_CONN_ABORTING;
    g_slots[slot_index].release_pending = true;
    if (!g_slots[slot_index].disconnect_watchdog_active)
    {
        g_slots[slot_index].disconnect_attempts = 0U;
        if (app_bt_issue_disconnect(slot_index) != 0)
        {
            rt_kprintf("[intercom] disconnect request failed for headset %u; retry scheduled\n",
                       slot_index + 1U);
        }
    }
    rt_kprintf("[intercom] disconnect requested for headset %u\n",
               slot_index + 1U);
}

static void app_bt_handle_acl_connected(const bt_notify_device_acl_conn_info_t *info)
{
    int slot_index = app_bt_find_slot_by_mac(&info->mac);
    app_bt_slot_t *slot;

    if (slot_index < 0)
    {
        bt_notify_device_mac_t mac = info->mac;

        rt_kprintf("[intercom] rejecting unassigned ACL device: ");
        app_bt_print_mac(&info->mac);
        rt_kprintf("\n");
        if (info->res == HCI_SUCC)
        {
            bt_interface_disconnect_req(mac.addr);
        }
        return;
    }

    slot = &g_slots[slot_index];
    slot->acl_connected = (info->res == HCI_SUCC);
    if (info->res != HCI_SUCC)
    {
        if (app_bt_connection_pending(slot))
        {
            app_bt_abort_connection(slot_index);
        }
    }
    else if ((slot->connection_stage == APP_BT_CONN_ABORTING) ||
             slot->release_pending)
    {
        rt_kprintf("[intercom] rejecting late ACL success for headset %u\n",
                   slot_index + 1U);
        app_bt_abort_connection(slot_index);
    }
    else if (!app_bt_connection_pending(slot) &&
             (slot->connection_stage != APP_BT_CONN_READY))
    {
        rt_kprintf("[intercom] rejecting unexpected ACL success for headset %u\n",
                   slot_index + 1U);
        app_bt_abort_connection(slot_index);
    }
    rt_kprintf("[intercom] headset %u ACL %s\n", slot_index + 1U,
               (info->res == HCI_SUCC) ? "connected" : "failed");
    app_bt_update_activity_state();
}

static void app_bt_handle_acl_disconnected(const bt_notify_device_base_info_t *info)
{
    int slot_index = app_bt_find_slot_by_mac(&info->mac);
    bool talk_affected = false;
    bool release_slot;
    uint16_t closed_handle;
    app_mode_t mode;

    if (slot_index < 0)
    {
        return;
    }

    mode = app_state_get_mode();
    talk_affected = app_bt_is_talk_mode(mode) ||
                    app_bt_sco_is_pending(&g_slots[slot_index]) ||
                    g_slots[slot_index].sco_primary.relay_registered ||
                    g_slots[slot_index].sco_orphan.relay_registered ||
                    (mode == APP_MODE_ERROR);
    if (talk_affected)
    {
        app_bt_unregister_relays();
    }
    app_bt_connect_timer_stop(slot_index);
    app_bt_disconnect_timer_cancel(slot_index);
    closed_handle = app_bt_sco_has_handle(&g_slots[slot_index]) ?
                    g_slots[slot_index].sco_primary.para.sco_hdl : 0U;
    app_bt_sco_mark_idle(&g_slots[slot_index], closed_handle);
    release_slot = g_slots[slot_index].release_pending;
    g_slots[slot_index].connection_stage = APP_BT_CONN_IDLE;
    g_slots[slot_index].acl_connected = false;
    g_slots[slot_index].a2dp_connected = false;
    g_slots[slot_index].hfp_connected = false;
    g_slots[slot_index].avrcp_connected = false;
    g_slots[slot_index].avrcp_connect_requested = false;
    g_slots[slot_index].avrcp_role_result = BT_ERROR_DISCONNECTED;
    g_slots[slot_index].remote_music_volume_pending = false;
    g_slots[slot_index].remote_music_volume_retries = 0U;
    g_slots[slot_index].disconnect_blocks_media = false;
    g_slots[slot_index].profile_cleanup_pending = false;
    if (release_slot)
    {
        app_bt_clear_slot(slot_index);
    }

    rt_kprintf("[intercom] headset %u ACL disconnected, reason=%u\n",
               slot_index + 1U, info->res);
    app_bt_refresh_leds();
    if (mode == APP_MODE_ERROR)
    {
        app_bt_report_error_cleanup();
    }
    else if (talk_affected && (mode != APP_MODE_TALK_STOPPING))
    {
        app_bt_begin_talk_stop();
    }
    else if (mode == APP_MODE_TALK_STOPPING)
    {
        app_bt_schedule_talk_stop_completion();
    }
    else
    {
        app_bt_update_media_for_links();
    }
}

static void app_bt_prepare_acl_for_audio(const bt_notify_device_mac_t *mac)
{
    bt_cm_device_manager_t *manager = bt_cm_get_env();
    BTS2S_BD_ADDR address;
    bt_cm_dev_acl_info_t *connection;

    if (bt_addr_convert_to_bts((bd_addr_t *)mac, &address) != 0U)
    {
        return;
    }
    if (manager == RT_NULL)
    {
        return;
    }
    connection = bt_cm_get_conn_by_addr(manager, &address);
    if ((connection != RT_NULL) && (connection->info.role == BT_LINK_SLAVE))
    {
        bt_interface_wr_link_policy_setting((unsigned char *)mac->addr, 0x05);
        bt_send_switch_role(&address, 0);
    }
}

static bool app_bt_avrcp_connected_now(const app_bt_slot_t *slot)
{
    bt_notify_device_mac_t mac = slot->mac;

    return bt_interface_avrcp_get_connection_by_addr(&mac) <
           CFG_MAX_AVRCP_CONN_NUM;
}

static int app_bt_find_unique_avrcp_slot(void)
{
    int found = -1;
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (!g_slots[index].used || !g_slots[index].avrcp_connected)
        {
            continue;
        }
        if (found >= 0)
        {
            return -1;
        }
        found = index;
    }
    return found;
}

static void app_bt_request_avrcp(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bt_err_t error;

    if (!g_avrcp_ready || !slot->used || !slot->acl_connected ||
            !slot->a2dp_connected || !slot->hfp_connected ||
            slot->release_pending || slot->avrcp_connected ||
            slot->avrcp_connect_requested)
    {
        return;
    }

    error = bt_interface_conn_to_source_ext(slot->mac.addr,
                                            BT_PROFILE_AVRCP);
    if (error == BT_EOK)
    {
        slot->avrcp_connect_requested = true;
        rt_kprintf("[intercom] headset %u AVRCP Target connect requested\n",
                   slot_index + 1U);
    }
    else if (app_bt_avrcp_connected_now(slot))
    {
        /* The incoming connection event is already queued. */
        slot->avrcp_connect_requested = true;
    }
    else
    {
        rt_kprintf("[intercom] headset %u optional AVRCP request failed: %d\n",
                   slot_index + 1U, (int)error);
    }
}

static bool app_bt_has_pending_remote_music_volume(void)
{
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].remote_music_volume_pending)
        {
            return true;
        }
    }
    return false;
}

static void app_bt_schedule_remote_music_volume(void)
{
    if (g_avrcp_volume_timer == RT_NULL)
    {
        return;
    }
    rt_timer_stop(g_avrcp_volume_timer);
    rt_timer_start(g_avrcp_volume_timer);
}

static void app_bt_apply_remote_music_volume(void)
{
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_slot_t *slot = &g_slots[index];
        bt_err_t error;
        uint8_t absolute_volume;

        if (!slot->remote_music_volume_pending)
        {
            continue;
        }
        if (!slot->used || !slot->avrcp_connected ||
                (slot->avrcp_role_result != BT_EOK) ||
                !app_bt_avrcp_connected_now(slot))
        {
            slot->remote_music_volume_pending = false;
            slot->remote_music_volume_retries = 0U;
            rt_kprintf("[intercom] headset %u music volume rejected: AVRCP is not connected\n",
                       index + 1U);
            continue;
        }

        absolute_volume = bt_interface_avrcp_local_vol_2_abs_vol(
                              slot->remote_music_volume_desired,
                              APP_VOLUME_LEVEL_MAX);
        error = bt_interface_avrcp_set_absolute_volume_as_tg_role_ext(
                    &slot->mac, absolute_volume);
        if (error == BT_EOK)
        {
            slot->remote_music_volume_level =
                slot->remote_music_volume_desired;
            slot->remote_music_volume_valid = true;
            slot->remote_music_volume_pending = false;
            slot->remote_music_volume_retries = 0U;
            rt_kprintf("[intercom] headset %u music volume requested: %u/%u (AVRCP=%u/127)\n",
                       index + 1U, slot->remote_music_volume_level,
                       APP_VOLUME_LEVEL_MAX, absolute_volume);
            if (app_bt_has_pending_remote_music_volume())
            {
                app_bt_schedule_remote_music_volume();
            }
            return;
        }
        if (error == BT_ERROR_STATE)
        {
            slot->remote_music_volume_retries++;
            if (slot->remote_music_volume_retries <
                    APP_BT_AVRCP_VOLUME_MAX_RETRIES)
            {
                app_bt_schedule_remote_music_volume();
                return;
            }
            slot->remote_music_volume_pending = false;
            slot->remote_music_volume_retries = 0U;
            rt_kprintf("[intercom] headset %u music volume timed out waiting for AVRCP\n",
                       index + 1U);
            if (slot->avrcp_connected && app_bt_avrcp_connected_now(slot))
            {
                bt_err_t reset_error = bt_interface_disc_ext(
                                           slot->mac.addr, BT_PROFILE_AVRCP);

                rt_kprintf("[intercom] headset %u AVRCP volume channel reset: %d\n",
                           index + 1U, (int)reset_error);
            }
            continue;
        }

        slot->remote_music_volume_pending = false;
        slot->remote_music_volume_retries = 0U;
        if (error == BT_ERROR_UNSUPPORTED)
        {
            rt_kprintf("[intercom] headset %u does not support AVRCP Absolute Volume\n",
                       index + 1U);
        }
        else
        {
            rt_kprintf("[intercom] headset %u music volume request failed: %d\n",
                       index + 1U, (int)error);
        }
    }
}

static void app_bt_handle_avrcp(uint16_t event_id,
                                const bt_notify_profile_state_info_t *info,
                                bt_err_t role_result)
{
    int slot_index = app_bt_find_slot_by_mac(&info->mac);
    app_bt_slot_t *slot;

    if (slot_index < 0)
    {
        return;
    }
    slot = &g_slots[slot_index];

    if ((event_id == BT_NOTIFY_AVRCP_PROFILE_CONNECTED) &&
            (info->res == BTS2_SUCC))
    {
        slot->avrcp_connected = true;
        slot->avrcp_connect_requested = false;
        slot->avrcp_role_result = role_result;
        if (role_result == BT_EOK)
        {
            rt_kprintf("[intercom] headset %u AVRCP Target connected\n",
                       slot_index + 1U);
        }
        else
        {
            rt_kprintf("[intercom] headset %u AVRCP role setup failed: %d\n",
                       slot_index + 1U, (int)role_result);
        }
        return;
    }

    if (app_bt_avrcp_connected_now(slot))
    {
        slot->avrcp_connect_requested = false;
        rt_kprintf("[intercom] headset %u ignored stale AVRCP failure, active control link remains\n",
                   slot_index + 1U);
        return;
    }

    if (slot->avrcp_connected || slot->avrcp_connect_requested)
    {
        rt_kprintf("[intercom] headset %u AVRCP disconnected, reason=%u\n",
                   slot_index + 1U, info->res);
    }
    slot->avrcp_connected = false;
    slot->avrcp_connect_requested = false;
    slot->avrcp_role_result = BT_ERROR_DISCONNECTED;
    slot->remote_music_volume_pending = false;
    slot->remote_music_volume_retries = 0U;
}

static void app_bt_handle_a2dp(uint16_t event_id,
                               const bt_notify_profile_state_info_t *info)
{
    int slot_index = app_bt_find_slot_by_mac(&info->mac);
    app_bt_slot_t *slot;

    if (slot_index < 0)
    {
        return;
    }
    slot = &g_slots[slot_index];

    if (event_id == BT_NOTIFY_A2DP_PROFILE_CONNECTED)
    {
        if (info->res == BTS2_SUCC)
        {
            app_mode_t mode = app_state_get_mode();

            if ((slot->connection_stage == APP_BT_CONN_READY) &&
                    slot->a2dp_connected)
            {
                rt_kprintf("[intercom] duplicate A2DP success for headset %u ignored\n",
                           slot_index + 1U);
                return;
            }

            slot->a2dp_connected = true;
            if ((slot->connection_stage != APP_BT_CONN_A2DP_PENDING) ||
                    slot->release_pending || app_bt_is_talk_mode(mode) ||
                    (mode == APP_MODE_ERROR))
            {
                rt_kprintf("[intercom] rejecting out-of-stage A2DP success for headset %u\n",
                           slot_index + 1U);
                if (app_bt_is_talk_mode(mode))
                {
                    slot->connection_stage = APP_BT_CONN_ABORTING;
                    slot->profile_cleanup_pending = true;
                    slot->disconnect_blocks_media = true;
                    app_bt_begin_talk_stop();
                }
                else
                {
                    app_bt_abort_connection(slot_index);
                }
                app_bt_refresh_leds();
                return;
            }

            slot->connection_stage = APP_BT_CONN_HFP_PENDING;
            app_bt_prepare_acl_for_audio(&info->mac);
            rt_kprintf("[intercom] headset %u A2DP Source connected\n",
                       slot_index + 1U);

            if (!slot->hfp_connected)
            {
                bt_err_t error = bt_interface_conn_to_source_ext(
                                     slot->mac.addr, BT_PROFILE_HFP);
                if (error != BT_EOK)
                {
                    rt_kprintf("[intercom] headset %u HFP AG request failed: %d\n",
                               slot_index + 1U, (int)error);
                    app_bt_abort_connection(slot_index);
                }
                else
                {
                    slot->connection_generation =
                        app_bt_next_connection_generation();
                    app_bt_connect_timer_start(slot_index,
                                               APP_BT_CONNECT_TIMEOUT_MS);
                }
            }
        }
        else
        {
            rt_kprintf("[intercom] headset %u A2DP connect failed: %u\n",
                       slot_index + 1U, info->res);
            if (slot->connection_stage == APP_BT_CONN_A2DP_PENDING)
            {
                app_bt_abort_connection(slot_index);
            }
        }
    }
    else
    {
        bool connection_failed = app_bt_connection_pending(slot);
        app_mode_t mode = app_state_get_mode();
        bool talk_affected = app_bt_is_talk_mode(mode) ||
                             app_bt_sco_is_pending(slot) ||
                             slot->sco_primary.relay_registered ||
                             slot->sco_orphan.relay_registered;

        slot->a2dp_connected = false;
        rt_kprintf("[intercom] headset %u A2DP disconnected\n",
                   slot_index + 1U);
        if (talk_affected)
        {
            slot->connection_stage = APP_BT_CONN_ABORTING;
            slot->profile_cleanup_pending = true;
            slot->disconnect_blocks_media = true;
            app_bt_begin_talk_stop();
        }
        else
        {
            if ((slot->connection_stage != APP_BT_CONN_ABORTING) &&
                    (connection_failed || app_bt_slot_has_link(slot)))
            {
                app_bt_abort_connection(slot_index);
            }
            app_bt_update_media_for_links();
        }
    }
    app_bt_refresh_leds();
    app_bt_update_activity_state();
}

static void app_bt_handle_hfp(uint16_t event_id,
                              const bt_notify_profile_state_info_t *info)
{
    int slot_index = app_bt_find_slot_by_mac(&info->mac);
    app_bt_slot_t *slot;

    if (slot_index < 0)
    {
        return;
    }
    slot = &g_slots[slot_index];

    if (event_id == BT_NOTIFY_AG_PROFILE_CONNECTED)
    {
        if (info->res == BTS2_SUCC)
        {
            app_mode_t mode = app_state_get_mode();

            if ((slot->connection_stage == APP_BT_CONN_READY) &&
                    slot->hfp_connected)
            {
                rt_kprintf("[intercom] duplicate HFP success for headset %u ignored\n",
                           slot_index + 1U);
                return;
            }

            slot->hfp_session_generation++;
            if (slot->hfp_session_generation == 0U)
            {
                slot->hfp_session_generation++;
            }
            slot->hfp_connected = true;
            slot->hfp_channel = info->profile_channel;
            if ((slot->connection_stage != APP_BT_CONN_HFP_PENDING) ||
                    !slot->a2dp_connected || slot->release_pending ||
                    app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR))
            {
                rt_kprintf("[intercom] rejecting out-of-stage HFP success for headset %u\n",
                           slot_index + 1U);
                if (app_bt_is_talk_mode(mode))
                {
                    slot->connection_stage = APP_BT_CONN_ABORTING;
                    slot->profile_cleanup_pending = true;
                    slot->disconnect_blocks_media = true;
                    app_bt_begin_talk_stop();
                }
                else
                {
                    app_bt_abort_connection(slot_index);
                }
                app_bt_refresh_leds();
                return;
            }

            slot->connection_stage = APP_BT_CONN_READY;
            app_bt_connect_timer_stop(slot_index);
            slot->restore_media_after_connect = false;
            rt_kprintf("[intercom] headset %u HFP AG connected, channel=%u\n",
                       slot_index + 1U, info->profile_channel);
            app_bt_update_media_for_links();
            app_bt_request_avrcp(slot_index);
        }
        else
        {
            rt_kprintf("[intercom] headset %u HFP AG connect failed: %u\n",
                       slot_index + 1U, info->res);
            if (slot->connection_stage == APP_BT_CONN_HFP_PENDING)
            {
                app_bt_abort_connection(slot_index);
            }
        }
    }
    else
    {
        bool connection_failed = app_bt_connection_pending(slot);
        app_mode_t mode = app_state_get_mode();
        bool sco_unresolved = app_bt_sco_is_pending(slot) ||
                              slot->sco_primary.relay_registered ||
                              slot->sco_orphan.relay_registered;
        bool require_acl_barrier = sco_unresolved ||
                                   slot->sco_requires_acl_barrier;
        bool talk_affected = app_bt_is_talk_mode(mode) || sco_unresolved ||
                             (mode == APP_MODE_ERROR);
        uint16_t closed_handle = app_bt_sco_has_handle(slot) ?
                                 slot->sco_primary.para.sco_hdl : 0U;

        if (talk_affected)
        {
            app_bt_unregister_relays();
            if (require_acl_barrier)
            {
                slot->sco_requires_acl_barrier = true;
            }
            if (mode != APP_MODE_ERROR)
            {
                slot->connection_stage = APP_BT_CONN_ABORTING;
                slot->profile_cleanup_pending = true;
                slot->disconnect_blocks_media = true;
            }
        }
        slot->hfp_connected = false;
        slot->hfp_channel = 0U;
        slot->hfp_session_generation++;
        if (slot->hfp_session_generation == 0U)
        {
            slot->hfp_session_generation++;
        }
        if (require_acl_barrier)
        {
            if ((slot->sco_phase == APP_BT_SCO_CONNECT_PENDING) ||
                    (slot->sco_phase == APP_BT_SCO_CANCEL_PENDING))
            {
                slot->sco_phase = APP_BT_SCO_CANCEL_PENDING;
            }
            else
            {
                slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
            }
        }
        else
        {
            app_bt_sco_mark_idle(slot, closed_handle);
        }
        rt_kprintf("[intercom] headset %u HFP AG disconnected\n",
                   slot_index + 1U);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_report_error_cleanup();
        }
        else if (talk_affected && (mode != APP_MODE_TALK_STOPPING))
        {
            app_bt_begin_talk_stop();
        }
        else if (mode == APP_MODE_TALK_STOPPING)
        {
            app_bt_schedule_talk_stop_completion();
        }
        else if ((slot->connection_stage != APP_BT_CONN_ABORTING) &&
                 (connection_failed || app_bt_slot_has_link(slot)))
        {
            rt_kprintf("[intercom] rolling back incomplete headset %u connection\n",
                       slot_index + 1U);
            app_bt_abort_connection(slot_index);
        }
        if (require_acl_barrier && slot->acl_connected)
        {
            slot->disconnect_blocks_media = true;
            app_bt_abort_connection(slot_index);
        }
    }
    app_bt_refresh_leds();
    app_bt_update_activity_state();
}

static bool app_bt_sco_parameters_match(
    const bt_device_sco_conn_para_t *left,
    const bt_device_sco_conn_para_t *right)
{
    return (left->sco_hdl == right->sco_hdl) &&
           (left->air_mode == right->air_mode) &&
           (left->tx_intvl == right->tx_intvl) &&
           (left->rx_pkt_len == right->rx_pkt_len) &&
           (left->tx_pkt_len == right->tx_pkt_len);
}

static void app_bt_close_sco_in_error(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bt_err_t error;

    g_restore_media = false;
    g_error_cleanup_reported = false;
    slot->sco_requires_acl_barrier = slot->acl_connected;
    if (app_bt_sco_has_live_handle(slot))
    {
        slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
    }
    app_bt_set_media_desired(false);
    app_bt_unregister_slot_relay(slot);
    if (slot->hfp_connected)
    {
        app_bt_set_call_state(slot, false);
    }
    error = app_bt_switch_sco(slot, false);
    if (error != BT_EOK)
    {
        rt_kprintf("[intercom] terminal SCO close request failed for headset %u: %d\n",
                   slot_index + 1U, (int)error);
    }
    if (slot->acl_connected)
    {
        slot->connection_stage = APP_BT_CONN_ABORTING;
        slot->disconnect_blocks_media = true;
        app_bt_abort_connection(slot_index);
    }
    app_bt_report_error_cleanup();
}

static void app_bt_enter_terminal_sco_error(const char *reason)
{
    uint8_t index;

    app_bt_talk_timer_cancel();
    g_talk_waiting_for_media = false;
    g_talk_stop_settling = false;
    g_talk_recovery_attempted = true;
    g_restore_media = false;
    g_error_cleanup_reported = false;
    if (g_pending_disconnect)
    {
        g_pending_disconnect = false;
        rt_kprintf("[intercom] deferred disconnect cancelled by terminal SCO error\n");
    }
    app_bt_set_media_desired(false);
    app_bt_unregister_relays();
    app_state_set_mode(APP_MODE_ERROR);
    rt_kprintf("[intercom] terminal SCO identity error: %s; tearing down both ACL links\n",
               reason);

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_slot_t *slot = &g_slots[index];

        if (!slot->used)
        {
            continue;
        }
        slot->sco_requires_acl_barrier = slot->acl_connected;
        if ((slot->sco_phase == APP_BT_SCO_CONNECT_PENDING) ||
                (slot->sco_phase == APP_BT_SCO_CANCEL_PENDING))
        {
            slot->sco_phase = APP_BT_SCO_CANCEL_PENDING;
        }
        else if (app_bt_sco_has_live_handle(slot) ||
                 slot->sco_requires_acl_barrier)
        {
            slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
        }
        if (slot->hfp_connected)
        {
            app_bt_set_call_state(slot, false);
            (void)app_bt_switch_sco(slot, false);
        }
        if (slot->acl_connected)
        {
            slot->connection_stage = APP_BT_CONN_ABORTING;
            slot->disconnect_blocks_media = true;
            app_bt_abort_connection(index);
        }
    }
    app_bt_report_error_cleanup();
}

static void app_bt_handle_sco(uint16_t event_id,
                              const bt_notify_device_sco_info_t *info)
{
    int slot_index;
    int channel_slot_index;
    app_bt_slot_t *slot;
    app_mode_t mode;
    bool session_matches;
    bool request_matches;

    if (info->sco_type != BT_NOTIFY_HFP_AG)
    {
        return;
    }

    slot_index = app_bt_find_slot_by_mac(&info->para.bd);
    if (slot_index < 0)
    {
        rt_kprintf("[intercom] SCO event for an unassigned device, channel=%u\n",
                   info->profile_channel);
        if ((event_id == BT_NOTIFY_COMMON_SCO_CONNECTED) &&
                (info->sco_res == BTS2_SUCC))
        {
            (void)app_bt_switch_sco_for_mac(&info->para.bd, false);
            rt_kprintf("[intercom] close requested for unassigned SCO handle=0x%04X\n",
                       info->para.sco_hdl);
        }
        return;
    }

    slot = &g_slots[slot_index];
    mode = app_state_get_mode();
    channel_slot_index = app_bt_find_slot_by_channel(info->profile_channel);
    session_matches = slot->hfp_connected &&
                      (slot->hfp_channel == info->profile_channel) &&
                      ((channel_slot_index < 0) ||
                       (channel_slot_index == slot_index));
    request_matches = session_matches &&
                      (slot->sco_request_channel == info->profile_channel) &&
                      (slot->sco_connection_generation ==
                       slot->connection_generation) &&
                      (slot->sco_hfp_session_generation ==
                       slot->hfp_session_generation);
    if ((mode == APP_MODE_TALK_STOPPING) && g_talk_stop_settling)
    {
        app_bt_talk_timer_start(APP_BT_SCO_EVENT_SETTLE_MS);
    }

    if ((event_id == BT_NOTIFY_COMMON_SCO_CONNECTED) &&
            (info->sco_res == BTS2_SUCC) &&
            (!slot->acl_connected || !slot->hfp_connected))
    {
        app_bt_unregister_slot_relay(slot);
        if (!slot->acl_connected || !slot->sco_requires_acl_barrier)
        {
            app_bt_sco_mark_idle(slot, info->para.sco_hdl);
        }
        else
        {
            app_bt_sco_retire_handle(slot, info->para.sco_hdl);
            slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
        }
        (void)app_bt_switch_sco_for_mac(&info->para.bd, false);
        rt_kprintf("[intercom] discarded SCO success without a live ACL/HFP session: headset=%u handle=0x%04X\n",
                   slot_index + 1U, info->para.sco_hdl);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_report_error_cleanup();
        }
        else if (mode == APP_MODE_TALK_STOPPING)
        {
            app_bt_schedule_talk_stop_completion();
        }
        else if ((mode == APP_MODE_TALK_STARTING) ||
                 (mode == APP_MODE_TALKING))
        {
            app_bt_begin_talk_stop();
        }
        return;
    }

    if (event_id == BT_NOTIFY_COMMON_SCO_CONNECTED)
    {
        app_bt_sco_phase_t phase = slot->sco_phase;

        if (info->sco_res != BTS2_SUCC)
        {
            uint32_t generation = slot->sco_generation;

            if (((phase != APP_BT_SCO_CONNECT_PENDING) &&
                    (phase != APP_BT_SCO_CANCEL_PENDING)) ||
                    !request_matches)
            {
                rt_kprintf("[intercom] ignoring stale SCO failure for headset %u: phase=%s channel=%u expected=%u\n",
                           slot_index + 1U,
                           app_bt_sco_phase_name(phase),
                           info->profile_channel,
                           slot->sco_request_channel);
                return;
            }

            app_bt_sco_mark_idle(slot, 0U);
            rt_kprintf("[intercom] headset %u SCO request %lu failed: %u\n",
                       slot_index + 1U, (unsigned long)generation,
                       info->sco_res);
            if (mode == APP_MODE_ERROR)
            {
                app_bt_report_error_cleanup();
            }
            else if (mode == APP_MODE_TALK_STOPPING)
            {
                app_bt_schedule_talk_stop_completion();
            }
            else
            {
                app_bt_begin_talk_stop();
            }
            return;
        }

        if (slot->sco_primary.present)
        {
            if (slot->sco_primary.para.sco_hdl == info->para.sco_hdl)
            {
                if (!app_bt_sco_parameters_match(&slot->sco_primary.para,
                                                 &info->para))
                {
                    slot->sco_requires_acl_barrier = true;
                    rt_kprintf("[intercom] SCO parameter conflict for headset %u handle=0x%04X\n",
                               slot_index + 1U, info->para.sco_hdl);
                    app_bt_enter_terminal_sco_error(
                        "one handle was reported with different parameters");
                    return;
                }
                rt_kprintf("[intercom] duplicate SCO success ignored for headset %u, handle=0x%04X\n",
                           slot_index + 1U, info->para.sco_hdl);
                if (slot->sco_phase == APP_BT_SCO_CLOSE_PENDING)
                {
                    if (mode == APP_MODE_ERROR)
                    {
                        app_bt_close_sco_in_error(slot_index);
                    }
                    else
                    {
                        (void)app_bt_switch_sco(slot, false);
                        app_bt_talk_timer_start(
                            APP_BT_TALK_STOP_TIMEOUT_MS);
                    }
                }
                return;
            }

            if (slot->sco_orphan.present)
            {
                if ((slot->sco_orphan.para.sco_hdl == info->para.sco_hdl) &&
                        app_bt_sco_parameters_match(&slot->sco_orphan.para,
                                                    &info->para))
                {
                    rt_kprintf("[intercom] duplicate orphan SCO success for headset %u, handle=0x%04X\n",
                               slot_index + 1U, info->para.sco_hdl);
                    app_bt_close_sco_in_error(slot_index);
                    return;
                }
                rt_kprintf("[intercom] SCO handle ledger overflow for headset %u: primary=0x%04X orphan=0x%04X new=0x%04X\n",
                           slot_index + 1U,
                           slot->sco_primary.para.sco_hdl,
                           slot->sco_orphan.para.sco_hdl,
                           info->para.sco_hdl);
                slot->sco_requires_acl_barrier = true;
                app_bt_enter_terminal_sco_error(
                    "more than two live handles were reported for one headset");
                return;
            }

            slot->sco_orphan.present = true;
            slot->sco_orphan.para = info->para;
            slot->sco_requires_acl_barrier = true;
            rt_kprintf("[intercom] conflicting SCO success for headset %u: primary=0x%04X orphan=0x%04X\n",
                       slot_index + 1U,
                       slot->sco_primary.para.sco_hdl,
                       info->para.sco_hdl);
            app_bt_enter_terminal_sco_error(
                "two different live handles were reported for one headset");
            return;
        }

        if (slot->sco_orphan.present)
        {
            slot->sco_requires_acl_barrier = true;
            rt_kprintf("[intercom] SCO success arrived while only an orphan handle is tracked for headset %u\n",
                       slot_index + 1U);
            app_bt_enter_terminal_sco_error(
                "primary SCO identity was lost while an orphan remained");
            return;
        }

        app_bt_sco_unretire_handle(slot, info->para.sco_hdl);
        slot->sco_primary.present = true;
        slot->sco_primary.relay_registered = false;
        slot->sco_primary.para = info->para;
        if ((phase == APP_BT_SCO_CONNECT_PENDING) &&
                request_matches && (mode == APP_MODE_TALK_STARTING))
        {
            slot->sco_phase = APP_BT_SCO_ACTIVE;
            bt_interface_spk_vol_change_req(slot->hfp_channel,
                                            slot->talk_volume_level);
            rt_kprintf("[intercom] headset %u SCO connected: request=%lu handle=0x%04X path=%d codec=%s air=%u interval=%u rx=%u tx=%u\n",
                       slot_index + 1U,
                       (unsigned long)slot->sco_generation,
                       info->para.sco_hdl,
                       app_bt_sco_relay_path(&info->para),
                       app_bt_sco_codec_name(info->para.air_mode),
                       info->para.air_mode, info->para.tx_intvl,
                       info->para.rx_pkt_len, info->para.tx_pkt_len);
            app_bt_try_start_relay();
            return;
        }

        slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
        g_talk_stop_settling = false;
        rt_kprintf("[intercom] rejecting late/unsolicited SCO for headset %u: request=%lu phase=%s channel=%u expected=%u handle=0x%04X\n",
                   slot_index + 1U,
                   (unsigned long)slot->sco_generation,
                   app_bt_sco_phase_name(phase),
                   info->profile_channel, slot->sco_request_channel,
                   info->para.sco_hdl);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_close_sco_in_error(slot_index);
            return;
        }
        if (!app_bt_is_talk_mode(mode))
        {
            g_restore_media = g_media_desired || app_bt_media_active();
            app_bt_request_media_stop();
        }
        if (mode != APP_MODE_TALK_STOPPING)
        {
            app_bt_begin_talk_stop();
        }
        (void)app_bt_switch_sco(slot, false);
        app_bt_talk_timer_start(APP_BT_TALK_STOP_TIMEOUT_MS);
        return;
    }

    if (slot->sco_primary.present &&
            (slot->sco_primary.para.sco_hdl == info->para.sco_hdl))
    {
        bool was_active = slot->sco_phase == APP_BT_SCO_ACTIVE;
        bool was_relay = slot->sco_primary.relay_registered;
        uint16_t active_handle = slot->sco_primary.para.sco_hdl;

        app_bt_unregister_slot_relay(slot);
        app_bt_sco_retire_handle(slot, active_handle);
        memset(&slot->sco_primary, 0, sizeof(slot->sco_primary));
        if (slot->sco_requires_acl_barrier || slot->sco_orphan.present)
        {
            slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
            rt_kprintf("[intercom] headset %u primary SCO disconnected: handle=0x%04X; waiting for ACL cleanup barrier\n",
                       slot_index + 1U, active_handle);
            if (mode == APP_MODE_ERROR)
            {
                app_bt_report_error_cleanup();
            }
            else if (mode == APP_MODE_TALK_STOPPING)
            {
                /* ACL_DISCONNECTED is the authoritative recovery barrier. */
            }
            else
            {
                app_bt_enter_terminal_sco_error(
                    "a conflicting SCO identity remained after disconnect");
            }
            return;
        }
        app_bt_sco_mark_idle(slot, active_handle);
        rt_kprintf("[intercom] headset %u SCO disconnected: handle=0x%04X\n",
                   slot_index + 1U, active_handle);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_report_error_cleanup();
        }
        else if ((mode == APP_MODE_TALK_STOPPING))
        {
            app_bt_schedule_talk_stop_completion();
        }
        else if (was_active || was_relay ||
                 (mode == APP_MODE_TALK_STARTING) ||
                 (mode == APP_MODE_TALKING))
        {
            app_bt_begin_talk_stop();
        }
        return;
    }

    if (slot->sco_orphan.present &&
            (slot->sco_orphan.para.sco_hdl == info->para.sco_hdl))
    {
        uint16_t orphan_handle = slot->sco_orphan.para.sco_hdl;

        app_bt_unregister_slot_relay(slot);
        app_bt_sco_retire_handle(slot, orphan_handle);
        memset(&slot->sco_orphan, 0, sizeof(slot->sco_orphan));
        slot->sco_phase = APP_BT_SCO_CLOSE_PENDING;
        rt_kprintf("[intercom] headset %u orphan SCO disconnected: handle=0x%04X; waiting for ACL cleanup barrier\n",
                   slot_index + 1U, orphan_handle);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_report_error_cleanup();
        }
        else
        {
            app_bt_enter_terminal_sco_error(
                "an orphan SCO was observed outside terminal cleanup");
        }
        return;
    }

    if (app_bt_sco_handle_is_retired(slot, info->para.sco_hdl))
    {
        rt_kprintf("[intercom] duplicate retired SCO disconnect ignored for headset %u: handle=0x%04X\n",
                   slot_index + 1U, info->para.sco_hdl);
        if (mode == APP_MODE_TALK_STOPPING)
        {
            app_bt_schedule_talk_stop_completion();
        }
        return;
    }

    if ((slot->sco_phase == APP_BT_SCO_CONNECT_PENDING) ||
            (slot->sco_phase == APP_BT_SCO_CANCEL_PENDING))
    {
        app_bt_sco_phase_t phase = slot->sco_phase;
        uint32_t generation = slot->sco_generation;

        if (!request_matches)
        {
            app_bt_sco_retire_handle(slot, info->para.sco_hdl);
            rt_kprintf("[intercom] ignoring stale SCO disconnect for headset %u: request=%lu phase=%s handle=0x%04X\n",
                       slot_index + 1U, (unsigned long)generation,
                       app_bt_sco_phase_name(phase),
                       info->para.sco_hdl);
            return;
        }

        if (phase == APP_BT_SCO_CONNECT_PENDING)
        {
            app_bt_sco_retire_handle(slot, info->para.sco_hdl);
            rt_kprintf("[intercom] uncorrelated SCO disconnect ignored while request %lu is opening for headset %u: handle=0x%04X\n",
                       (unsigned long)generation, slot_index + 1U,
                       info->para.sco_hdl);
            return;
        }

        if (slot->sco_requires_acl_barrier)
        {
            app_bt_sco_retire_handle(slot, info->para.sco_hdl);
            rt_kprintf("[intercom] SCO cancellation event recorded for headset %u; waiting for ACL cleanup barrier\n",
                       slot_index + 1U);
            return;
        }
        if ((info->para.sco_hdl == 0U) &&
                (mode != APP_MODE_TALK_STOPPING))
        {
            rt_kprintf("[intercom] zero-handle SCO cancellation cannot be correlated for headset %u; waiting for timeout recovery\n",
                       slot_index + 1U);
            return;
        }

        app_bt_sco_mark_idle(slot, info->para.sco_hdl);
        rt_kprintf("[intercom] headset %u SCO request %lu cancellation settled: handle=0x%04X\n",
                   slot_index + 1U, (unsigned long)generation,
                   info->para.sco_hdl);
        if (mode == APP_MODE_ERROR)
        {
            app_bt_report_error_cleanup();
        }
        else if (mode == APP_MODE_TALK_STOPPING)
        {
            app_bt_schedule_talk_stop_completion();
        }
        else
        {
            app_bt_begin_talk_stop();
        }
        return;
    }

    app_bt_sco_retire_handle(slot, info->para.sco_hdl);
    rt_kprintf("[intercom] unexpected SCO disconnect retired for headset %u: handle=0x%04X phase=%s\n",
               slot_index + 1U, info->para.sco_hdl,
               app_bt_sco_phase_name(slot->sco_phase));
    if (mode == APP_MODE_TALK_STOPPING)
    {
        app_bt_schedule_talk_stop_completion();
    }
}

static void app_bt_handle_internal(const app_bt_event_t *event)
{
    switch (event->id)
    {
    case APP_BT_CMD_SCAN:
        app_bt_handle_scan_command();
        break;
    case APP_BT_CMD_CONNECT:
        app_bt_handle_connect_command(event);
        break;
    case APP_BT_CMD_DISCONNECT:
        app_bt_handle_disconnect_command(event);
        break;
    case APP_BT_CMD_TALK_START:
        app_bt_start_talk();
        break;
    case APP_BT_CMD_TALK_STOP:
        if ((app_state_get_mode() == APP_MODE_TALK_STARTING) ||
                (app_state_get_mode() == APP_MODE_TALKING))
        {
            app_bt_begin_talk_stop();
        }
        else
        {
            rt_kprintf("[intercom] talk is not active\n");
        }
        break;
    case APP_BT_CMD_SET_VOLUME:
        if (event->data.volume.target == APP_VOLUME_TARGET_MUSIC)
        {
            int slot_index = app_bt_find_slot_by_mac(
                                 &event->data.volume.mac);

            if ((slot_index < 0) || (slot_index >= APP_HEADSET_COUNT) ||
                    !g_slots[slot_index].used ||
                    g_slots[slot_index].release_pending ||
                    (g_slots[slot_index].connection_generation !=
                     event->data.volume.connection_generation))
            {
                rt_kprintf("[intercom] headset music volume rejected: target connection changed\n");
                break;
            }
            g_slots[slot_index].remote_music_volume_desired =
                event->data.volume.level;
            g_slots[slot_index].remote_music_volume_retries = 0U;
            g_slots[slot_index].remote_music_volume_pending = true;
            app_bt_apply_remote_music_volume();
        }
        else if (event->data.volume.target == APP_VOLUME_TARGET_TALK)
        {
            uint8_t requested = 0U;
            app_mode_t mode = app_state_get_mode();
            int slot_index = app_bt_find_slot_by_mac(&event->data.volume.mac);
            app_bt_slot_t *slot;

            if ((slot_index < 0) ||
                    !g_slots[slot_index].used ||
                    g_slots[slot_index].release_pending ||
                    (g_slots[slot_index].connection_generation !=
                     event->data.volume.connection_generation))
            {
                rt_kprintf("[intercom] headset talk volume rejected: target connection changed\n");
                break;
            }
            slot = &g_slots[slot_index];

            slot->talk_volume_level = event->data.volume.level;
            if (((mode == APP_MODE_TALK_STARTING) ||
                    (mode == APP_MODE_TALKING)) && slot->hfp_connected)
            {
                bt_interface_spk_vol_change_req(slot->hfp_channel,
                                                slot->talk_volume_level);
                requested = 1U;
            }
            rt_kprintf("[intercom] headset %u talk volume set to %u/%u, requested=%u\n",
                       slot_index + 1,
                       slot->talk_volume_level, APP_VOLUME_LEVEL_MAX,
                       requested);
        }
        break;
    case APP_BT_CMD_AVRCP_VOLUME_RETRY:
        app_bt_apply_remote_music_volume();
        break;
    case APP_BT_CMD_TALK_TIMEOUT:
        if (event->data.token != g_talk_timer_token)
        {
            break;
        }
        if (app_state_get_mode() == APP_MODE_TALK_STOPPING)
        {
            if (g_talk_stop_settling && app_bt_all_sco_quiescent())
            {
                g_talk_stop_settling = false;
                app_bt_complete_talk_stop();
            }
            else
            {
                g_talk_stop_settling = false;
                app_bt_recover_stuck_sco();
            }
        }
        else if (app_state_get_mode() == APP_MODE_TALK_STARTING)
        {
            if (g_talk_waiting_for_media)
            {
                rt_kprintf("[intercom] A2DP suspend timeout; talk aborted\n");
                g_talk_waiting_for_media = false;
                app_bt_talk_timer_cancel();
                app_bt_set_media_desired(g_restore_media);
                g_restore_media = false;
                app_state_set_mode(APP_MODE_IDLE);
                app_bt_update_activity_state();
            }
            else
            {
                rt_kprintf("[intercom] two-SCO setup timeout; rolling back\n");
                app_bt_begin_talk_stop();
            }
        }
        break;
    case APP_BT_CMD_CONNECT_TIMEOUT:
        if ((event->data.timer.slot >= 0) &&
                ((uint8_t)event->data.timer.slot < APP_HEADSET_COUNT))
        {
            int slot_index = event->data.timer.slot;
            app_bt_slot_t *slot = &g_slots[slot_index];

            if ((g_connecting_slot == slot_index) &&
                    (event->data.timer.generation ==
                     slot->connection_generation) &&
                    app_bt_connection_pending(slot))
            {
                if (slot->connection_stage ==
                        APP_BT_CONN_MEDIA_STOP_PENDING)
                {
                    rt_kprintf("[intercom] A2DP sharing suspend timed out before connecting headset %u\n",
                               slot_index + 1U);
                    app_bt_cancel_deferred_connect(slot_index);
                }
                else
                {
                    rt_kprintf("[intercom] headset %u connection timed out\n",
                               slot_index + 1U);
                    app_bt_abort_connection(slot_index);
                }
            }
        }
        break;
    case APP_BT_CMD_DISCONNECT_TIMEOUT:
        if ((event->data.timer.slot >= 0) &&
                ((uint8_t)event->data.timer.slot < APP_HEADSET_COUNT))
        {
            int slot_index = event->data.timer.slot;
            app_bt_slot_t *slot = &g_slots[slot_index];

            if (!slot->disconnect_watchdog_active ||
                    (event->data.timer.generation !=
                     slot->disconnect_generation))
            {
                break;
            }
            if (!app_bt_slot_has_link(slot))
            {
                bool release_slot = slot->release_pending;

                app_bt_disconnect_timer_cancel(slot_index);
                slot->connection_stage = APP_BT_CONN_IDLE;
                slot->disconnect_blocks_media = false;
                if (release_slot)
                {
                    app_bt_clear_slot(slot_index);
                }
                app_bt_refresh_leds();
                app_bt_update_media_for_links();
                rt_kprintf("[intercom] headset %u disconnect completed without ACL event\n",
                           slot_index + 1U);
            }
            else if (slot->disconnect_attempts <
                     APP_BT_DISCONNECT_MAX_ATTEMPTS)
            {
                int error = app_bt_issue_disconnect(slot_index);

                rt_kprintf("[intercom] retrying headset %u disconnect (%u/%u), result=%d\n",
                           slot_index + 1U, slot->disconnect_attempts,
                           APP_BT_DISCONNECT_MAX_ATTEMPTS, error);
            }
            else
            {
                bool blocks_media = slot->disconnect_blocks_media;

                app_bt_disconnect_timer_cancel(slot_index);
                slot->release_pending = false;
                slot->disconnect_blocks_media = false;
                if (slot->a2dp_connected && slot->hfp_connected)
                {
                    slot->connection_stage = APP_BT_CONN_READY;
                }
                if (blocks_media)
                {
                    app_bt_set_media_desired(false);
                    app_state_set_mode(APP_MODE_ERROR);
                    rt_kprintf("[intercom] deferred disconnect timed out; media remains stopped\n");
                }
                else
                {
                    rt_kprintf("[intercom] disconnect watchdog exhausted for headset %u\n",
                               slot_index + 1U);
                    app_bt_update_media_for_links();
                }
            }
        }
        break;
    case APP_BT_EVENT_MEDIA_CLOSED:
        app_bt_handle_media_closed(event->data.token);
        break;
    case APP_BT_EVENT_MEDIA_ERROR:
        g_media_desired = false;
        rt_kprintf("[intercom] A2DP sharing error: %d\n",
                   (int32_t)event->data.token);
        break;
    default:
        break;
    }
}

static void app_bt_manager_thread(void *parameter)
{
    app_bt_event_t event;

    (void)parameter;
    while (1)
    {
        if (rt_mq_recv(g_bt_event_queue, &event, sizeof(event),
                       RT_WAITING_FOREVER) != RT_EOK)
        {
            continue;
        }

        rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
        if (event.type == APP_BT_INTERNAL_EVENT_TYPE)
        {
            app_bt_handle_internal(&event);
        }
        else if (event.type == BT_NOTIFY_COMMON)
        {
            switch (event.id)
            {
            case BT_NOTIFY_COMMON_BT_STACK_READY:
            {
                static const char local_name[] = "SiFli Intercom";

                bt_interface_set_local_name(strlen(local_name), (void *)local_name);
                bt_interface_set_scan_mode(0U, 0U);
                g_bt_ready = true;
                rt_mb_send(g_bt_ready_mailbox, 1U);
                rt_kprintf("[intercom] Bluetooth stack and profiles are ready\n");
                break;
            }
            case BT_NOTIFY_COMMON_DISCOVER_IND:
                app_bt_handle_discovery(&event.data.discovery);
                break;
            case BT_NOTIFY_COMMON_INQUIRY_CMP:
                if (g_scanning)
                {
                    g_scanning = false;
                    app_bt_dump_discovery();
                    app_bt_update_activity_state();
                }
                break;
            case BT_NOTIFY_COMMON_ACL_CONNECTED:
                app_bt_handle_acl_connected(&event.data.acl_connected);
                break;
            case BT_NOTIFY_COMMON_ACL_DISCONNECTED:
                app_bt_handle_acl_disconnected(&event.data.acl_disconnected);
                break;
            case BT_NOTIFY_COMMON_SCO_CONNECTED:
            case BT_NOTIFY_COMMON_SCO_DISCONNECTED:
                app_bt_handle_sco(event.id, &event.data.sco);
                break;
            default:
                break;
            }
        }
        else if ((event.type == BT_NOTIFY_A2DP) &&
                 ((event.id == BT_NOTIFY_A2DP_PROFILE_CONNECTED) ||
                  (event.id == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED)))
        {
            app_bt_handle_a2dp(event.id, &event.data.profile);
        }
        else if ((event.type == BT_NOTIFY_HFP_AG) &&
                 ((event.id == BT_NOTIFY_AG_PROFILE_CONNECTED) ||
                  (event.id == BT_NOTIFY_AG_PROFILE_DISCONNECTED)))
        {
            app_bt_handle_hfp(event.id, &event.data.profile);
        }
        else if (event.type == BT_NOTIFY_AVRCP)
        {
            if (event.id == BT_NOTIFY_AVRCP_OPEN_COMPLETE)
            {
                uint8_t index;

                g_avrcp_ready = true;
                rt_kprintf("[intercom] AVRCP control service is ready\n");
                for (index = 0U; index < APP_HEADSET_COUNT; index++)
                {
                    app_bt_request_avrcp(index);
                }
            }
            else if ((event.id == BT_NOTIFY_AVRCP_PROFILE_CONNECTED) ||
                     (event.id == BT_NOTIFY_AVRCP_PROFILE_DISCONNECTED))
            {
                app_bt_handle_avrcp(event.id,
                                    &event.data.avrcp.profile,
                                    event.data.avrcp.role_result);
            }
            else if (event.id == BT_NOTIFY_AVRCP_ABSOLUTE_VOLUME)
            {
                int source_slot = app_bt_find_unique_avrcp_slot();
                uint8_t level = event.data.avrcp_reported_volume;
                bool duplicate = false;

                /* The SDK maps VOLUME_CHANGED to AUDIO_MAX_VOLUME before
                 * publishing this event. Keep a fallback for the other SDK
                 * path, which can still publish an absolute 0..127 value. */
                if (level > APP_VOLUME_LEVEL_MAX)
                {
                    level = bt_interface_avrcp_abs_vol_2_local_vol(
                                level & 0x7FU, APP_VOLUME_LEVEL_MAX);
                }
                if (g_avrcp_reported_volume_valid &&
                        g_avrcp_reported_source_known &&
                        (source_slot >= 0) &&
                        (g_avrcp_reported_volume_level == level) &&
                        app_bt_mac_equal(&g_avrcp_reported_mac,
                                         &g_slots[source_slot].mac))
                {
                    duplicate = true;
                }

                g_avrcp_reported_volume_valid = true;
                g_avrcp_reported_volume_level = level;
                g_avrcp_reported_source_known = source_slot >= 0;
                if (source_slot >= 0)
                {
                    g_avrcp_reported_mac = g_slots[source_slot].mac;
                    if (!duplicate)
                    {
                        rt_kprintf("[intercom] headset %u AVRCP volume event: level=%u/%u\n",
                                   source_slot + 1, level,
                                   APP_VOLUME_LEVEL_MAX);
                    }
                }
                else
                {
                    rt_kprintf("[intercom] AVRCP volume event: level=%u/%u\n",
                               level,
                               APP_VOLUME_LEVEL_MAX);
                }
            }
        }
        rt_mutex_release(g_bt_lock);
    }
}

static int app_bt_event_callback(uint16_t type, uint16_t event_id,
                                 uint8_t *data, uint16_t data_len)
{
    app_bt_event_t event = {0};
    bool enqueue = true;

    event.type = type;
    event.id = event_id;

    if (type == BT_NOTIFY_A2DP)
    {
        app_a2dp_share_on_a2dp_event(event_id);
    }

    if (type == BT_NOTIFY_COMMON)
    {
        switch (event_id)
        {
        case BT_NOTIFY_COMMON_BT_STACK_READY:
        case BT_NOTIFY_COMMON_INQUIRY_CMP:
            break;
        case BT_NOTIFY_COMMON_DISCOVER_IND:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(bt_notify_remote_device_info_t)))
            {
                enqueue = false;
            }
            else
            {
                const bt_notify_remote_device_info_t *source =
                    (const bt_notify_remote_device_info_t *)data;

                event.data.discovery.mac = source->mac;
                event.data.discovery.dev_cls = source->dev_cls;
                event.data.discovery.rssi = source->rssi;
                strncpy(event.data.discovery.name, source->bt_name,
                        sizeof(event.data.discovery.name) - 1U);
            }
            break;
        case BT_NOTIFY_COMMON_ACL_CONNECTED:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(event.data.acl_connected)))
            {
                enqueue = false;
            }
            else
            {
                memcpy(&event.data.acl_connected, data,
                       sizeof(event.data.acl_connected));
                event.data.acl_connected.acl_info = RT_NULL;
            }
            break;
        case BT_NOTIFY_COMMON_ACL_DISCONNECTED:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(event.data.acl_disconnected)))
            {
                enqueue = false;
            }
            else
            {
                memcpy(&event.data.acl_disconnected, data,
                       sizeof(event.data.acl_disconnected));
            }
            break;
        case BT_NOTIFY_COMMON_SCO_CONNECTED:
        case BT_NOTIFY_COMMON_SCO_DISCONNECTED:
            if ((data == RT_NULL) || (data_len < sizeof(event.data.sco)))
            {
                enqueue = false;
            }
            else
            {
                memcpy(&event.data.sco, data, sizeof(event.data.sco));
            }
            break;
        default:
            enqueue = false;
            break;
        }
    }
    else if ((type == BT_NOTIFY_A2DP) &&
             ((event_id == BT_NOTIFY_A2DP_PROFILE_CONNECTED) ||
              (event_id == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED)))
    {
        if ((data == RT_NULL) || (data_len < sizeof(event.data.profile)))
        {
            enqueue = false;
        }
        else
        {
            memcpy(&event.data.profile, data, sizeof(event.data.profile));
        }
    }
    else if ((type == BT_NOTIFY_HFP_AG) &&
             ((event_id == BT_NOTIFY_AG_PROFILE_CONNECTED) ||
              (event_id == BT_NOTIFY_AG_PROFILE_DISCONNECTED)))
    {
        if ((data == RT_NULL) || (data_len < sizeof(event.data.profile)))
        {
            enqueue = false;
        }
        else
        {
            memcpy(&event.data.profile, data, sizeof(event.data.profile));
        }
    }
    else if (type == BT_NOTIFY_AVRCP)
    {
        switch (event_id)
        {
        case BT_NOTIFY_AVRCP_OPEN_COMPLETE:
            break;
        case BT_NOTIFY_AVRCP_PROFILE_CONNECTED:
        case BT_NOTIFY_AVRCP_PROFILE_DISCONNECTED:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(event.data.avrcp.profile)))
            {
                enqueue = false;
            }
            else
            {
                const bt_notify_profile_state_info_t *source =
                    (const bt_notify_profile_state_info_t *)data;

                memcpy(&event.data.avrcp.profile, source,
                       sizeof(event.data.avrcp.profile));
                event.data.avrcp.role_result = BT_EOK;
                if ((event_id == BT_NOTIFY_AVRCP_PROFILE_CONNECTED) &&
                        (source->res == BTS2_SUCC))
                {
                    event.data.avrcp.role_result =
                        bt_interface_set_avrcp_role_ext(
                            &event.data.avrcp.profile.mac, AVRCP_TG);
                }
            }
            break;
        case BT_NOTIFY_AVRCP_ABSOLUTE_VOLUME:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(event.data.avrcp_reported_volume)))
            {
                enqueue = false;
            }
            else
            {
                event.data.avrcp_reported_volume = *data;
            }
            break;
        default:
            enqueue = false;
            break;
        }
    }
    else
    {
        enqueue = false;
    }

    if (enqueue && (g_bt_event_queue != RT_NULL) &&
            (rt_mq_send(g_bt_event_queue, &event, sizeof(event)) != RT_EOK))
    {
        return -RT_EFULL;
    }
    return RT_EOK;
}

static void app_bt_talk_timeout(void *parameter)
{
    (void)parameter;
    if (app_bt_post_simple_event(APP_BT_CMD_TALK_TIMEOUT,
                                 g_talk_timer_token) != RT_EOK)
    {
        rt_kprintf("[intercom] failed to queue talk timeout\n");
    }
}

static void app_bt_connect_timeout(void *parameter)
{
    int8_t slot;
    uint32_t generation;

    (void)parameter;
    slot = g_connect_timer_arm.slot;
    generation = g_connect_timer_arm.generation;
    if (app_bt_post_timer_event(APP_BT_CMD_CONNECT_TIMEOUT, slot,
                                generation) != RT_EOK)
    {
        rt_kprintf("[intercom] failed to queue connection timeout\n");
    }
}

static void app_bt_disconnect_timeout(void *parameter)
{
    uint8_t slot_index = *(const uint8_t *)parameter;

    if ((slot_index >= APP_HEADSET_COUNT) ||
            (app_bt_post_timer_event(APP_BT_CMD_DISCONNECT_TIMEOUT,
                                     (int8_t)slot_index,
                                     g_slots[slot_index].disconnect_generation) !=
             RT_EOK))
    {
        rt_kprintf("[intercom] failed to queue disconnect timeout\n");
    }
}

static void app_bt_avrcp_volume_timeout(void *parameter)
{
    (void)parameter;
    if (app_bt_post_simple_event(APP_BT_CMD_AVRCP_VOLUME_RETRY, 0U) !=
            RT_EOK)
    {
        rt_kprintf("[intercom] failed to queue AVRCP volume retry\n");
    }
}

static int app_bt_post_simple_event(uint16_t event_id, uint32_t token)
{
    app_bt_event_t event = {0};

    event.type = APP_BT_INTERNAL_EVENT_TYPE;
    event.id = event_id;
    event.data.token = token;
    return rt_mq_send(g_bt_event_queue, &event, sizeof(event));
}

static int app_bt_post_timer_event(uint16_t event_id, int8_t slot,
                                   uint32_t generation)
{
    app_bt_event_t event = {0};

    event.type = APP_BT_INTERNAL_EVENT_TYPE;
    event.id = event_id;
    event.data.timer.slot = slot;
    event.data.timer.generation = generation;
    return rt_mq_send(g_bt_event_queue, &event, sizeof(event));
}

static int app_bt_post_command(uint16_t command,
                               const bt_notify_device_mac_t *mac,
                               const char *name)
{
    app_bt_event_t event = {0};

    event.type = APP_BT_INTERNAL_EVENT_TYPE;
    event.id = command;
    if (mac != RT_NULL)
    {
        event.data.command_device.mac = *mac;
    }
    if (name != RT_NULL)
    {
        strncpy(event.data.command_device.name, name,
                sizeof(event.data.command_device.name) - 1U);
    }
    return rt_mq_send(g_bt_event_queue, &event, sizeof(event));
}

static int app_bt_post_volume_command(app_volume_target_t target,
                                      const bt_notify_device_mac_t *mac,
                                      uint32_t connection_generation,
                                      uint8_t level)
{
    app_bt_event_t event = {0};

    event.type = APP_BT_INTERNAL_EVENT_TYPE;
    event.id = APP_BT_CMD_SET_VOLUME;
    event.data.volume.target = target;
    event.data.volume.level = level;
    event.data.volume.mac = *mac;
    event.data.volume.connection_generation = connection_generation;
    return rt_mq_send(g_bt_event_queue, &event, sizeof(event));
}

int app_bt_init(void)
{
    rt_thread_t manager_thread;
    rt_uint32_t ready_event;
    bt_interface_status_t callback_status;
    int sharing_result;
    uint8_t index;

    g_bt_initialized = false;
    memset(g_slots, 0, sizeof(g_slots));
    g_connecting_slot = -1;
    g_connect_timer_arm.slot = -1;
    g_connect_timer_arm.generation = 0U;
    g_next_connection_generation = 0U;
    g_next_sco_generation = 0U;
    g_bt_ready = false;
    g_avrcp_ready = false;
    g_scanning = false;
    g_restore_media = false;
    g_talk_waiting_for_media = false;
    g_talk_stop_settling = false;
    g_talk_recovery_attempted = false;
    g_error_cleanup_reported = false;
    g_pending_disconnect = false;
    g_talk_timer_token = 0U;
    g_talk_media_sequence = 0U;
    g_media_stop_sequence = 0U;
    g_media_desired = false;
    g_talk_volume_level = APP_BT_DEFAULT_TALK_VOLUME;
    g_avrcp_reported_volume_valid = false;
    g_avrcp_reported_source_known = false;
    g_avrcp_reported_volume_level = 0U;
    memset(&g_avrcp_reported_mac, 0, sizeof(g_avrcp_reported_mac));
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_clear_slot(index);
    }
    g_bt_event_queue = rt_mq_create("btmgr", sizeof(app_bt_event_t),
                                    APP_BT_EVENT_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    g_bt_ready_mailbox = rt_mb_create("btrdy", 2U, RT_IPC_FLAG_FIFO);
    g_bt_lock = rt_mutex_create("btlock", RT_IPC_FLAG_FIFO);
    g_talk_timer = rt_timer_create("bttalk", app_bt_talk_timeout, RT_NULL,
                                   rt_tick_from_millisecond(APP_BT_TALK_START_TIMEOUT_MS),
                                   RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    g_connect_timer = rt_timer_create("btconn", app_bt_connect_timeout, RT_NULL,
                                      rt_tick_from_millisecond(APP_BT_CONNECT_TIMEOUT_MS),
                                      RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    g_avrcp_volume_timer = rt_timer_create(
                               "btavvol", app_bt_avrcp_volume_timeout, RT_NULL,
                               rt_tick_from_millisecond(
                                   APP_BT_AVRCP_VOLUME_RETRY_MS),
                               RT_TIMER_FLAG_ONE_SHOT |
                               RT_TIMER_FLAG_SOFT_TIMER);
    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        const char *name = (index == 0U) ? "btdisc0" : "btdisc1";

        g_disconnect_timer_slot_ids[index] = index;
        g_disconnect_timers[index] = rt_timer_create(
                                         name, app_bt_disconnect_timeout,
                                         &g_disconnect_timer_slot_ids[index],
                                         rt_tick_from_millisecond(APP_BT_DISCONNECT_TIMEOUT_MS),
                                         RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
    if ((g_bt_event_queue == RT_NULL) || (g_bt_ready_mailbox == RT_NULL) ||
            (g_bt_lock == RT_NULL) ||
            (g_talk_timer == RT_NULL) || (g_connect_timer == RT_NULL) ||
            (g_avrcp_volume_timer == RT_NULL) ||
            (g_disconnect_timers[0] == RT_NULL) ||
            (g_disconnect_timers[1] == RT_NULL))
    {
        return -RT_ENOMEM;
    }

    sharing_result = app_a2dp_share_init(app_bt_share_callback, RT_NULL);
    if (sharing_result != RT_EOK)
    {
        return sharing_result;
    }

    manager_thread = rt_thread_create("btmgr", app_bt_manager_thread, RT_NULL,
                                      2048, RT_THREAD_PRIORITY_MIDDLE,
                                      RT_THREAD_TICK_DEFAULT);
    if (manager_thread == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    callback_status = bt_interface_register_bt_event_notify_callback(
                          app_bt_event_callback);
    if ((callback_status != BT_INTERFACE_STATUS_OK) &&
            (callback_status != BT_INTERFACE_STATUS_ALREADY_EXIST))
    {
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(manager_thread) != RT_EOK)
    {
        bt_interface_unregister_bt_event_notify_callback(app_bt_event_callback);
        return -RT_ERROR;
    }

    sifli_ble_enable();
    if ((rt_mb_recv(g_bt_ready_mailbox, &ready_event,
                    rt_tick_from_millisecond(APP_BT_READY_TIMEOUT_MS)) != RT_EOK) ||
            (ready_event != 1U))
    {
        return -RT_ETIMEOUT;
    }
    g_bt_initialized = true;
    return RT_EOK;
}

bool app_bt_is_ready(void)
{
    return g_bt_initialized && g_bt_ready;
}

int app_bt_scan(void)
{
    app_mode_t mode = app_state_get_mode();

    if (!g_bt_initialized || !g_bt_ready ||
            ((mode != APP_MODE_IDLE) && (mode != APP_MODE_MEDIA)))
    {
        return -RT_EBUSY;
    }
    return app_bt_post_command(APP_BT_CMD_SCAN, RT_NULL, RT_NULL);
}

int app_bt_connect(const char *target)
{
    bt_notify_device_mac_t mac = {0};
    char name[APP_BT_NAME_LENGTH] = {0};
    uint8_t index;

    if (!g_bt_initialized || !g_bt_ready || (target == RT_NULL))
    {
        return -RT_EINVAL;
    }
    if (app_bt_is_talk_mode(app_state_get_mode()) ||
            (app_state_get_mode() == APP_MODE_ERROR))
    {
        return -RT_EBUSY;
    }

    if (app_bt_parse_index(target, &index))
    {
        rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
        if (index > g_discovered_count)
        {
            rt_mutex_release(g_bt_lock);
            return -RT_EINVAL;
        }
        mac = g_discovered[index - 1U].mac;
        strncpy(name, g_discovered[index - 1U].name, sizeof(name) - 1U);
        rt_mutex_release(g_bt_lock);
    }
    else if (!app_bt_parse_mac(target, &mac))
    {
        return -RT_EINVAL;
    }

    return app_bt_post_command(APP_BT_CMD_CONNECT, &mac, name);
}

int app_bt_disconnect(const char *target)
{
    bt_notify_device_mac_t mac = {0};
    uint8_t index;

    if (!g_bt_initialized || !g_bt_ready || (target == RT_NULL))
    {
        return -RT_EINVAL;
    }

    if (app_bt_parse_index(target, &index))
    {
        if (index > APP_HEADSET_COUNT)
        {
            return -RT_EINVAL;
        }
        rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
        if (!g_slots[index - 1U].used)
        {
            rt_mutex_release(g_bt_lock);
            return -RT_EINVAL;
        }
        mac = g_slots[index - 1U].mac;
        rt_mutex_release(g_bt_lock);
    }
    else if (!app_bt_parse_mac(target, &mac))
    {
        return -RT_EINVAL;
    }

    return app_bt_post_command(APP_BT_CMD_DISCONNECT, &mac, RT_NULL);
}

int app_bt_talk(bool start)
{
    app_mode_t mode = app_state_get_mode();

    if (!g_bt_initialized || !g_bt_ready)
    {
        return -RT_EBUSY;
    }
    if ((start && ((mode != APP_MODE_MEDIA) || g_scanning ||
                   (g_connecting_slot >= 0))) ||
            (!start && !app_bt_is_talk_mode(mode)))
    {
        return -RT_EBUSY;
    }
    return app_bt_post_command(start ? APP_BT_CMD_TALK_START :
                               APP_BT_CMD_TALK_STOP, RT_NULL, RT_NULL);
}

int app_bt_set_headset_volume(app_volume_target_t target, uint8_t headset,
                              uint8_t level)
{
    uint8_t slot_index;
    bool ready;
    bt_notify_device_mac_t mac;
    uint32_t connection_generation;

    if (!g_bt_initialized)
    {
        return -RT_ENOSYS;
    }
    if (((target != APP_VOLUME_TARGET_MUSIC) &&
            (target != APP_VOLUME_TARGET_TALK)) ||
            (headset == 0U) || (headset > APP_HEADSET_COUNT) ||
            (level > APP_VOLUME_LEVEL_MAX))
    {
        return -RT_EINVAL;
    }

    slot_index = headset - 1U;
    rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
    ready = g_slots[slot_index].used &&
            g_slots[slot_index].acl_connected &&
            (g_slots[slot_index].connection_stage == APP_BT_CONN_READY) &&
            !g_slots[slot_index].release_pending;
    if (target == APP_VOLUME_TARGET_MUSIC)
    {
        ready = ready && g_slots[slot_index].avrcp_connected &&
                (g_slots[slot_index].avrcp_role_result == BT_EOK);
    }
    else
    {
        ready = ready && g_slots[slot_index].hfp_connected;
    }
    mac = g_slots[slot_index].mac;
    connection_generation = g_slots[slot_index].connection_generation;
    rt_mutex_release(g_bt_lock);
    if (!ready)
    {
        return -RT_EBUSY;
    }
    return app_bt_post_volume_command(target, &mac, connection_generation,
                                      level);
}

void app_bt_print_volume(void)
{
    uint8_t index;
    uint8_t music_volume;
    uint8_t talk_volume;

    if (!g_bt_initialized || (g_bt_lock == RT_NULL))
    {
        rt_kprintf("[intercom] Bluetooth manager is not initialized\n");
        return;
    }
    music_volume = app_a2dp_share_get_volume();
    rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
    talk_volume = g_talk_volume_level;
    rt_kprintf("[intercom] volume: music=%u/%u talk=%u/%u\n",
               music_volume, APP_VOLUME_LEVEL_MAX,
               talk_volume, APP_VOLUME_LEVEL_MAX);
    if (g_avrcp_reported_volume_valid)
    {
        rt_kprintf("  last-headset-volume: level=%u/%u",
                   g_avrcp_reported_volume_level,
                   APP_VOLUME_LEVEL_MAX);
        if (g_avrcp_reported_source_known)
        {
            rt_kprintf(" source=");
            app_bt_print_mac(&g_avrcp_reported_mac);
        }
        rt_kprintf("\n");
    }
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        const app_bt_slot_t *slot = &g_slots[index];

        if (!slot->used)
        {
            continue;
        }
        rt_kprintf("  headset%u: talk=%u/%u music-requested=",
                   index + 1U, slot->talk_volume_level,
                   APP_VOLUME_LEVEL_MAX);
        if (slot->remote_music_volume_valid)
        {
            rt_kprintf("%u/%u", slot->remote_music_volume_level,
                       APP_VOLUME_LEVEL_MAX);
        }
        else
        {
            rt_kprintf("unchanged");
        }
        rt_kprintf(" avrcp=%u pending=%u\n", slot->avrcp_connected,
                   slot->remote_music_volume_pending);
    }
    rt_mutex_release(g_bt_lock);
}

void app_bt_print_status(void)
{
    uint8_t index;
    app_i2s_stats_t i2s_stats;
    app_a2dp_share_status_t share_status;
    app_a2dp_share_stats_t share_stats;
    rt_size_t rx_buffered;
    rt_uint32_t total_memory = 0U;
    rt_uint32_t used_memory = 0U;
    rt_uint32_t max_used_memory = 0U;
    uint8_t music_volume;
    uint8_t talk_volume;

    if (!g_bt_initialized || (g_bt_lock == RT_NULL))
    {
        rt_kprintf("[intercom] Bluetooth manager is not initialized\n");
        return;
    }

    app_i2s_get_stats(&i2s_stats);
    app_a2dp_share_get_status(&share_status);
    app_a2dp_share_get_stats(&share_stats);
    music_volume = app_a2dp_share_get_volume();
    rx_buffered = app_i2s_rx_buffered();
    rt_memory_info(&total_memory, &used_memory, &max_used_memory);
    rt_kprintf("[intercom] state=%s bt=%s avrcp=%s scan=%s media=%s discovered=%u\n",
               app_state_mode_name(app_state_get_mode()),
               g_bt_ready ? "ready" : "not-ready",
               g_avrcp_ready ? "ready" : "not-ready",
               g_scanning ? "running" : "idle",
               app_bt_media_state_name(),
               g_discovered_count);

    rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        const app_bt_slot_t *slot = &g_slots[index];

        rt_kprintf("  headset%u: ", index + 1U);
        if (!slot->used)
        {
            rt_kprintf("unassigned\n");
            continue;
        }
        app_bt_print_mac(&slot->mac);
        rt_kprintf("  %s  ACL=%u A2DP=%u HFP=%u(ch=%u session=%lu) AVRCP=%u SCO=%u phase=%s request=%lu relay=%u barrier=%u",
                   slot->name[0] ? slot->name : "<unknown>",
                   slot->acl_connected, slot->a2dp_connected,
                   slot->hfp_connected, slot->hfp_channel,
                   (unsigned long)slot->hfp_session_generation,
                   slot->avrcp_connected,
                   app_bt_sco_has_live_handle(slot),
                   app_bt_sco_phase_name(slot->sco_phase),
                   (unsigned long)slot->sco_generation,
                   slot->sco_primary.relay_registered ||
                   slot->sco_orphan.relay_registered,
                   slot->sco_requires_acl_barrier);
        if (slot->sco_primary.present)
        {
            rt_kprintf(" primary=0x%04X path=%d codec=%s air=%u interval=%u rx=%u tx=%u",
                       slot->sco_primary.para.sco_hdl,
                       app_bt_sco_relay_path(&slot->sco_primary.para),
                       app_bt_sco_codec_name(slot->sco_primary.para.air_mode),
                       slot->sco_primary.para.air_mode,
                       slot->sco_primary.para.tx_intvl,
                       slot->sco_primary.para.rx_pkt_len,
                       slot->sco_primary.para.tx_pkt_len);
        }
        if (slot->sco_orphan.present)
        {
            rt_kprintf(" orphan=0x%04X",
                       slot->sco_orphan.para.sco_hdl);
        }
        rt_kprintf(" volume(talk=%u/%u music-requested=",
                   slot->talk_volume_level, APP_VOLUME_LEVEL_MAX);
        if (slot->remote_music_volume_valid)
        {
            rt_kprintf("%u/%u", slot->remote_music_volume_level,
                       APP_VOLUME_LEVEL_MAX);
        }
        else
        {
            rt_kprintf("unchanged");
        }
        rt_kprintf(")");
        rt_kprintf("\n");
    }
    talk_volume = g_talk_volume_level;
    rt_mutex_release(g_bt_lock);

    rt_kprintf("  volume: music=%u/%u talk=%u/%u\n",
               music_volume, APP_VOLUME_LEVEL_MAX,
               talk_volume, APP_VOLUME_LEVEL_MAX);

    rt_kprintf("  I2S: signaled=%u rx=%u pipe-drop=%u ring-drop=%u "
               "underflow=%u errors=%u\n",
               i2s_stats.rx_indicated_bytes, i2s_stats.rx_bytes,
               i2s_stats.rx_pipe_dropped_bytes,
               i2s_stats.rx_overflow_bytes, i2s_stats.rx_underflows,
               i2s_stats.device_errors);
    rt_kprintf("  A2DP sharing: connected=%u streaming=%u start-pending=%u "
               "suspend-pending=%u config=%s kicked=%u error=%d\n",
               share_status.connected_count, share_status.streaming_count,
               share_status.start_pending_count,
               share_status.suspend_pending_count,
               share_status.config_valid ? "valid" : "invalid",
               share_status.kicked, share_status.last_error);
    rt_kprintf("  SBC: rate=%u frame=%u frames/packet=%u pcm/packet=%u "
               "pcm-bytes=%u frames=%u packets=%u encode-errors=%u\n",
               share_status.sample_rate, share_status.sbc_frame_size,
               share_status.frames_per_packet,
               share_status.pcm_bytes_per_packet,
               share_stats.pcm_bytes, share_stats.sbc_frames,
               share_stats.packets_encoded, share_stats.encode_errors);
    rt_kprintf("  sharing buffer: i2s=%u/%u ring=%u/%u target=%u "
               "pumped=%u queue-drop=%u ring-drop=%u kicks=%u\n",
               rx_buffered, APP_I2S_RX_RING_SIZE,
               share_status.ring_bytes, share_status.ring_capacity,
               share_status.prefill_target, share_stats.records_pumped,
               share_stats.queue_drops, share_stats.ring_drops,
               share_stats.kick_count);
    rt_kprintf("  heap: used=%u max-used=%u total=%u free-now=%u\n",
               used_memory, max_used_memory, total_memory,
               (used_memory <= total_memory) ? total_memory - used_memory : 0U);
}
