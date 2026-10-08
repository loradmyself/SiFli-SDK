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
#include "hci_api.h"

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
#define APP_BT_RECONNECT_INTERVAL_MS 3000U
#define APP_BT_RECONNECT_QUEUE_SIZE 3
/* After a reset the headsets are actively paging us; while WE page THEM
 * at the same time both sides go deaf (a controller cannot page-scan
 * during its own page: measured page timeouts and 0x0D rejects that end
 * the moment the remote's page gets through). Let the remote's page win
 * first — outgoing paging starts only after this quiet window. */
#define APP_BT_RECONNECT_PASSIVE_WAIT_MS 15000U
/* Runtime drop: the headset usually re-pages within a second or two. */
#define APP_BT_RECONNECT_DROP_WAIT_MS 5000U
/* Outgoing chasing is deliberately SHORT:3 fast tries + a few backed-off
 * slow tries (~2 minutes total). A powered-off headset must stop being
 * paged quickly so the user's "disconnect" feels clean. Power-on
 * recovery does NOT depend on this window — the headset pages us when
 * it boots and is accepted via the bonded-incoming path regardless of
 * the queue state. */
#define APP_BT_RECONNECT_FAST_RETRY 3U
#define APP_BT_RECONNECT_SLOW_INTERVAL_MS 15000U
#define APP_BT_RECONNECT_MAX_TOTAL_RETRY 6U
/* On an ACL with AVDTP deferred, wait this long for the headset to open
 * AVDTP itself before we initiate — both sides opening simultaneously
 * collides at L2CAP PSM 0x19 ("conn rej psm: 19") and BOTH attempts die
 * (seen on Anyka OWS T700 after a reset: the headset is in its own
 * reconnect mode and opens AVDTP ~4-6s after the ACL comes up, right
 * into our pending attempt). Must outlast that window; headsets that
 * never self-open only pay this delay once per connect. */
#define APP_BT_ACCEPT_AVDTP_GRACE_MS 8000U
/* HFP is acquired in the BACKGROUND once A2DP is up (music/name/AVRCP do
 * not wait for it). First give the headset this window to open SLC itself:
 * measured on Redmi Buds 4 Pro, its HF starts SLC ~2.3s after AVDTP, and
 * firing our AG→HF connect into that in-flight attempt collides ("conn
 * rej psm 19" / AG res 19) and wedges the link until it drops. Anyka OWS
 * T700 opens its own SLC ~3s after AVDTP and spends ~2s in CONFIG — a 5s
 * defer landed our request right inside that window and every attempt
 * came back AG res 19. If the remote stays silent past the window we
 * request HFP ourselves, and on failure quietly retry every
 * APP_BT_HFP_RETRY_INTERVAL_MS — never tearing down a link that is
 * already playing music. */
#define APP_BT_HFP_DEFER_GRACE_MS 8000U
#define APP_BT_HFP_RETRY_INTERVAL_MS 5000U
#define APP_BT_HFP_RETRY_MAX 10U
/* A failed attempt (AG res 19 = collision with the remote's own SLC)
 * clears the way for a clean retry — come back quickly instead of
 * sitting out a full retry interval. */
#define APP_BT_HFP_RETRY_FAST_MS 3000U
/* AVDTP flap recovery: a headset that closes and re-opens AVDTP around
 * its own SLC bring-up must not cost us the whole ACL (dropping it there
 * killed the remote's in-flight SLC and left its AVDTP wedged for
 * minutes). Re-send AVDTP a couple of times before giving up. */
#define APP_BT_AV_RESEND_MAX 2U
#define APP_BT_AV_RESEND_DELAY_MS 1500U
/* A queued `intercom talk start` stays valid this long: the command is
 * accepted even while a headset's HFP is still connecting and talk then
 * starts by itself the moment both headsets are ready. */
#define APP_BT_TALK_QUEUED_TIMEOUT_MS 30000U
/* Max consecutive 2s poll waits (encryption gate / page-in-flight gate)
 * before falling through to the normal timeout recovery. */
#define APP_BT_GATE_WAITS_MAX 8U
/* Bounded retries to restart the share after a spurious ERROR while a
 * healthy headset is still streaming (share returns -EBUSY until it
 * quiesces after the failure). */
#define APP_BT_MEDIA_RECOVER_MAX 8U
#define APP_BT_MEDIA_RECOVER_DELAY_MS 500U
/* After boot/reset the first headset to reconnect auto-resumes music,
 * which would arm the silence guard and freeze outgoing retries for the
 * second headset forever (it does not self-page after OUR reset).
 * Allow outgoing attempts during this recovery window even while media
 * streams; once it closes, strict silence-during-playback applies. */
#define APP_BT_RECONNECT_BOOT_WINDOW_MS 60000U
/* Link supervision timeout written explicitly after every ACL-up so the
 * value is known and visible in the HCI log (Write_Link_Supervision_
 * Timeout, 0x0C37). Units of 0.625ms. It also bounds how long a dead
 * link (e.g. a residual ACL after our reset) blocks new connections
 * with "connection already exists", so keep it short — 10s still
 * absorbs the radio blips of a second-headset connect. */
#define APP_BT_LINK_SUPERV_TIMEOUT 0x3E80U  /* 10s */
/* Role-seize watch (S2/S3): the relay must be piconet MASTER for both
 * headsets — as slave of two headset-masters it time-shares RX slots
 * across two piconets and the second headset starves silent
 * ("connected=2 streaming=2" but no audio over the air). A real role
 * switch completes in milliseconds, but the role itself is only known
 * AFTER the stack's role discovery (HCI Read Current Role) lands
 * ~0.5-1s after ACL-up — Connection Complete carries no role field —
 * so the first check waits APP_BT_ROLE_DISC_MS. If the link is still
 * slave the seize is sent raw (GAP's switch_role_req is locally
 * rejected) and the check re-arms for the one disconnect/reconnect
 * fallback; not-yet-READY links just re-check every 2s. */
#define APP_BT_ROLE_DISC_MS 1500U
#define APP_BT_ROLE_RECHECK_MS 3000U
#define APP_BT_ROLE_NOT_READY_RETRY_MS 2000U

typedef enum
{
    APP_BT_CMD_SCAN = 1,
    APP_BT_CMD_CONNECT,
    /* Manual `intercom connect`: (re)bond from scratch — clears any stored
     * bond for the target before connecting. Distinct from APP_BT_CMD_
     * CONNECT (auto-reconnect), which must keep the bond. */
    APP_BT_CMD_CONNECT_MANUAL,
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
    APP_BT_CMD_RECONNECT,
    APP_BT_CMD_MEDIA_RECOVER,
    APP_BT_CMD_HFP_TIMEOUT,
    APP_BT_CMD_ROLE_RECHECK,
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
    /* Incoming-accept flow: AVDTP not sent yet, waiting out the grace
     * window for the headset to open AVDTP first. */
    bool av_deferred;
    /* A2DP is up; HFP acquisition is running in the background — waiting
     * out the defer grace for the remote to open SLC before requesting
     * HFP ourselves (then quiet retries). */
    bool hfp_deferred;
    /* One AG→HF request is outstanding. Firing a second into the stack
     * while the first is in flight (talk "acceleration" landing 4ms after
     * a retry did exactly that) makes both fail with AG res 19. */
    bool hfp_request_in_flight;
    uint8_t hfp_tries;
    /* Bounded AVDTP re-sends while the ACL is alive but A2DP flapped
     * mid-setup (some headsets close and re-open AVDTP around their own
     * SLC bring-up; aborting the ACL there wedges the remote). */
    uint8_t av_resend_tries;
    /* Bounded 2s wait loops (encryption / page-in-flight): unbounded
     * re-arming used to wedge any_connect_pending forever and block all
     * manual connects. */
    uint8_t gate_waits;
    /* Role-seize: raw HCI Switch Role sent for this ACL (relay must be
     * master). policy_frozen latches the post-stream DISB_ALL_LM_MODES
     * write that stops the remote flipping the link back to slave. */
    bool role_seize_sent;
    bool policy_frozen;
    /* When the current outgoing attempt started: a duplicate manual
     * connect within 2s of this is a double-press, not a wedged attempt
     * worth taking over (the takeover cancelled an in-flight Create
     * Connection and echoed ghost hdl 65535 res 2 twice). */
    rt_tick_t connect_attempt_tick;
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
        bt_notify_rmt_name_t rname; /* remote name request result */
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
static rt_timer_t g_connect_timers[APP_HEADSET_COUNT];
static uint8_t g_connect_timer_slot_ids[APP_HEADSET_COUNT];
static rt_timer_t g_avrcp_volume_timer;
static rt_timer_t g_disconnect_timers[APP_HEADSET_COUNT];
static uint8_t g_disconnect_timer_slot_ids[APP_HEADSET_COUNT];
static rt_timer_t g_hfp_timers[APP_HEADSET_COUNT];
static uint8_t g_hfp_timer_slot_ids[APP_HEADSET_COUNT];
static rt_timer_t g_role_timers[APP_HEADSET_COUNT];
static uint8_t g_role_timer_slot_ids[APP_HEADSET_COUNT];
/* S3 fallback memory: one disconnect/reconnect bounce per headset per
 * boot. Kept as MACs (not slot flags) — the reconnected headset may
 * land in the other slot. */
static bt_notify_device_mac_t g_role_bounce_used[APP_HEADSET_COUNT];
static uint8_t g_role_bounce_count;
static app_bt_discovery_t g_discovered[APP_BT_MAX_DISCOVERED];
static app_bt_slot_t g_slots[APP_HEADSET_COUNT];
static uint8_t g_discovered_count;
static volatile uint32_t g_connect_timer_gen[APP_HEADSET_COUNT];
static volatile bool g_connect_timer_armed[APP_HEADSET_COUNT];
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
/* Queued `intercom talk start`: accepted while a headset is still
 * connecting, starts by itself once both headsets are READY. */
static bool g_talk_pending;
static uint32_t g_talk_pending_tick;
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

static rt_timer_t g_reconnect_timer;
static bt_notify_device_mac_t g_reconnect_queue[APP_BT_RECONNECT_QUEUE_SIZE];
static uint8_t g_reconnect_len;
static uint8_t g_reconnect_retry[APP_BT_RECONNECT_QUEUE_SIZE];
static rt_tick_t g_reconnect_next_try[APP_BT_RECONNECT_QUEUE_SIZE];
static bool g_reconnect_active;
static rt_tick_t g_reconnect_boot_deadline;
/* Wedged-media-path bounce state (see app_bt_bounce_media_link). */
#define APP_BT_WEDGE_BOUNCE_MAX 3U
#define APP_BT_WEDGE_BOUNCE_COOLDOWN_MS 30000U
static rt_tick_t g_wedge_bounce_tick[APP_HEADSET_COUNT];
static uint8_t g_wedge_bounce_count[APP_HEADSET_COUNT];
/* Brief window in which auto-reconnect may page a headset back even
 * while the other headset keeps media active (set when a wedged link
 * is bounced and must come straight back). */
static rt_tick_t g_reconnect_media_bypass_until;
/* MACs that reached A2DP at least once since this boot — used to tell
 * "reset recovery of a headset that never came up yet" (may page during
 * playback inside the boot window) apart from "user powered off a
 * headset that WAS up" (strict silence, always). */
static bt_notify_device_mac_t g_reconnect_seen[APP_HEADSET_COUNT];
static uint8_t g_reconnect_seen_count;

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
static void app_bt_hfp_timer_start(int slot_index, uint32_t timeout_ms);
static void app_bt_hfp_timer_stop(int slot_index);
static void app_bt_role_timer_start(int slot_index, uint32_t timeout_ms);
static void app_bt_role_timer_stop(int slot_index);
static int app_bt_find_slot_by_mac(const bt_notify_device_mac_t *mac);
static void app_bt_reconnect_enqueue(const bt_notify_device_mac_t *mac);
static void app_bt_reconnect_try_next(void);
static void app_bt_reconnect_load_bonded(void);
static void app_bt_drop_residual_links(void);
static void app_bt_reconnect_stop(void);
static void app_bt_reconnect_remove_mac(const bt_notify_device_mac_t *mac);
static void app_bt_reconnect_timer_cb(void *parameter);
static void app_bt_media_recover_bts_cb(uint16_t message, void *parameter);
static bool app_bt_mac_in_queue(const bt_notify_device_mac_t *mac);
static bool app_bt_is_bonded_earphone(const bt_notify_device_mac_t *mac);
static bool app_bt_is_talk_mode(app_mode_t mode);
static void app_bt_mark_seen(const bt_notify_device_mac_t *mac);

/* Conditions under which an incoming ACL may be accepted and driven
 * through the normal profile-connect flow. */
static bool app_bt_incoming_acceptable(void)
{
    app_mode_t mode = app_state_get_mode();

    return !app_bt_is_talk_mode(mode) && (mode != APP_MODE_ERROR) &&
           !g_scanning;
}

/* A queued (or at least previously bonded earphone) device may page us
 * during reconnection and must not be rejected as "unassigned". A device
 * with an assigned slot counts too: while a manual connect is in flight
 * its bond has been cleared for re-pairing, and its racing incoming page
 * used to be rejected ("late ACL success") — killing a perfectly good
 * link that could not be recovered by any later step. */
static bool app_bt_may_accept_incoming(const bt_notify_device_mac_t *mac)
{
    int slot_index = app_bt_find_slot_by_mac(mac);

    if (app_bt_mac_in_queue(mac) || app_bt_is_bonded_earphone(mac))
    {
        return true;
    }
    /* A mid-connect slot counts (its bond may be cleared for
     * re-pairing), but never one being torn down: an explicit
     * forget/disconnect must not be resurrected by its own racing page
     * ("won the race" recovery used to revive it). */
    return (slot_index >= 0) && !g_slots[slot_index].release_pending &&
           (g_slots[slot_index].connection_stage != APP_BT_CONN_ABORTING);
}

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

/* Reconnect paths have no name source (bonding does not store names,
 * and the slot is fresh after a reboot) — ask the connected headset
 * for its name so `intercom status` does not show <unknown>. */
static void app_bt_request_remote_name(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    BTS2S_BD_ADDR addr;

    if (slot->name[0] != '\0')
    {
        return;
    }
    if (bt_addr_convert_to_bts((bd_addr_t *)slot->mac.addr, &addr) != 0U)
    {
        return;
    }
    gap_rd_rmt_name_req(bts2_task_get_app_task_id(), addr);
    rt_kprintf("[intercom] headset %u remote name requested\n",
               slot_index + 1U);
}

static void app_bt_handle_remote_name(const bt_notify_rmt_name_t *nm)
{
    int slot_index;

    if ((nm == RT_NULL) || (nm->res != BTS2_SUCC) ||
            (nm->bt_name[0] == '\0'))
    {
        return;
    }
    slot_index = app_bt_find_slot_by_mac(&nm->mac);
    if (slot_index < 0)
    {
        return;
    }
    if (g_slots[slot_index].name[0] != '\0')
    {
        return; /* keep a name learned from discovery */
    }
    strncpy(g_slots[slot_index].name, nm->bt_name,
            sizeof(g_slots[slot_index].name) - 1U);
    g_slots[slot_index].name[sizeof(g_slots[slot_index].name) - 1U] = '\0';
    rt_kprintf("[intercom] headset %u remote name: %s\n",
               slot_index + 1U, g_slots[slot_index].name);
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

    app_bt_hfp_timer_stop(slot_index);
    app_bt_role_timer_stop(slot_index);
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

/* Per-slot connect watchdogs. A single shared timer used to let one
 * headset's arming clobber another's (the overwritten attempt then never
 * timed out and its latch wedged) — the root of "only one headset
 * connects reliably" depending on which flow won the timer. */
static void app_bt_connect_timer_start(int slot_index, uint32_t timeout_ms)
{
    rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);

    rt_timer_stop(g_connect_timers[slot_index]);
    g_connect_timer_gen[slot_index] = g_slots[slot_index].connection_generation;
    g_connect_timer_armed[slot_index] = true;
    rt_timer_control(g_connect_timers[slot_index], RT_TIMER_CTRL_SET_TIME,
                     &ticks);
    rt_timer_start(g_connect_timers[slot_index]);
}

static void app_bt_connect_timer_stop(int slot_index)
{
    rt_timer_stop(g_connect_timers[slot_index]);
    g_connect_timer_armed[slot_index] = false;
}

static bool app_bt_any_connect_pending(void)
{
    uint8_t i;

    for (i = 0U; i < APP_HEADSET_COUNT; i++)
    {
        if (g_connect_timer_armed[i])
        {
            return true;
        }
    }
    return false;
}

/* Background HFP acquisition timers are per-slot and independent of the
 * connect timer: they run while the slot is already READY (music playing)
 * and must not hold the g_connecting_slot latch. */
static void app_bt_hfp_timer_start(int slot_index, uint32_t timeout_ms)
{
    rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);

    if (g_hfp_timers[slot_index] == RT_NULL)
    {
        return;
    }
    rt_timer_stop(g_hfp_timers[slot_index]);
    rt_timer_control(g_hfp_timers[slot_index], RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(g_hfp_timers[slot_index]);
}

static void app_bt_hfp_timer_stop(int slot_index)
{
    if (g_hfp_timers[slot_index] != RT_NULL)
    {
        rt_timer_stop(g_hfp_timers[slot_index]);
    }
}

static void app_bt_hfp_timeout(void *parameter)
{
    uint8_t slot_id = *(uint8_t *)parameter;

    if ((slot_id < APP_HEADSET_COUNT) &&
            (app_bt_post_timer_event(APP_BT_CMD_HFP_TIMEOUT, (int8_t)slot_id,
                                     0U) != RT_EOK))
    {
        rt_kprintf("[intercom] failed to queue HFP timeout\n");
    }
}

/* Role-seize timers are per-slot one-shots, independent of the connect
 * timer (they run while the slot may already be READY). First tick reads
 * the discovered role and fires the raw Switch Role; later ticks are the
 * "still slave?" re-check before the one disconnect/reconnect fallback. */
static void app_bt_role_timer_start(int slot_index, uint32_t timeout_ms)
{
    rt_tick_t ticks = rt_tick_from_millisecond(timeout_ms);

    if (g_role_timers[slot_index] == RT_NULL)
    {
        return;
    }
    rt_timer_stop(g_role_timers[slot_index]);
    rt_timer_control(g_role_timers[slot_index], RT_TIMER_CTRL_SET_TIME, &ticks);
    rt_timer_start(g_role_timers[slot_index]);
}

static void app_bt_role_timer_stop(int slot_index)
{
    if (g_role_timers[slot_index] != RT_NULL)
    {
        rt_timer_stop(g_role_timers[slot_index]);
    }
}

static void app_bt_role_timeout(void *parameter)
{
    uint8_t slot_id = *(uint8_t *)parameter;

    if ((slot_id < APP_HEADSET_COUNT) &&
            (app_bt_post_timer_event(APP_BT_CMD_ROLE_RECHECK, (int8_t)slot_id,
                                     0U) != RT_EOK))
    {
        rt_kprintf("[intercom] failed to queue role re-check\n");
    }
}

/* Single choke point for AG->HF connect requests. Refuses to fire a
 * second request while one is in flight (a talk acceleration landing 4ms
 * after a retry made both die with AG res 19) and treats a middleware
 * "busy" answer (the remote's own SLC is mid-setup) as a short-poll
 * deferral rather than a burnt try. */
static void app_bt_hfp_send_request(int slot_index, const char *why)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bt_err_t error;

    if (slot->hfp_request_in_flight)
    {
        rt_kprintf("[intercom] headset %u HFP request already in flight — %s deferred\n",
                   slot_index + 1U, why);
        return;
    }
    error = bt_interface_conn_to_source_ext(slot->mac.addr, BT_PROFILE_HFP);
    if (error == (bt_err_t)BT_ERROR_IN_PROGRESS)
    {
        /* The remote is opening the SLC itself right now. Do not count
         * this as a try — its own SLC usually lands within a second; if
         * it fails, the failure notify restarts acquisition. */
        rt_kprintf("[intercom] headset %u HFP request suppressed: remote SLC in progress (%s)\n",
                   slot_index + 1U, why);
        slot->hfp_deferred = true;
        app_bt_hfp_timer_start(slot_index, APP_BT_HFP_RETRY_FAST_MS);
        return;
    }
    if (error != BT_EOK)
    {
        rt_kprintf("[intercom] headset %u HFP request failed: %d (%s)\n",
                   slot_index + 1U, (int)error, why);
        slot->hfp_deferred = true;
        app_bt_hfp_timer_start(slot_index, APP_BT_HFP_RETRY_FAST_MS);
        return;
    }
    slot->hfp_request_in_flight = true;
    slot->hfp_tries++;
    rt_kprintf("[intercom] headset %u HFP request %u/%u sent (%s)\n",
               slot_index + 1U, slot->hfp_tries, APP_BT_HFP_RETRY_MAX, why);
    app_bt_hfp_timer_start(slot_index, APP_BT_HFP_RETRY_INTERVAL_MS);
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
    slot->av_deferred = false;
    slot->av_resend_tries = 0U;
    slot->gate_waits = 0U;
    slot->connect_attempt_tick = rt_tick_get();
    bt_interface_set_scan_mode(0U, 0U);
    /* Proven-stable connect flow (commit 6269331a2): AVDTP immediately,
     * 10s watchdog. The carrier/defer/grace complexity that accumulated
     * here was band-aids for bugs now fixed at their roots (a2dp_reset
     * cancelling foreign attempts, conn_ind missing role, shared connect
     * timer, double create). The remote-open preference stays only on the
     * incoming-accept path (begin_accept_wait). */
    error = bt_interface_conn_to_source_ext(slot->mac.addr, BT_PROFILE_A2DP);
    if (error != BT_EOK)
    {
        rt_kprintf("[intercom] A2DP connect request failed: %d\n",
                   (int)error);
        app_bt_reconnect_enqueue(&slot->mac);
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

/* Accepted an incoming ACL: do NOT send our own AVDTP yet — give the
 * headset APP_BT_ACCEPT_AVDTP_GRACE_MS to open AVDTP itself (headsets
 * that page us usually drive their own profiles). The CONNECT_TIMEOUT
 * handler kicks our side if the remote stays silent; until then the
 * watchdog is armed so a dead peer still gets cleaned up. */
static int app_bt_begin_accept_wait(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];

    app_bt_connect_timer_stop(slot_index);
    slot->connection_generation = app_bt_next_connection_generation();
    slot->connection_stage = APP_BT_CONN_A2DP_PENDING;
    slot->av_deferred = true;
    slot->av_resend_tries = 0U;
    slot->gate_waits = 0U;
    app_bt_connect_timer_start(slot_index, APP_BT_ACCEPT_AVDTP_GRACE_MS);
    app_state_set_mode(APP_MODE_CONNECTING);
    rt_kprintf("[intercom] headset %u ACL up; waiting for remote AVDTP first (%dms grace)\n",
               slot_index + 1U, APP_BT_ACCEPT_AVDTP_GRACE_MS);
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
    bool was_connect_attempt;

    if (slot->connection_stage == APP_BT_CONN_MEDIA_STOP_PENDING)
    {
        app_bt_cancel_deferred_connect(slot_index);
        return;
    }

    /* Only an attempt that never reached READY counts as "ours": its
     * ensuing ACL disconnect must not re-enter the reconnect queue
     * (fail → disconnect → re-enqueue → fail loops forever).
     * A READY connection torn down here (headset powered off mid-use,
     * HFP lost → "rolling back") must keep release_pending false so the
     * ACL disconnect re-queues it and the headset is picked up again —
     * by our slow retry or by its own incoming page — when it is back. */
    was_connect_attempt =
        (slot->connection_stage == APP_BT_CONN_A2DP_PENDING) ||
        (slot->connection_stage == APP_BT_CONN_HFP_PENDING);
    slot->connection_stage = APP_BT_CONN_ABORTING;
    app_bt_connect_timer_stop(slot_index);
    if (app_bt_slot_has_link(slot))
    {
        if (was_connect_attempt)
        {
            slot->release_pending = true;
        }
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
    int blocker = -1;
    app_mode_t mode = app_state_get_mode();

    if (g_scanning || (mode == APP_MODE_ERROR) ||
            ((mode != APP_MODE_MEDIA) && (mode != APP_MODE_CONNECTING)))
    {
        rt_kprintf("[intercom] talk command rejected in state %s\n",
                   app_state_mode_name(mode));
        g_talk_pending = false;
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
            if (blocker < 0)
            {
                blocker = (int)index;
            }
            if (g_slots[index].used && g_slots[index].a2dp_connected &&
                    !g_slots[index].hfp_connected &&
                    g_slots[index].hfp_deferred)
            {
                /* Talk was requested while this headset's HFP is still in
                 * background acquisition. Kick the HFP request NOW instead
                 * of waiting out the defer — app_bt_hfp_send_request
                 * refuses to double-fire into a request that is already
                 * outstanding or into the remote's own in-flight SLC. */
                g_slots[index].hfp_deferred = false;
                app_bt_hfp_timer_stop(index);
                app_bt_hfp_send_request((int)index, "talk");
            }
        }
    }
    if (!ready || !app_bt_all_sco_quiescent())
    {
        if (!g_talk_pending)
        {
            g_talk_pending = true;
            g_talk_pending_tick = rt_tick_get();
        }
        if (blocker >= 0)
        {
            rt_kprintf("[intercom] talk queued — will start automatically when headset %d is ready (HFP connecting)\n",
                       blocker + 1);
        }
        else
        {
            rt_kprintf("[intercom] talk queued — will start automatically when the link settles\n");
        }
        return;
    }

    g_talk_pending = false;
    g_restore_media = g_media_desired || app_bt_media_active();
    g_talk_recovery_attempted = false;
    g_error_cleanup_reported = false;
    g_talk_stop_settling = false;
    app_state_set_mode(APP_MODE_TALK_STARTING);
    g_talk_waiting_for_media = true;
    g_talk_media_sequence = app_bt_request_media_stop();
    app_bt_talk_timer_start(APP_BT_MEDIA_CLOSE_TIMEOUT_MS);
    rt_kprintf("[intercom] talk start accepted — waiting for both A2DP streams to stop\n");
}

/* A previously queued talk request becomes startable the moment the last
 * blocker clears (HFP lands / second headset READY / a pending connect
 * resolves). Called from the success paths — cheap no-op when nothing is
 * queued. */
static void app_bt_kick_queued_talk(void)
{
    if (!g_talk_pending)
    {
        return;
    }
    if ((rt_tick_get() - g_talk_pending_tick) >=
            rt_tick_from_millisecond(APP_BT_TALK_QUEUED_TIMEOUT_MS))
    {
        g_talk_pending = false;
        rt_kprintf("[intercom] queued talk expired (30s) — press 'intercom talk start' again\n");
        return;
    }
    app_bt_start_talk();
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
                    !g_connect_timer_armed[slot_index] ||
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
    bool pending_with_link = false;
    uint8_t i;

    if (g_scanning || (mode == APP_MODE_SCANNING))
    {
        /* Self-heal: an incoming page escapes a running inquiry and its
         * completion event never arrives — the stuck scan flag (and the
         * mode pinned after it) used to make every later `intercom scan`
         * fail as "busy" / "in state scanning" forever. Drop the stale
         * state and start over. */
        if (g_scanning)
        {
            bt_interface_stop_inquiry();
            g_scanning = false;
        }
        app_bt_update_activity_state();
        mode = app_state_get_mode();
        rt_kprintf("[intercom] clearing stalled scan state\n");
    }
    if ((mode != APP_MODE_IDLE) && (mode != APP_MODE_MEDIA) &&
            (mode != APP_MODE_CONNECTING))
    {
        rt_kprintf("[intercom] scan rejected in state %s\n",
                   app_state_mode_name(mode));
        return;
    }
    /* Both headsets already connected — nothing left to discover, and
     * inquiry radio time would only glitch the live streams. */
    if (app_bt_a2dp_count() >= 2U)
    {
        rt_kprintf("[intercom] scan rejected: both headsets are already connected\n");
        return;
    }
    /* A pending attempt that already HAS a link is a real profile setup
     * in progress — refuse rather than tear it down mid-handshake. */
    for (i = 0U; i < APP_HEADSET_COUNT; i++)
    {
        if (g_connect_timer_armed[i] && app_bt_slot_has_link(&g_slots[i]))
        {
            pending_with_link = true;
        }
    }
    if (pending_with_link)
    {
        rt_kprintf("[intercom] scan rejected: a headset connection is being established\n");
        return;
    }
    /* An in-flight page to an absent headset must not block the operator's
     * scan (after a reset with headsets in their case this was "scan
     * rejected: device is not ready or is busy" for minutes). Abort the
     * link-less attempts — the reconnect queue re-pages them once the
     * scan finishes (reconnect_try_next defers while mode == SCANNING). */
    for (i = 0U; i < APP_HEADSET_COUNT; i++)
    {
        if (g_connect_timer_armed[i] && !app_bt_slot_has_link(&g_slots[i]))
        {
            rt_kprintf("[intercom] deferring headset %u auto-connect for scan\n",
                       i + 1U);
            app_bt_abort_connection(i);
            app_bt_disconnect_timer_cancel(i);
            app_bt_clear_slot(i);
        }
    }
    app_bt_refresh_leds();

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
    rt_kprintf("[intercom] inquiry started for about x seconds\n");
}

/* Connect without suspending the running A2DP share first: the share
 * pump's reconcile path starts the newly opened link (hot-join) while the
 * existing headset keeps streaming, so music is not interrupted. */
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
    if (app_bt_any_connect_pending())
    {
        if (event->id != APP_BT_CMD_CONNECT_MANUAL)
        {
            rt_kprintf("[intercom] another headset connection is still pending\n");
            return;
        }
        {
            /* Manual connect is an explicit operator decision: cancel
             * every in-flight attempt that is NOT the target device (it
             * may be endless accept/grace churn on a dying headset) and
             * take over. Same-device attempts fall through to the
             * per-slot checks below. */
            int target_slot =
                app_bt_find_slot_by_mac(&event->data.command_device.mac);
            uint8_t i;

            for (i = 0U; i < APP_HEADSET_COUNT; i++)
            {
                if (!g_connect_timer_armed[i] || ((int)i == target_slot))
                {
                    continue;
                }
                rt_kprintf("[intercom] cancelling pending headset %u connection for manual connect\n",
                           i + 1U);
                app_bt_abort_connection(i);
                app_bt_disconnect_timer_cancel(i);
                app_bt_clear_slot(i);
                app_bt_refresh_leds();
            }
        }
    }

    /* Outgoing page + AVDTP setup steals radio time from a live A2DP
     * link. With a single radio this brief glitch is unavoidable — warn
     * but proceed (queuing deadlocks when media never goes idle). */
    if (app_bt_media_active() && (app_bt_a2dp_count() > 0U))
    {
        rt_kprintf("[intercom] connecting while music is playing; brief audio interruption expected\n");
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
        bool fully_up = slot->a2dp_connected &&
                        (slot->connection_stage == APP_BT_CONN_READY) &&
                        app_bt_slot_has_link(slot);

        if ((event->id != APP_BT_CMD_CONNECT_MANUAL) || fully_up)
        {
            rt_kprintf("[intercom] headset %u is already connecting/connected\n",
                       slot_index + 1U);
            return;
        }
        /* Duplicate press while the first attempt is still fresh (two
         * `intercom connect 1` 376ms apart in the 9-29 16:57 log): taking
         * it over cancelled an in-flight Create Connection and the cancel
         * raced its completion — ghost hdl 65535 res 2 twice and both
         * attempts died. Refuse the duplicate; attempts older than 2s can
         * still take over for wedge recovery as before. */
        if (g_connect_timer_armed[slot_index] &&
                ((rt_tick_get() - slot->connect_attempt_tick) <
                 rt_tick_from_millisecond(2000U)))
        {
            rt_kprintf("[intercom] headset %u attempt just started; duplicate connect ignored\n",
                       slot_index + 1U);
            return;
        }
        /* Manual connect is explicit operator intent: take over the
         * half-dead attempt on the SAME device (a wedged attempt used to
         * reject every recovery command until it timed out). */
        rt_kprintf("[intercom] headset %u taking over stale attempt\n",
                   slot_index + 1U);
        app_bt_connect_timer_stop(slot_index);
        app_bt_disconnect_timer_cancel(slot_index);
        app_bt_abort_connection(slot_index);
        app_bt_clear_slot(slot_index);
        app_bt_refresh_leds();
    }

    if (event->id == APP_BT_CMD_CONNECT_MANUAL)
    {
        /* Manual connect = (re)bond from scratch: drop any stored bond and
         * link key for this address first so the new link pairs cleanly
         * instead of racing a stale key (KEYMISSING → unpair). The fresh
         * bond is written again when the pairing completes.
         * Auto-reconnect (APP_BT_CMD_CONNECT) must never do this — it
         * relies on the existing bond to re-link without pairing. */
        bool had_bond = app_bt_is_bonded_earphone(
                            &event->data.command_device.mac);

        app_bt_reconnect_remove_mac(&event->data.command_device.mac);
        bt_cm_delete_bonded_devs_and_linkkey(
            (uint8_t *)event->data.command_device.mac.addr);
        if (had_bond)
        {
            /* The unpair is an async message to the BT task: paling
             * immediately raced it and the new link could authenticate
             * with the old key BEFORE it was deleted — the key then
             * vanished mid-session, the headset dropped the link and its
             * next page failed authentication (observed as "connected but
             * no name/no sound" until the link died). Let the unpair
             * settle before connecting. */
            rt_thread_mdelay(300);
        }
        rt_kprintf("[intercom] cleared stored bond for ");
        app_bt_print_mac(&event->data.command_device.mac);
        rt_kprintf("\n");
    }

    {
        /* Auto-reconnect posts an empty name; keep the one we learned
         * from the original discovery so `intercom status` still shows
         * it instead of <unknown>. */
        char saved_name[APP_BT_NAME_LENGTH] = {0};

        strncpy(saved_name, slot->used ? slot->name : "",
                sizeof(saved_name) - 1U);
        app_bt_clear_slot(slot_index);
        slot->used = true;
        slot->mac = event->data.command_device.mac;
        if (event->data.command_device.name[0] != '\0')
        {
            strncpy(slot->name, event->data.command_device.name,
                    sizeof(slot->name) - 1U);
        }
        else
        {
            strncpy(slot->name, saved_name, sizeof(slot->name) - 1U);
        }
    }

    app_bt_begin_a2dp_connect(slot_index);
}

static void app_bt_handle_disconnect_command(const app_bt_event_t *event)
{
    int slot_index = app_bt_find_slot_by_mac(&event->data.command_device.mac);

    /* Manual disconnect = forget this headset completely: the stored
     * bond/link key and any reconnect-queue entry go away with the link.
     * Reset-reconnect then only ever covers the headsets the user has not
     * explicitly disconnected (at most the 2 in the bond table). */
    app_bt_reconnect_remove_mac(&event->data.command_device.mac);
    bt_cm_delete_bonded_devs_and_linkkey(
        (uint8_t *)event->data.command_device.mac.addr);
    rt_kprintf("[intercom] forgetting headset ");
    app_bt_print_mac(&event->data.command_device.mac);
    rt_kprintf("\n");

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

    /* An incoming page escapes a running inquiry and the completion event
     * never arrives — drop the stale scan flag here so `intercom scan`
     * keeps working. */
    if (g_scanning)
    {
        g_scanning = false;
        app_bt_update_activity_state();
    }

    if (slot_index < 0)
    {
        /* Incoming ACL we did not start: accept it when it comes from a
         * queued (or at least previously bonded) earphone — e.g. headset
         * powered on and paged us while page scan is enabled during
         * reconnection — and continue the normal profile-connect flow
         * over this ACL instead of rejecting it.
         * NOTE: accepted even while another outgoing attempt is in
         * flight — each slot carries its own stage/generation and the
         * in-flight page converges through the stack's own failure
         * event; blocking here made us reject a perfectly good incoming
         * page from the other queued headset. */
        if ((info->res == HCI_SUCC) &&
                app_bt_may_accept_incoming(&info->mac) &&
                app_bt_incoming_acceptable())
        {
            slot_index = app_bt_find_free_slot();
            if (slot_index >= 0)
            {
                rt_kprintf("[intercom] accepting incoming ACL from queued headset: ");
                app_bt_print_mac(&info->mac);
                rt_kprintf("\n");
                /* Seed the queue (dedup for members) so a bonded-only
                 * incoming device is subject to the same back-off policy
                 * if this connection attempt fails. */
                app_bt_reconnect_enqueue(&info->mac);
                app_bt_clear_slot(slot_index);
                g_slots[slot_index].used = true;
                g_slots[slot_index].mac = info->mac;
                app_bt_begin_accept_wait(slot_index);
            }
        }
        if (slot_index < 0)
        {
            bt_notify_device_mac_t mac = info->mac;

            rt_kprintf("[intercom] rejecting unassigned ACL device: ");
            app_bt_print_mac(&info->mac);
            rt_kprintf(" (in_queue=%d bonded=%d acceptable=%d res=%u)\n",
                       app_bt_mac_in_queue(&info->mac) ? 1 : 0,
                       app_bt_is_bonded_earphone(&info->mac) ? 1 : 0,
                       app_bt_incoming_acceptable() ? 1 : 0, info->res);
            if (info->res == HCI_SUCC)
            {
                bt_interface_disconnect_req(mac.addr);
            }
            return;
        }
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
        if ((info->res == HCI_SUCC) &&
                app_bt_may_accept_incoming(&slot->mac) &&
                app_bt_incoming_acceptable())
        {
            /* Our outgoing attempt just failed/left the slot in abort
             * state, but the headset's own page won the race a moment
             * later — this is exactly the power-on reconnect. Recover
             * instead of murdering the perfectly good ACL. */
            rt_kprintf("[intercom] incoming ACL won the race; recovering headset %u\n",
                       slot_index + 1U);
            app_bt_connect_timer_stop(slot_index);
            app_bt_disconnect_timer_cancel(slot_index);
            slot->release_pending = false;
            slot->connection_stage = APP_BT_CONN_IDLE;
            app_bt_begin_accept_wait(slot_index);
        }
        else
        {
            rt_kprintf("[intercom] rejecting late ACL success for headset %u\n",
                       slot_index + 1U);
            app_bt_abort_connection(slot_index);
        }
    }
    else if (!app_bt_connection_pending(slot) &&
             (slot->connection_stage != APP_BT_CONN_READY))
    {
        if ((slot->connection_stage == APP_BT_CONN_IDLE) &&
                app_bt_may_accept_incoming(&slot->mac) &&
                app_bt_incoming_acceptable())
        {
            /* Slot still assigned after an earlier drop (stage IDLE) and
             * the headset paged us back in — continue the connect flow
             * over this ACL instead of rejecting it as "unexpected". */
            rt_kprintf("[intercom] accepting incoming ACL for waiting headset %u\n",
                       slot_index + 1U);
            app_bt_begin_accept_wait(slot_index);
        }
        else
        {
            rt_kprintf("[intercom] rejecting unexpected ACL success for headset %u\n",
                       slot_index + 1U);
            app_bt_abort_connection(slot_index);
        }
    }
    if ((info->res == HCI_SUCC) && slot->av_deferred &&
            (slot->connection_stage == APP_BT_CONN_A2DP_PENDING))
    {
        /* Outgoing connect with AVDTP deferred: give the remote the same
         * accept-wait grace as an incoming ACL so its own AVDTP open wins
         * cleanly instead of colliding with ours. */
        app_bt_begin_accept_wait(slot_index);
    }
    rt_kprintf("[intercom] headset %u ACL %s\n", slot_index + 1U,
               (info->res == HCI_SUCC) ? "connected" : "failed");
    if (info->res == HCI_SUCC)
    {
        /* Make the supervision window explicit (the stack's own write
         * comes from the prebuilt lib with an unknown value). */
        BTS2S_BD_ADDR addr;

        if (bt_addr_convert_to_bts((bd_addr_t *)info->mac.addr, &addr) == 0U)
        {
            gap_wr_super_vision_timeout_req(bts2_task_get_app_task_id(),
                                            addr,
                                            APP_BT_LINK_SUPERV_TIMEOUT);
        }
        /* Audio link: prohibit Sniff immediately at ACL-up (role-switch
         * only, 0x01) and kick the link out of Sniff if it is already
         * there. The headset case-reconnect used to idle into Sniff in
         * this very window (bt_cm arms a 5s-idle sniff policy on every
         * connect) and the A2DP stream later played silent. */
        bt_interface_wr_link_policy_setting(info->mac.addr, 0x01);
        bt_interface_exit_sniff_mode(info->mac.addr);
        /* Role-seize watch (S2/S3): Connection Complete carries no role
         * and bt_cm's info.role still defaults to master here — the real
         * role only lands with the stack's role discovery ~0.5-1s later.
         * Arm a re-check instead of deciding now; outgoing links (pager
         * = always master) make the check a no-op. */
        app_bt_role_timer_start(slot_index, APP_BT_ROLE_DISC_MS);
        /* Page time must not eat the pairing/profile budget: the watchdog
         * started at the connect command, so an 8s page left only 2s for
         * SSP + AVDTP and the attempt was aborted 355ms after the fresh
         * bond was written (9-29 16:53 log) — that half-committed pairing
         * is what desynced the link keys (auth failure / KEYMISSING /
         * reason 19 loop). Now that the ACL exists, give the profile
         * phase its own full window; A2DP success stops the timer. */
        if (app_bt_connection_pending(slot) && !slot->av_deferred)
        {
            app_bt_connect_timer_start(slot_index, APP_BT_CONNECT_TIMEOUT_MS);
        }
        /* Fetch the remote name now so `intercom status` shows it even
         * if the A2DP/HFP profile setup later fails half-way. */
        // app_bt_request_remote_name(slot_index); // moved to after AVDTP to avoid HCI race
    }
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
    if (((info->res == 0x05U) || (info->res == 0x06U)) &&
            app_bt_is_bonded_earphone(&info->mac))
    {
        /* Authentication Failure (0x05) / PIN or Key Missing (0x06):
         * the stored key itself is bad — retrying with it can never
         * recover. This is the ONLY signal that justifies deleting a
         * bond; a plain A2DP timeout or a remote power-off keeps it. */
        bt_cm_delete_bonded_devs_and_linkkey((uint8_t *)info->mac.addr);
        rt_kprintf("[intercom] headset %u bond dropped (auth failure, reason=%u); next attempt re-pairs\n",
                   slot_index + 1U, info->res);
    }
    if (!release_slot)
    {
        /* Unexpected drop (headset powered off / out of range): queue
         * for auto-reconnect... */
        app_bt_reconnect_enqueue(&info->mac);
        if (!app_bt_is_talk_mode(mode) && (mode != APP_MODE_ERROR))
        {
            /* ...but do not leave a dead entry behind: `intercom status`
             * must not keep showing a device that is gone. Reconnect
             * needs only the queue; an incoming page from the headset
             * is accepted later via the unassigned-slot path. (Talk and
             * error flows keep the old behaviour of retaining the slot.) */
            app_bt_clear_slot(slot_index);
        }
    }
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

/* True once the link key has been verified (encryption up). Profile
 * L2CAP connects sent before that vanish without a trace on some
 * headsets (slow authentication cases) and the link is later torn down
 * by our own timeout — gate AVDTP/HFP initiates on this. */
static bool app_bt_link_encrypted(const bt_notify_device_mac_t *mac)
{
    bt_cm_device_manager_t *manager = bt_cm_get_env();
    BTS2S_BD_ADDR address;
    bt_cm_dev_acl_info_t *connection;

    if (bt_addr_convert_to_bts((bd_addr_t *)mac, &address) != 0U)
    {
        return false;
    }
    if (manager == RT_NULL)
    {
        return false;
    }
    connection = bt_cm_get_conn_by_addr(manager, &address);
    return (connection != RT_NULL) && (connection->encrypted != 0U);
}

/* True while a Create Connection for this device is still in flight.
 * Issuing a second one on top collides in the controller ("res 2/8"
 * unknown-connection failures kill BOTH attempts) — re-arm instead. */
static bool app_bt_acl_creating(const bt_notify_device_mac_t *mac)
{
    bt_cm_device_manager_t *manager = bt_cm_get_env();
    BTS2S_BD_ADDR address;
    bt_cm_dev_acl_info_t *connection;

    if (bt_addr_convert_to_bts((bd_addr_t *)mac, &address) != 0U)
    {
        return false;
    }
    if (manager == RT_NULL)
    {
        return false;
    }
    connection = bt_cm_get_conn_by_addr(manager, &address);
    return (connection != RT_NULL) &&
           (connection->state == BT_CM_ACL_STATE_CONNECTING);
}

static bool app_bt_role_bounce_used(const bt_notify_device_mac_t *mac)
{
    uint8_t i;

    for (i = 0U; i < g_role_bounce_count; i++)
    {
        if (app_bt_mac_equal(&g_role_bounce_used[i], mac))
        {
            return true;
        }
    }
    return false;
}

static void app_bt_role_bounce_mark(const bt_notify_device_mac_t *mac)
{
    if (g_role_bounce_count < APP_HEADSET_COUNT)
    {
        g_role_bounce_used[g_role_bounce_count] = *mac;
        g_role_bounce_count++;
    }
}

/* S2: post-hoc master seize for a link that came up as slave.
 *
 * bt_send_switch_role() (gap_switch_role_req) never reaches the air on
 * these links: gap_dm_switch_role_msg_hdl locally rejects the request
 * (its ACL cache reads the slave link's cached role as "already master",
 * and req_role == cur_role quick-fails) and the app's ROLE_SWITCH_CFM
 * returns res:1 in the SAME millisecond — a real LMP role switch needs
 * at least one 1.25ms baseband slot, so that CFM is provably local.
 * Send the raw HCI Switch Role instead — the exact command GAP itself
 * issues when its cache is right. Completion returns as
 * BTS2MU_GAP_ROLE_SWITCH_IND (GAP has no pending request to answer with
 * a CFM) and bt_cm updates connection->info.role from it. */
static bool app_bt_seize_master_role(int slot_index, const char *why)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    bt_cm_device_manager_t *manager = bt_cm_get_env();
    bt_cm_dev_acl_info_t *connection;
    BTS2S_BD_ADDR address;

    if (!slot->used || !slot->acl_connected)
    {
        return false;
    }
    if (manager == RT_NULL)
    {
        return false;
    }
    if (bt_addr_convert_to_bts((bd_addr_t *)&slot->mac, &address) != 0U)
    {
        return false;
    }
    connection = bt_cm_get_conn_by_addr(manager, &address);
    if ((connection == RT_NULL) || (connection->info.role != BT_LINK_SLAVE))
    {
        return false;
    }
    hcia_switch_role(&address, 0, RT_NULL);
    slot->role_seize_sent = true;
    rt_kprintf("[intercom] headset %u master seize sent (%s)\n",
               slot_index + 1U, why);
    app_bt_role_timer_start(slot_index, APP_BT_ROLE_RECHECK_MS);
    return true;
}

static void app_bt_prepare_acl_for_audio(const bt_notify_device_mac_t *mac)
{
    int slot_index = app_bt_find_slot_by_mac(mac);

    /* Audio link policy: role-switch ONLY (0x01). This used to write
     * 0x05 = role-switch|SNIFF — i.e. it explicitly ALLOWED the sniff
     * the headset then slid into after reconnect, and the stream went
     * silent ("streaming but no audio"). The SNIFF bit (0x04) stays
     * cleared for the whole life of the link; exit sniff in case the
     * link is already there. Written for both master and slave roles —
     * the old slave-only guard left master links unprotected. */
    bt_interface_wr_link_policy_setting((unsigned char *)mac->addr, 0x01);
    bt_interface_exit_sniff_mode((unsigned char *)mac->addr);
    if (slot_index >= 0)
    {
        app_bt_seize_master_role(slot_index, "A2DP ready");
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
            if (g_media_desired)
            {
                /* Reconnected while music is desired: actively notify
                 * PLAYING so the headset's CT side also considers the
                 * stream live (some buds keep their sink muted until
                 * their play-status view says PLAYING). */
                bt_interface_avrcp_set_playback_status(
                    AVRCP_PLAY_STATUS_PLAYING, &info->mac);
                rt_kprintf("[intercom] headset %u AVRCP play status -> PLAYING\n",
                           slot_index + 1U);
            }
            /* Push the desired music volume right after AVRCP is up —
             * many buds come back from a reconnect with absolute volume
             * 0 (muted) and without an explicit push they stay silent. */
            slot->remote_music_volume_desired = 5U;
            slot->remote_music_volume_retries = 0U;
            slot->remote_music_volume_pending = true;
            app_bt_schedule_remote_music_volume();
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
            slot->av_deferred = false; /* remote (or we) opened AVDTP */
            slot->av_resend_tries = 0U;
            app_bt_mark_seen(&slot->mac);
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

            /* A2DP alone is enough for READY: music, AVRCP and the remote
             * name must work even when HFP lags or never lands (our
             * outgoing AG→HF connect wedges on these headsets; their own
             * SLC often arrives seconds later). */
            slot->connection_stage = APP_BT_CONN_READY;
            app_bt_prepare_acl_for_audio(&info->mac);
            app_bt_connect_timer_stop(slot_index);
            slot->restore_media_after_connect = false;
            rt_kprintf("[intercom] headset %u A2DP Source connected\n",
                       slot_index + 1U);
            app_bt_update_media_for_links();
            app_bt_request_avrcp(slot_index);
            app_bt_request_remote_name(slot_index);
            if (!slot->hfp_connected)
            {
                /* Background HFP acquisition: give the headset a window to
                 * open SLC itself first — our AG→HF request fired into its
                 * mid-flight SLC collides ("conn rej psm 19" / AG res 19)
                 * and wedges the link until it drops (Redmi Buds 4 Pro). */
                slot->hfp_deferred = true;
                slot->hfp_tries = 0U;
                slot->gate_waits = 0U;
                app_bt_hfp_timer_start(slot_index,
                                       APP_BT_HFP_DEFER_GRACE_MS);
                rt_kprintf("[intercom] headset %u HFP deferred %ums; waiting for the remote to open SLC first\n",
                           slot_index + 1U, APP_BT_HFP_DEFER_GRACE_MS);
            }
            /* Kick the share immediately: start streaming to this newly
             * opened link instead of waiting for the next pump tick —
             * "connected but no music" ends here when media is desired. */
            if ((mode != APP_MODE_ERROR) && !app_bt_is_talk_mode(mode))
            {
                app_a2dp_share_reconcile_streams();
            }
            app_bt_kick_queued_talk();
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

        if ((info->res == AV_CONNECT_CONFLICT) ||
                (info->res == AV_DUPLICATE_CONNECTING))
        {
            /* "Another connect in progress" — in real logs this is the
             * REMOTE's own AVDTP open moving through the AV library, and
             * it LANDED 1-2s later every time we left it alone (20:01
             * session: 9b then "A2DP Source connected" 1.4s later).
             * Never tear anything down on 9a/9b — killing the link here
             * murders the in-flight connect. If an attempt is pending,
             * give it one more watchdog window to land. */
            if (app_bt_connection_pending(slot))
            {
                app_bt_connect_timer_start(slot_index,
                                           APP_BT_CONNECT_TIMEOUT_MS);
            }
            rt_kprintf("[intercom] headset %u A2DP conflict %u ignored; keeping the link\n",
                       slot_index + 1U, (unsigned int)info->res);
            return;
        }
        slot->a2dp_connected = false;
        rt_kprintf("[intercom] headset %u A2DP disconnected (res=%u)\n",
                   slot_index + 1U, (unsigned int)info->res);
        if (talk_affected)
        {
            slot->connection_stage = APP_BT_CONN_ABORTING;
            slot->profile_cleanup_pending = true;
            slot->disconnect_blocks_media = true;
            app_bt_begin_talk_stop();
        }
        else if ((mode != APP_MODE_ERROR) && app_bt_slot_has_link(slot) &&
                 !slot->release_pending &&
                 (slot->av_resend_tries < APP_BT_AV_RESEND_MAX))
        {
            /* AVDTP flapped while the ACL is healthy — the remote often
             * closes and re-opens AVDTP around its own SLC bring-up
             * (Anyka dropped AVDTP 30ms after opening its HFP SLC).
             * Dropping the ACL here used to kill the remote's in-flight
             * SLC and wedge its AVDTP for minutes (every follow-up
             * attempt then died with 97). Re-send AVDTP after a short
             * back-off instead; if the remote is truly gone the ACL drop
             * follows and handle_acl_disconnected cleans up. */
            uint8_t other = (slot_index == 0) ? 1U : 0U;

            slot->av_resend_tries++;
            /* Route the re-send through the deferred branch of the
             * connect watchdog: it re-issues AVDTP after the delay and
             * re-arms the 10s watchdog around it. The stage must move
             * back to A2DP_PENDING so the watchdog sees a live attempt
             * (READY would make it "clear the stale timeout" instead). */
            slot->connection_stage = APP_BT_CONN_A2DP_PENDING;
            slot->av_deferred = true;
            slot->gate_waits = 0U;
            if (!g_connect_timer_armed[other] &&
                    ((info->res == AV_CONN_ATTMPT_FAILED) ||
                     (info->res == AV_CONNECT_CONFLICT) ||
                     (info->res == AV_DUPLICATE_CONNECTING)))
            {
                a2dp_reset_req();
            }
            app_bt_connect_timer_start(slot_index,
                                       APP_BT_AV_RESEND_DELAY_MS);
            rt_kprintf("[intercom] headset %u A2DP re-send scheduled in %ums (attempt %u/%u)\n",
                       slot_index + 1U, APP_BT_AV_RESEND_DELAY_MS,
                       slot->av_resend_tries, APP_BT_AV_RESEND_MAX);
            app_bt_update_media_for_links();
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
            slot->hfp_deferred = false;
            slot->hfp_request_in_flight = false;
            app_bt_hfp_timer_stop(slot_index);
            slot->hfp_channel = info->profile_channel;
            if ((slot->connection_stage == APP_BT_CONN_A2DP_PENDING) &&
                    !slot->release_pending && !app_bt_is_talk_mode(mode) &&
                    (mode != APP_MODE_ERROR))
            {
                /* Remote (headset) established HFP before our AVDTP
                 * finished. Keep the stage at A2DP_PENDING and wait —
                 * READY is taken when A2DP lands (handle_a2dp). Aborting
                 * here killed reconnects that were actually succeeding
                 * and knocked the healthy headset's share into error. */
                rt_kprintf("[intercom] headset %u HFP AG connected early (A2DP pending), channel=%u\n",
                           slot_index + 1U, info->profile_channel);
                app_bt_refresh_leds();
                return;
            }
            if ((slot->connection_stage != APP_BT_CONN_READY) ||
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

            /* Late HFP fill on a READY (A2DP-only) link that has been
             * playing music already — just complete the profile set. */
            rt_kprintf("[intercom] headset %u HFP AG connected, channel=%u\n",
                       slot_index + 1U, info->profile_channel);
            app_bt_update_media_for_links();
            app_bt_request_avrcp(slot_index);
            app_bt_request_remote_name(slot_index);
            app_bt_refresh_leds();
            app_bt_kick_queued_talk();
        }
        else
        {
            rt_kprintf("[intercom] headset %u HFP AG connect failed: %u\n",
                       slot_index + 1U, info->res);
            slot->hfp_request_in_flight = false;
            if ((slot->connection_stage == APP_BT_CONN_READY) &&
                    slot->a2dp_connected && !slot->hfp_connected &&
                    slot->used)
            {
                /* Background HFP acquisition failed (res 19 = we fired
                 * into the remote's own in-flight SLC). That failure
                 * clears the air — come back fast instead of sitting out
                 * a full retry interval. */
                slot->hfp_deferred = true;
                app_bt_hfp_timer_start(slot_index, APP_BT_HFP_RETRY_FAST_MS);
                rt_kprintf("[intercom] headset %u HFP retry scheduled in %ums\n",
                           slot_index + 1U, APP_BT_HFP_RETRY_FAST_MS);
            }
            else if (slot->connection_stage == APP_BT_CONN_HFP_PENDING)
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
        slot->hfp_request_in_flight = false;
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
            /* Single-SCO loss during talk: the walkie-talkie relay needs
             * both sides, so one drop tears the session down. (A true
             * single-SCO fallback is not implemented.) */
            rt_kprintf("[intercom] talk abort: headset %u SCO dropped (was_active=%d was_relay=%d mode=%s); tearing down session\n",
                       slot_index + 1U, was_active ? 1 : 0,
                       was_relay ? 1 : 0,
                       app_state_mode_name(mode));
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
    case APP_BT_CMD_CONNECT_MANUAL:
        app_bt_handle_connect_command(event);
        break;
    case APP_BT_CMD_DISCONNECT:
        app_bt_handle_disconnect_command(event);
        break;
    case APP_BT_CMD_TALK_START:
        app_bt_start_talk();
        break;
    case APP_BT_CMD_TALK_STOP:
        if (g_talk_pending)
        {
            g_talk_pending = false;
            rt_kprintf("[intercom] queued talk cancelled\n");
        }
        if ((app_state_get_mode() == APP_MODE_TALK_STARTING) ||
                (app_state_get_mode() == APP_MODE_TALKING))
        {
            app_bt_begin_talk_stop();
        }
        else if (!g_talk_pending)
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
    case APP_BT_CMD_HFP_TIMEOUT:
        if ((event->data.timer.slot >= 0) &&
                ((uint8_t)event->data.timer.slot < APP_HEADSET_COUNT))
        {
            int slot_index = event->data.timer.slot;
            app_bt_slot_t *slot = &g_slots[slot_index];

            if (!slot->used || !slot->hfp_deferred || slot->hfp_connected)
            {
                slot->hfp_deferred = false;
                break;
            }
            if (slot->acl_connected && !app_bt_link_encrypted(&slot->mac) &&
                    (slot->gate_waits < APP_BT_GATE_WAITS_MAX))
            {
                /* Same rule as AVDTP: never start RFCOMM before the link
                 * is encrypted — the SABM vanishes without a trace. */
                slot->gate_waits++;
                app_bt_hfp_timer_start(slot_index, 2000U);
                break;
            }
            if (slot->hfp_tries >= APP_BT_HFP_RETRY_MAX)
            {
                /* Never tear down a link that is playing music. A late
                 * remote SLC is still accepted by handle_hfp; until then
                 * the headset is music-only (talk is rejected politely). */
                slot->hfp_deferred = false;
                slot->hfp_request_in_flight = false;
                rt_kprintf("[intercom] headset %u HFP unavailable; music-only until the headset opens SLC itself\n",
                           slot_index + 1U);
                break;
            }
            /* This timer tick means the previous request's window is
             * over — allow the retry to fire. */
            slot->hfp_request_in_flight = false;
            app_bt_hfp_send_request(slot_index, "retry");
        }
        break;
    case APP_BT_CMD_ROLE_RECHECK:
        if ((event->data.timer.slot >= 0) &&
                ((uint8_t)event->data.timer.slot < APP_HEADSET_COUNT))
        {
            int slot_index = event->data.timer.slot;
            app_bt_slot_t *slot = &g_slots[slot_index];
            bt_cm_device_manager_t *manager;
            bt_cm_dev_acl_info_t *connection;
            BTS2S_BD_ADDR address;
            app_mode_t mode = app_state_get_mode();

            if (!slot->used || !slot->acl_connected)
            {
                break;
            }
            if (app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR))
            {
                /* Bouncing now would kill SCO / error cleanup. */
                app_bt_role_timer_start(slot_index,
                                        APP_BT_ROLE_NOT_READY_RETRY_MS);
                break;
            }
            manager = bt_cm_get_env();
            if ((manager == RT_NULL) ||
                    (bt_addr_convert_to_bts((bd_addr_t *)&slot->mac,
                                            &address) != 0U))
            {
                break;
            }
            connection = bt_cm_get_conn_by_addr(manager, &address);
            if ((connection == RT_NULL) ||
                    (connection->info.role != BT_LINK_SLAVE))
            {
                /* Master confirmed — star topology, done. */
                break;
            }
            if (!slot->role_seize_sent)
            {
                /* First look at a slave role: try the raw seize and give
                 * it a whole window to land before considering the
                 * fallback. */
                app_bt_seize_master_role(slot_index, "role re-check");
                break;
            }
            if (slot->connection_stage != APP_BT_CONN_READY)
            {
                /* Seize already sent but profiles still settling —
                 * keep watching. */
                app_bt_role_timer_start(slot_index,
                                        APP_BT_ROLE_NOT_READY_RETRY_MS);
                break;
            }
            if (!app_bt_role_bounce_used(&slot->mac))
            {
                app_bt_role_bounce_mark(&slot->mac);
                rt_kprintf("[intercom] headset %u still slave after role seize; bouncing ACL for outgoing (master) reconnect\n",
                           slot_index + 1U);
                /* release_pending stays false on purpose: the ACL-down
                 * handler then re-queues this headset and the reconnect
                 * is outgoing — an outgoing connect is ALWAYS master
                 * (pager role), no role switch involved. One bounce per
                 * headset per boot; if the new link is slave again the
                 * topology is kept as-is so the other headset is not
                 * disturbed. */
                if (app_bt_issue_disconnect(slot_index) != 0)
                {
                    rt_kprintf("[intercom] headset %u role-bounce disconnect failed\n",
                               slot_index + 1U);
                }
            }
        }
        break;
    case APP_BT_CMD_CONNECT_TIMEOUT:
        if ((event->data.timer.slot >= 0) &&
                ((uint8_t)event->data.timer.slot < APP_HEADSET_COUNT))
        {
            int slot_index = event->data.timer.slot;
            app_bt_slot_t *slot = &g_slots[slot_index];

            if (!g_connect_timer_armed[slot_index] ||
                    (event->data.timer.generation !=
                     g_connect_timer_gen[slot_index]))
            {
                /* A newer connect attempt owns this slot's watchdog; this
                 * event is its stale predecessor's. */
                break;
            }
            if (!app_bt_connection_pending(slot))
            {
                /* State moved on without this timer being stopped — clear
                 * the latch instead of dropping silently: a dropped event
                 * used to pin g_connecting_slot forever ("another headset
                 * connection is still pending"), which also froze the
                 * whole reconnect queue. */
                rt_kprintf("[intercom] clearing stale connect timeout for headset %u\n",
                           slot_index + 1U);
                app_bt_connect_timer_stop(slot_index);
                break;
            }
            {
                if (slot->connection_stage ==
                        APP_BT_CONN_MEDIA_STOP_PENDING)
                {
                    rt_kprintf("[intercom] A2DP sharing suspend timed out before connecting headset %u\n",
                               slot_index + 1U);
                    app_bt_cancel_deferred_connect(slot_index);
                }
                else if (slot->av_deferred)
                {
                    /* Grace window expired without the remote opening
                     * AVDTP — take the initiator role now, but never
                     * before the link is encrypted: the request vanishes
                     * otherwise and the headset gets torn down by our own
                     * timeout (slow-auth headset cases). */
                    bt_err_t error;

                    if (slot->acl_connected &&
                            !app_bt_link_encrypted(&slot->mac) &&
                            (slot->gate_waits < APP_BT_GATE_WAITS_MAX))
                    {
                        slot->gate_waits++;
                        app_bt_connect_timer_start(slot_index, 2000U);
                        rt_kprintf("[intercom] headset %u AVDTP deferred; waiting for link encryption\n",
                                   slot_index + 1U);
                        break;
                    }
                    if (!slot->acl_connected &&
                            app_bt_acl_creating(&slot->mac) &&
                            (slot->gate_waits < APP_BT_GATE_WAITS_MAX))
                    {
                        /* The page is still running in the controller
                         * (Create Connection can take 15-20s when the
                         * remote is busy). Issuing another one on top
                         * collides and kills BOTH attempts — just wait. */
                        slot->gate_waits++;
                        app_bt_connect_timer_start(slot_index, 2000U);
                        rt_kprintf("[intercom] headset %u page still in flight; waiting\n",
                                   slot_index + 1U);
                        break;
                    }
                    slot->av_deferred = false;
                    error = bt_interface_conn_to_source_ext(
                                slot->mac.addr, BT_PROFILE_A2DP);
                    if (error != BT_EOK)
                    {
                        rt_kprintf("[intercom] headset %u deferred A2DP request failed: %d\n",
                                   slot_index + 1U, (int)error);
                        app_bt_abort_connection(slot_index);
                    }
                    else
                    {
                        slot->connection_generation =
                            app_bt_next_connection_generation();
                        app_bt_connect_timer_start(slot_index,
                                                   APP_BT_CONNECT_TIMEOUT_MS);
                        rt_kprintf("[intercom] headset %u remote AVDTP grace expired; initiating A2DP ourselves\n",
                                   slot_index + 1U);
                    }
                }
                else
                {
                    /* Page still running in the controller: cancelling it
                     * here raced the create completion and echoed the
                     * ghost "hdl 65535 res 2" invalid-handle status on
                     * every timed-out attempt (both 9-29 logs). Wait it
                     * out in bounded slices instead — the controller's
                     * own page timeout then settles the create with a
                     * real status, and a late success still lands (ACL-up
                     * re-arms the profile window above). */
                    if (!slot->acl_connected && app_bt_acl_creating(&slot->mac) &&
                            (slot->gate_waits < APP_BT_GATE_WAITS_MAX))
                    {
                        slot->gate_waits++;
                        app_bt_connect_timer_start(slot_index, 5000U);
                        rt_kprintf("[intercom] headset %u page still in flight; waiting\n",
                                   slot_index + 1U);
                        break;
                    }
                    /* Real timeout: hand the device back to the reconnect
                     * queue (its back-off re-drives a clean connect via
                     * APP_BT_CMD_CONNECT) instead of re-issuing in place. */
                    {
                        uint8_t other = (slot_index == 0) ? 1U : 0U;

                        if (!g_connect_timer_armed[other])
                        {
                            /* No other connect is in flight — clear the AV
                             * layer's stale request latch so the next
                             * attempt is not met with conflict/dupe.
                             * Skipped while a second headset is
                             * mid-connect: the reset would cancel ITS
                             * pending AVDTP too. */
                            a2dp_reset_req();
                        }
                    }
                    /* No bond deletion here: a plain A2DP timeout says
                     * nothing about the key. Deleting the bond used to
                     * turn one slow AVDTP into "rejecting unassigned ACL"
                     * on the headset's own re-page (bonded=0) and forced
                     * a manual re-pair. Auth/key failures delete the bond
                     * at the disconnect handler instead — that is the
                     * only signal that says the key itself is bad. */
                    rt_kprintf("[intercom] headset %u connection timed out; queued for retry\n",
                               slot_index + 1U);
                    app_bt_reconnect_enqueue(&slot->mac);
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
                slot->disconnect_blocks_media = false;
                if (!slot->release_pending &&
                        slot->a2dp_connected && slot->hfp_connected)
                {
                    /* Only a NON-teardown watchdog may restore READY.
                     * For an explicit forget/disconnect the slot must
                     * stay pending so the eventual drop is treated as a
                     * release — a slow peer's late detach used to run
                     * with release_pending cleared and resurrect the
                     * link (re-queued as if the user wanted it back). */
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
    {
        bool healthy = (app_bt_a2dp_count() != 0U);

        /* A teardown race (e.g. disconnecting an in-flight accept) can
         * push the share into ERROR while the OTHER headset is still
         * streaming. Do NOT clear media_desired here — that wedged
         * playback until some unrelated BT event happened to restart
         * the share. Keep desired in line with reality and drive a
         * bounded retry chain through the share's -EBUSY quiesce
         * window (share_start succeeds once the producer settles). */
        g_media_desired = healthy;
        rt_kprintf("[intercom] A2DP sharing error: %d (media desired=%d)\n",
                   (int32_t)event->data.token, healthy);
        if (healthy)
        {
            (void)app_a2dp_share_start();
            bts2_timer_ev_add(APP_BT_MEDIA_RECOVER_DELAY_MS,
                              app_bt_media_recover_bts_cb,
                              APP_BT_MEDIA_RECOVER_MAX, RT_NULL);
        }
        break;
    }
    case APP_BT_CMD_MEDIA_RECOVER:
        if ((event->data.token == 0U) || (app_bt_a2dp_count() == 0U))
        {
            break;
        }
        if (app_a2dp_share_start() != RT_EOK)
        {
            bts2_timer_ev_add(APP_BT_MEDIA_RECOVER_DELAY_MS,
                              app_bt_media_recover_bts_cb,
                              (uint16_t)(event->data.token - 1U),
                              RT_NULL);
        }
        else
        {
            rt_kprintf("[intercom] A2DP sharing recovered, media restored\n");
        }
        break;
    case APP_BT_CMD_RECONNECT:
        app_bt_reconnect_try_next();
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
                g_reconnect_boot_deadline = rt_tick_get() +
                                            rt_tick_from_millisecond(
                                                APP_BT_RECONNECT_BOOT_WINDOW_MS);
                app_bt_drop_residual_links();
                app_bt_reconnect_load_bonded();
                /* Kick the first attempt immediately instead of waiting
                 * for the first periodic-timer tick (saves 3s on boot). */
                app_bt_reconnect_try_next();
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
            case BT_NOTIFY_COMMON_REMOTE_NAME_RSP:
                app_bt_handle_remote_name(&event.data.rname);
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

/* S4: once a master link is streaming, freeze its link policy to
 * DISB_ALL_LM_MODES (0x0000) so the remote cannot flip the relay back
 * to slave (or into sniff) mid-stream — the double-slave scatternet is
 * exactly the "connected=2 streaming=2 but second headset silent"
 * failure. Called on every AVDTP START event because the stream-
 * (re)start path writes 0x0001 just before START (its sniff block),
 * which would otherwise re-open role switching on every resume. */
static void app_bt_freeze_master_policies(void)
{
    bt_cm_device_manager_t *manager = bt_cm_get_env();
    uint8_t index;

    if (manager == RT_NULL)
    {
        return;
    }
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        app_bt_slot_t *slot = &g_slots[index];
        bt_cm_dev_acl_info_t *connection;
        BTS2S_BD_ADDR address;

        if (!slot->used || !slot->acl_connected)
        {
            continue;
        }
        if (bt_addr_convert_to_bts((bd_addr_t *)&slot->mac, &address) != 0U)
        {
            continue;
        }
        connection = bt_cm_get_conn_by_addr(manager, &address);
        if ((connection == RT_NULL) ||
                (connection->info.role != BT_LINK_MASTER))
        {
            /* Never freeze a slave link: a pending/future role seize
             * would be rejected ("role change not allowed") by the very
             * policy meant to protect the stream. */
            continue;
        }
        if (bt_interface_get_a2dp_stream_state(&slot->mac) !=
                APP_A2DP_STREAM_PLAYING)
        {
            continue;
        }
        bt_interface_wr_link_policy_setting(slot->mac.addr, 0x0000);
        if (!slot->policy_frozen)
        {
            slot->policy_frozen = true;
            rt_kprintf("[intercom] headset %u link policy frozen (master, streaming)\n",
                       index + 1U);
        }
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
        bool drop_from_unconnected = false;

        if ((event_id == BT_NOTIFY_A2DP_PROFILE_DISCONNECTED) &&
                (data != RT_NULL) &&
                (data_len >= sizeof(bt_notify_profile_state_info_t)))
        {
            const bt_notify_profile_state_info_t *ps =
                (const bt_notify_profile_state_info_t *)data;
            int idx = app_bt_find_slot_by_mac(&ps->mac);

            /* A failed connect attempt also emits DISCONNECTED. It never
             * fed the share encoder, and forwarding it would invalidate
             * the encoder and glitch the healthy headset's music. Only
             * forward drops of links that were actually up. */
            drop_from_unconnected = (idx >= 0) &&
                                    !g_slots[idx].a2dp_connected;
        }
        if (!drop_from_unconnected)
        {
            app_a2dp_share_on_a2dp_event(event_id);
        }
        if ((event_id == BT_NOTIFY_A2DP_START_CFM) ||
                (event_id == BT_NOTIFY_A2DP_START_IND))
        {
            /* Stream is (re)starting — re-assert the master link-policy
             * freeze after bt_av_start_stream's 0x0001 sniff block. */
            app_bt_freeze_master_policies();
        }
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
        case BT_NOTIFY_COMMON_REMOTE_NAME_RSP:
            if ((data == RT_NULL) ||
                    (data_len < sizeof(event.data.rname)))
            {
                enqueue = false;
            }
            else
            {
                memcpy(&event.data.rname, data,
                       sizeof(event.data.rname));
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
        case BT_NOTIFY_AVRCP_PLAY_STATUS:
            /* Temporarily disabled entirely to isolate the talk-mode
             * SCO-drop bug. Do NOT send any AVRCP command to the headset. */
            enqueue = false;
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
    uint8_t slot_id = *(uint8_t *)parameter;

    if ((slot_id < APP_HEADSET_COUNT) &&
            (app_bt_post_timer_event(APP_BT_CMD_CONNECT_TIMEOUT,
                                     (int8_t)slot_id,
                                     g_connect_timer_gen[slot_id]) != RT_EOK))
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

/* Runs in the BTS task: re-posts into the manager queue so a media
 * recovery chain survives the share's post-failure -EBUSY window. */
static void app_bt_media_recover_bts_cb(uint16_t message, void *parameter)
{
    (void)parameter;
    if (app_bt_post_simple_event(APP_BT_CMD_MEDIA_RECOVER, message) != RT_EOK)
    {
        rt_kprintf("[intercom] failed to queue media recover\n");
    }
}

/* ---- auto-reconnect helpers ---- */

static void app_bt_mark_seen(const bt_notify_device_mac_t *mac)
{
    uint8_t i;

    for (i = 0; i < g_reconnect_seen_count; i++)
    {
        if (app_bt_mac_equal(&g_reconnect_seen[i], mac))
        {
            return;
        }
    }
    if (g_reconnect_seen_count < APP_HEADSET_COUNT)
    {
        g_reconnect_seen[g_reconnect_seen_count] = *mac;
        g_reconnect_seen_count++;
    }
}

/* ---- wedged media-path recovery ---------------------------------
 * The A2DP share watchdog (app_a2dp_share.c) calls this when a link
 * reports streaming but its media stops being delivered: the AVDTP
 * media path of that one stream is wedged and never recovers on its
 * own -- teardown + reconnect is the proven heal. Bounce = drop the ACL
 * with release_pending left clear so the existing unexpected-drop path
 * re-queues the headset and auto-reconnect brings it back as our own
 * outgoing connect (the path proven to deliver audio). Rate limited
 * per headset so a link that keeps wedging cannot loop the radio. */
bool app_bt_bounce_media_link(const unsigned char *mac)
{
    bt_notify_device_mac_t addr;
    int slot_index;
    app_mode_t mode;
    rt_tick_t now = rt_tick_get();

    memset(&addr, 0, sizeof(addr));
    memcpy(addr.addr, mac, sizeof(addr.addr));
    slot_index = app_bt_find_slot_by_mac(&addr);
    if ((slot_index < 0) || (slot_index >= APP_HEADSET_COUNT) ||
            !g_slots[slot_index].used ||
            !g_slots[slot_index].acl_connected ||
            g_slots[slot_index].release_pending)
    {
        return false;
    }
    mode = app_state_get_mode();
    if (app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR))
    {
        return false; /* never disturb an active call */
    }
    if (g_wedge_bounce_count[slot_index] >= APP_BT_WEDGE_BOUNCE_MAX)
    {
        return false;
    }
    if ((g_wedge_bounce_tick[slot_index] != 0U) &&
            ((now - g_wedge_bounce_tick[slot_index]) <
             rt_tick_from_millisecond(APP_BT_WEDGE_BOUNCE_COOLDOWN_MS)))
    {
        return false;
    }
    g_wedge_bounce_tick[slot_index] = now;
    g_wedge_bounce_count[slot_index]++;
    /* Let auto-reconnect page this headset straight back even though
     * the other headset keeps media active -- the silence gate would
     * otherwise hold the queue until playback stops. */
    g_reconnect_media_bypass_until =
        now + rt_tick_from_millisecond(APP_BT_WEDGE_BOUNCE_COOLDOWN_MS);
    rt_kprintf("[intercom] headset %u media wedged; bouncing link (%u/%u)\n",
               slot_index + 1U, g_wedge_bounce_count[slot_index],
               (unsigned int)APP_BT_WEDGE_BOUNCE_MAX);
    return bt_interface_disconnect_req(addr.addr) == 0;
}

static bool app_bt_mac_in_queue(const bt_notify_device_mac_t *mac)
{
    uint8_t i;

    for (i = 0; i < g_reconnect_len; i++)
    {
        if (app_bt_mac_equal(&g_reconnect_queue[i], mac))
        {
            return true;
        }
    }
    return false;
}

static bool app_bt_is_bonded_earphone(const bt_notify_device_mac_t *mac)
{
    bt_cm_bonded_dev_t *bonded = bt_cm_get_bonded_dev();
    BTS2S_BD_ADDR want;
    uint8_t i;

    /* Do NOT use bt_cm_get_bonded_dev_by_addr(): it was observed to
     * return NULL for bonds that are visibly present when iterated
     * (reject logs printed bonded=0 right next to bond[1] boot prints),
     * likely a bd_eq/stack-quirk inside the prebuilt code. Iterate the
     * table ourselves and compare the three address fields directly. */
    if ((mac == RT_NULL) || (bonded == RT_NULL))
    {
        return false;
    }
    memset(&want, 0, sizeof(want));
    if (bt_addr_convert_to_bts((bd_addr_t *)mac->addr, &want) != 0U)
    {
        return false;
    }
    for (i = 0; i < BT_CM_MAX_BOND; i++)
    {
        const BTS2S_BD_ADDR *cur = &bonded->info[i].bd_addr;

        if (bonded->info[i].is_use &&
                (cur->lap == want.lap) && (cur->uap == want.uap) &&
                (cur->nap == want.nap))
        {
            return true;
        }
    }
    return false;
}

static void app_bt_reconnect_enqueue(const bt_notify_device_mac_t *mac)
{
    int slot;

    if (app_bt_mac_in_queue(mac))
    {
        return;
    }
    slot = app_bt_find_slot_by_mac(mac);
    if ((slot >= 0) && (g_slots[slot].connection_stage == APP_BT_CONN_READY) &&
            g_slots[slot].a2dp_connected && g_slots[slot].hfp_connected)
    {
        /* Fully up (music + HFP) — nothing to retry. A half-up link
         * (e.g. HFP early but A2DP missing) MUST still be queued: the
         * silent skip used to leave "queued for retry" as a lie, the
         * queue stayed empty, and the headset's own re-page was then
         * rejected as unassigned. */
        return;
    }
    if (g_reconnect_len >= APP_BT_RECONNECT_QUEUE_SIZE)
    {
        rt_kprintf("[intercom] reconnect queue full — not queueing ");
        app_bt_print_mac(mac);
        rt_kprintf("\n");
        return;
    }
    g_reconnect_queue[g_reconnect_len] = *mac;
    g_reconnect_retry[g_reconnect_len] = 0U;
    g_reconnect_next_try[g_reconnect_len] =
        rt_tick_get() + rt_tick_from_millisecond(APP_BT_RECONNECT_DROP_WAIT_MS);
    g_reconnect_len++;
    /* Keep page scan on while a headset is queued so a headset that
     * powers back on can page us and be accepted immediately. */
    bt_interface_set_scan_mode(0U, 1U);
    if (!g_reconnect_active)
    {
        g_reconnect_active = true;
        rt_timer_start(g_reconnect_timer);
    }
}

static void app_bt_reconnect_remove_at(uint8_t idx)
{
    uint8_t i;

    if (idx >= g_reconnect_len)
    {
        return;
    }
    for (i = idx; i + 1U < g_reconnect_len; i++)
    {
        g_reconnect_queue[i] = g_reconnect_queue[i + 1U];
        g_reconnect_retry[i] = g_reconnect_retry[i + 1U];
        g_reconnect_next_try[i] = g_reconnect_next_try[i + 1U];
    }
    g_reconnect_len--;
    if (g_reconnect_len == 0U)
    {
        g_reconnect_active = false;
        rt_timer_stop(g_reconnect_timer);
        /* Nobody left to wait for — restore the normal non-connectable
         * mode (page scan off) used outside the reconnect window. */
        bt_interface_set_scan_mode(0U, 0U);
    }
}

static void app_bt_reconnect_remove_connected(void)
{
    uint8_t i = 0;

    while (i < g_reconnect_len)
    {
        int slot = app_bt_find_slot_by_mac(&g_reconnect_queue[i]);

        if ((slot >= 0) && g_slots[slot].hfp_connected)
        {
            app_bt_reconnect_remove_at(i);
        }
        else
        {
            i++;
        }
    }
}

static void app_bt_reconnect_remove_mac(const bt_notify_device_mac_t *mac)
{
    uint8_t i = 0;

    while (i < g_reconnect_len)
    {
        if (app_bt_mac_equal(&g_reconnect_queue[i], mac))
        {
            app_bt_reconnect_remove_at(i);
        }
        else
        {
            i++;
        }
    }
}

static void app_bt_reconnect_stop(void)
{
    g_reconnect_len = 0;
    g_reconnect_active = false;
    rt_timer_stop(g_reconnect_timer);
}

static bool app_bt_one_headset_up(void)
{
    uint8_t index;

    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        if (g_slots[index].used && app_bt_slot_has_link(&g_slots[index]))
        {
            return true;
        }
    }
    return false;
}

static void app_bt_reconnect_try_next(void)
{
    app_mode_t mode = app_state_get_mode();
    const bt_notify_device_mac_t *mac;
    app_bt_slot_t *slot;
    int slot_index;

    app_bt_reconnect_remove_connected();
    if (g_reconnect_len == 0U)
    {
        /* Queue drained: remove_connected already stopped the timer. */
        g_reconnect_active = false;
        return;
    }
    /* begin_a2dp_connect disables page scan on every attempt — re-assert
     * it while headsets are still queued (set_scan_mode de-dupes). */
    bt_interface_set_scan_mode(0U, 1U);

    if (app_bt_is_talk_mode(mode) || (mode == APP_MODE_ERROR) ||
            (mode == APP_MODE_SCANNING))
    {
        return; /* periodic timer keeps polling until the state clears */
    }
    if (app_bt_any_connect_pending())
    {
        return; /* a connection attempt is already in progress */
    }

    /* One headset is already up: never page the rest of the queue. A
     * powered-off headset cannot answer anyway, the failed page steals
     * radio time from the live link, and the stack logs that attempt as
     * "link connect failed" noise. The remaining headsets page US when
     * they are ready (powered on / back in range) and come up clean on
     * the incoming path. The wedge-bounce path opens a short bypass
     * window so its own headset is paged straight back despite this
     * gate. */
    if (app_bt_one_headset_up() &&
            (rt_tick_get() >= g_reconnect_media_bypass_until))
    {
        return;
    }

    mac = &g_reconnect_queue[0];
    slot_index = app_bt_find_slot_by_mac(mac);
    if (slot_index >= 0)
    {
        slot = &g_slots[slot_index];
        if (slot->used && ((slot->connection_stage != APP_BT_CONN_IDLE) ||
                           app_bt_slot_has_link(slot) ||
                           slot->release_pending))
        {
            /* Attempt/teardown already in flight for this device — wait
             * without burning a retry counter on a rejected command. */
            return;
        }
    }

    /* Back-off gate: fast phase runs every timer tick; slow phase arms
     * this deadline so weak/off headsets are retried every 15s instead
     * of every 3s (paging the radio disturbs the healthy headset). */
    if (rt_tick_get() < g_reconnect_next_try[0])
    {
        return;
    }

    if (g_reconnect_retry[0] >= APP_BT_RECONNECT_MAX_TOTAL_RETRY)
    {
        rt_kprintf("[intercom] reconnect giving up on headset\n");
        app_bt_reconnect_remove_at(0);
        return;
    }
    g_reconnect_retry[0]++;
    if (g_reconnect_retry[0] > APP_BT_RECONNECT_FAST_RETRY)
    {
        /* Exponential back-off: 15s → 30s → 60s → 120s (cap). A dead
         * headset must not keep paging every 15s — each page runs for
         * seconds and the radio contention glitches the healthy
         * headset's music. */
        uint8_t extra = g_reconnect_retry[0] - APP_BT_RECONNECT_FAST_RETRY -
                        1U;
        rt_tick_t gap_ms;

        if (extra > 3U)
        {
            extra = 3U;
        }
        gap_ms = APP_BT_RECONNECT_SLOW_INTERVAL_MS << extra;
        g_reconnect_next_try[0] = rt_tick_get() +
                                  rt_tick_from_millisecond(gap_ms);
        rt_kprintf("[intercom] auto-reconnect slow retry %u/%u (next in %ums)\n",
                   g_reconnect_retry[0], APP_BT_RECONNECT_MAX_TOTAL_RETRY,
                   (unsigned int)gap_ms);
        /* Rotate AFTER posting the command below (rotation invalidates
         * the &queue[0] captured in `mac`); see after-post block. */
        app_bt_post_command(APP_BT_CMD_CONNECT, mac, RT_NULL);
        if (g_reconnect_len > 1U)
        {
            /* Round-robin: an unreachable headset would otherwise sit at
             * the head and starve every other queued headset for the
             * whole back-off window. Rotate it to the tail. */
            bt_notify_device_mac_t head_mac = g_reconnect_queue[0];
            uint8_t head_retry = g_reconnect_retry[0];
            rt_tick_t head_next = g_reconnect_next_try[0];
            uint8_t n = (uint8_t)(g_reconnect_len - 1U);

            memmove(&g_reconnect_queue[0], &g_reconnect_queue[1],
                    n * sizeof(g_reconnect_queue[0]));
            memmove(&g_reconnect_retry[0], &g_reconnect_retry[1],
                    n * sizeof(g_reconnect_retry[0]));
            memmove(&g_reconnect_next_try[0], &g_reconnect_next_try[1],
                    n * sizeof(g_reconnect_next_try[0]));
            g_reconnect_queue[n] = head_mac;
            g_reconnect_retry[n] = head_retry;
            g_reconnect_next_try[n] = head_next;
        }
        return;
    }
    rt_kprintf("[intercom] auto-reconnect attempt %u/%u\n",
               g_reconnect_retry[0], APP_BT_RECONNECT_FAST_RETRY);
    app_bt_post_command(APP_BT_CMD_CONNECT, mac, RT_NULL);
}

/* A warm reset keeps the BT controller's link table: the pre-reset ACLs
 * linger there until its link supervision expires — until then Create
 * Connection fails with "connection already exists" (res 11) and the
 * headsets still believe they are connected (they only notice the loss
 * when THEIR supervision runs out, which feels like a long disconnect).
 * Dropping every residual link at boot unblocks our pages immediately
 * and gives the headsets a real disconnect event so they re-page within
 * seconds instead of waiting out their own supervision window. */
static void app_bt_drop_residual_links(void)
{
    bt_cm_bonded_dev_t *bonded = bt_cm_get_bonded_dev();
    uint8_t i;

    for (i = 0U; i < BT_CM_MAX_BOND; i++)
    {
        if (bonded->info[i].is_use && !bd_is_empty(&bonded->info[i].bd_addr))
        {
            bt_notify_device_mac_t mac;

            memset(&mac, 0, sizeof(mac));
            bt_addr_convert_to_general(&bonded->info[i].bd_addr,
                                       (bd_addr_t *)&mac);
            if (bt_interface_disconnect_req(mac.addr) == 0)
            {
                rt_kprintf("[intercom] dropping residual link: ");
                app_bt_print_mac(&mac);
                rt_kprintf("\n");
            }
        }
    }
}

static void app_bt_reconnect_load_bonded(void)
{
    bt_cm_bonded_dev_t *bonded = bt_cm_get_bonded_dev();
    uint8_t i;

    for (i = 0; i < BT_CM_MAX_BOND; i++)
    {
        bt_cm_dev_info_t *dev = &bonded->info[i];

        if (dev->is_use && !bd_is_empty(&dev->bd_addr))
        {
            bt_notify_device_mac_t mac;

            memset(&mac, 0, sizeof(mac));
            bt_addr_convert_to_general(&dev->bd_addr, (bd_addr_t *)&mac);
            rt_kprintf("[intercom] bond[%u]: ", i);
            app_bt_print_mac(&mac);
            rt_kprintf(" link_type=%u%s\n", dev->link_type,
                       (i == bonded->last_bond_idx) ? " (last)" : "");
            app_bt_reconnect_enqueue(&mac);
            rt_kprintf("[intercom] queued bonded headset for reconnect\n");
        }
    }
    /* Passive-first after boot: the headsets page us on their own within
     * seconds; only fall back to outgoing paging after the quiet window. */
    for (i = 0U; i < g_reconnect_len; i++)
    {
        g_reconnect_next_try[i] =
            rt_tick_get() +
            rt_tick_from_millisecond(APP_BT_RECONNECT_PASSIVE_WAIT_MS);
    }
}

static void app_bt_reconnect_timer_cb(void *parameter)
{
    (void)parameter;
    app_bt_post_simple_event(APP_BT_CMD_RECONNECT, 0U);
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

/* Incoming ACL accept role (overrides bt_cm's __WEAK default of
 * BT_LINK_SLAVE). The relay is the A2DP source for TWO headsets: when a
 * headset pages us after a reset the accept must request "become master"
 * (HCI Accept_Connection_Request role = 0x00) so both headsets end up as
 * slaves in OUR single piconet. Accepted as slave they keep their own
 * piconets and the relay time-shares RX slots across two masters — the
 * second headset then gets starved and goes silent ("connected=2
 * streaming=2" but no audio over the air). Outgoing connects are already
 * master (pager role), so this makes both directions converge on the same
 * star topology. */
uint8_t bt_get_acpt_link_role(void)
{
    return BT_LINK_MASTER;
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
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        g_connect_timer_armed[index] = false;
        g_connect_timer_gen[index] = 0U;
    }
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
    for (index = 0U; index < APP_HEADSET_COUNT; index++)
    {
        const char *name = (index == 0U) ? "btconn0" : "btconn1";

        g_connect_timer_slot_ids[index] = index;
        g_connect_timers[index] = rt_timer_create(
                                      name, app_bt_connect_timeout,
                                      &g_connect_timer_slot_ids[index],
                                      rt_tick_from_millisecond(
                                          APP_BT_CONNECT_TIMEOUT_MS),
                                      RT_TIMER_FLAG_ONE_SHOT |
                                      RT_TIMER_FLAG_SOFT_TIMER);
    }
    g_avrcp_volume_timer = rt_timer_create(
                               "btavvol", app_bt_avrcp_volume_timeout, RT_NULL,
                               rt_tick_from_millisecond(
                                   APP_BT_AVRCP_VOLUME_RETRY_MS),
                               RT_TIMER_FLAG_ONE_SHOT |
                               RT_TIMER_FLAG_SOFT_TIMER);
    g_reconnect_timer = rt_timer_create(
                            "btrcn", app_bt_reconnect_timer_cb, RT_NULL,
                            rt_tick_from_millisecond(
                                APP_BT_RECONNECT_INTERVAL_MS),
                            RT_TIMER_FLAG_PERIODIC |
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
    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        const char *name = (index == 0U) ? "bhfp0" : "bhfp1";

        g_hfp_timer_slot_ids[index] = index;
        g_hfp_timers[index] = rt_timer_create(
                                  name, app_bt_hfp_timeout,
                                  &g_hfp_timer_slot_ids[index],
                                  rt_tick_from_millisecond(
                                      APP_BT_HFP_DEFER_GRACE_MS),
                                  RT_TIMER_FLAG_ONE_SHOT |
                                  RT_TIMER_FLAG_SOFT_TIMER);
    }
    for (index = 0; index < APP_HEADSET_COUNT; index++)
    {
        const char *name = (index == 0U) ? "btrole0" : "btrole1";

        g_role_timer_slot_ids[index] = index;
        g_role_timers[index] = rt_timer_create(
                                   name, app_bt_role_timeout,
                                   &g_role_timer_slot_ids[index],
                                   rt_tick_from_millisecond(
                                       APP_BT_ROLE_DISC_MS),
                                   RT_TIMER_FLAG_ONE_SHOT |
                                   RT_TIMER_FLAG_SOFT_TIMER);
    }
    if ((g_bt_event_queue == RT_NULL) || (g_bt_ready_mailbox == RT_NULL) ||
            (g_bt_lock == RT_NULL) ||
            (g_talk_timer == RT_NULL) ||
            (g_connect_timers[0] == RT_NULL) ||
            (g_connect_timers[1] == RT_NULL) ||
            (g_avrcp_volume_timer == RT_NULL) ||
            (g_reconnect_timer == RT_NULL) ||
            (g_disconnect_timers[0] == RT_NULL) ||
            (g_disconnect_timers[1] == RT_NULL) ||
            (g_hfp_timers[0] == RT_NULL) ||
            (g_hfp_timers[1] == RT_NULL) ||
            (g_role_timers[0] == RT_NULL) ||
            (g_role_timers[1] == RT_NULL))
    {
        return -RT_ENOMEM;
    }
    app_bt_reconnect_stop();

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

bool app_bt_is_link_registered(const unsigned char *mac)
{
    bt_notify_device_mac_t key;
    int slot;

    if (mac == RT_NULL)
    {
        return false;
    }
    memcpy(key.addr, mac, sizeof(key.addr));
    slot = app_bt_find_slot_by_mac(&key);
    return (slot >= 0) && g_slots[slot].used &&
           g_slots[slot].a2dp_connected;
}

int app_bt_scan(void)
{
    app_mode_t mode = app_state_get_mode();

    /* CONNECTING is allowed: after a reset the auto-reconnect queue keeps
     * a slot "connecting" while it pages absent headsets, and that used to
     * reject every `intercom scan` with "device is not ready or is busy".
     * The command handler pauses those attempts for the inquiry. */
    if (!g_bt_initialized || !g_bt_ready ||
            ((mode != APP_MODE_IDLE) && (mode != APP_MODE_MEDIA) &&
             (mode != APP_MODE_SCANNING) && (mode != APP_MODE_CONNECTING)))
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

    return app_bt_post_command(APP_BT_CMD_CONNECT_MANUAL, &mac, name);
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

int app_bt_unbond(const char *target)
{
    bt_notify_device_mac_t mac = {0};

    if (!g_bt_initialized || (target == RT_NULL))
    {
        return -RT_EINVAL;
    }
    if (!app_bt_parse_mac(target, &mac))
    {
        return -RT_EINVAL;
    }

    rt_mutex_take(g_bt_lock, RT_WAITING_FOREVER);
    app_bt_reconnect_remove_mac(&mac);
    rt_mutex_release(g_bt_lock);
    bt_cm_delete_bonded_devs_and_linkkey((uint8_t *)mac.addr);
    rt_kprintf("[intercom] bond removed: ");
    app_bt_print_mac(&mac);
    rt_kprintf("\n");
    return RT_EOK;
}

int app_bt_talk(bool start)
{
    app_mode_t mode = app_state_get_mode();

    if (!g_bt_initialized || !g_bt_ready)
    {
        return -RT_EBUSY;
    }
    /* Start is accepted (and queued if the link is not READY yet) as long
     * as we are in a media-ish state; only a hard reject in scan/error.
     * Stop covers both an active talk and a still-queued request. */
    if (start && (g_scanning || (mode == APP_MODE_ERROR) ||
                  ((mode != APP_MODE_MEDIA) && (mode != APP_MODE_CONNECTING))))
    {
        return -RT_EBUSY;
    }
    if (!start && !app_bt_is_talk_mode(mode) && !g_talk_pending)
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
    {
        uint8_t i;

        for (i = 0U; i < share_status.media_con_count; i++)
        {
            bt_notify_device_mac_t mac;

            if ((share_status.media_con_active & (uint8_t)(1U << i)) == 0U)
            {
                continue;
            }
            memset(&mac, 0, sizeof(mac));
            memcpy(mac.addr, share_status.media_con_mac[i], sizeof(mac.addr));
            rt_kprintf("  media-tx[%u]: ", i);
            app_bt_print_mac(&mac);
            rt_kprintf(" req=%u nomem=%u qos=%u buff=%u\n",
                       (unsigned int)share_status.media_req[i],
                       (unsigned int)share_status.media_nomem[i],
                       (unsigned int)share_status.media_qos[i],
                       share_status.media_buff[i]);
        }
    }
    rt_kprintf("  heap: used=%u max-used=%u total=%u free-now=%u\n",
               used_memory, max_used_memory, total_memory,
               (used_memory <= total_memory) ? total_memory - used_memory : 0U);
}
