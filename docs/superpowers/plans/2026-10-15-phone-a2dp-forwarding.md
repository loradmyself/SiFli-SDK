# Phone A2DP Forwarding Implementation Plan

> **For agentic workers:** REQUIRED SUBSKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the two-headset HFP/SCO intercom while replacing I2S PCM input with one phone A2DP stream forwarded to both headset A2DP sinks.

**Architecture:** Add a separate phone connection context beside the two headset slots. Replace the PCM/SBC producer with a raw A2DP packet ring consumed by `bt_avsrc_sharing()`, keep the existing headset SCO relay state machine, and dynamically assign AVRCP CT to the phone and AVRCP TG to headsets.

**Tech Stack:** SiFli-SDK, RT-Thread, SCons/Kconfig, SiFli Classic Bluetooth middleware, A2DP/AVRCP/HFP profiles.

---

## File Map

- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/project/proj.conf`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/project/sf32lb57x/proj.conf`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/SConscript`.
- Replace `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_a2dp_share.h`.
- Replace `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_a2dp_share.c`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_bt.h`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_bt.c`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_cli.c`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/main.c`.
- Delete `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_i2s.c`.
- Delete `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_i2s.h`.
- Modify `example/Dual_Bluetooth_Headset_Walkie-Talkie/README.md`.

## Task 1: Configuration and Build Surface

**Files:**
- Modify `project/proj.conf`
- Modify `project/sf32lb57x/proj.conf`
- Modify `src/SConscript`
- Modify `src/main.c`
- Delete `src/app_i2s.c`, `src/app_i2s.h`

- [ ] Change `project/proj.conf` to remove I2S, record-pipe, HCI-log and console-log settings and keep Classic BT, HFP AG, voice relay, A2DP Source/Sharing and AVRCP enabled.
- [ ] Change `project/sf32lb57x/proj.conf` to add `CONFIG_CFG_AV_SNK=y`, set `CONFIG_CFG_MAX_BT_ACL_NUM=3`, `CONFIG_CFG_MAX_BT_BOND_NUM=3`, and remove I2S settings.
- [ ] Remove the I2S include and initialization from `src/main.c`; call `app_bt_init()` directly.
- [ ] Exclude `app_i2s.c` from `src/SConscript` and delete both `app_i2s.c` and `app_i2s.h`.
- [ ] Run `git diff --check`.
- [ ] Commit with `build: remove i2s input from intercom demo`.

## Task 2: Phone A2DP Packet Share Module

**Files:**
- Replace `src/app_a2dp_share.h`
- Replace `src/app_a2dp_share.c`

Required public interface:

```c
typedef enum {
    APP_A2DP_SHARE_STATE_UNINITIALIZED,
    APP_A2DP_SHARE_STATE_IDLE,
    APP_A2DP_SHARE_STATE_PREFILLING,
    APP_A2DP_SHARE_STATE_RUNNING,
    APP_A2DP_SHARE_STATE_STOPPING,
    APP_A2DP_SHARE_STATE_ERROR
} app_a2dp_share_state_t;

typedef struct {
    bool initialized;
    bool desired;
    bool phone_streaming;
    bool source_streaming;
    uint8_t source_connected_count;
    uint8_t source_streaming_count;
    app_a2dp_share_state_t state;
    uint32_t ring_bytes;
    uint32_t ring_capacity;
    uint32_t phone_packets;
    uint32_t packets_relayed;
    uint32_t ring_drops;
    uint32_t invalid_packets;
} app_a2dp_share_status_t;

int app_a2dp_share_init(app_a2dp_share_callback_t callback, void *user_data);
int app_a2dp_share_start(void);
int app_a2dp_share_stop(uint32_t sequence);
void app_a2dp_share_on_a2dp_event(uint16_t event_id);
void app_a2dp_share_on_media_data(const bt_notify_a2dp_data_t *media);
void app_a2dp_share_set_phone_connected(bool connected);
void app_a2dp_share_reconcile_streams(void);
bool app_a2dp_share_is_stopped(void);
void app_a2dp_share_get_status(app_a2dp_share_status_t *status);
const char *app_a2dp_share_state_name(app_a2dp_share_state_t state);
```

- [ ] Implement one `rt_ringbuffer32` using `SINK_DATA_LIST_MAX_THRESHOLD * AV_MTU_SIZE` bytes.
- [ ] Store each phone packet as `uint16_t data_length` followed by `AV_FIXED_MEDIA_PKT_HDR_SIZE` media bytes.
- [ ] Reject null data, zero length and `data_length > AV_MTU_SIZE`; count invalid packets.
- [ ] On ring overflow, drop the incoming packet and increment `ring_drops`.
- [ ] Start `bt_avsrc_sharing()` only after the ring reaches `AV_MTU_SIZE * SINK_DATA_LIST_START_THRESHOLD`.
- [ ] Keep the existing per-source-link start/stop reconciliation and wedge bounce hook where the old module exposed them.
- [ ] Clear the ring on stop, phone suspend/disconnect and A2DP source disconnect.
- [ ] Build the project after the module replacement; expected failure is only unresolved `app_bt` calls until Task 3.
- [ ] Commit with `feat: relay phone a2dp packets to shared sinks`.

## Task 3: Phone Context and A2DP Event Routing

**Files:**
- Modify `src/app_bt.h`
- Modify `src/app_bt.c`

- [ ] Add `app_bt_phone_disconnect()` and `app_bt_has_phone_media()` declarations to `app_bt.h`.
- [ ] Add one `app_bt_phone_t` context with MAC, ACL, A2DP, AVRCP and stream state. Do not use `g_slots[APP_HEADSET_COUNT]` for the phone.
- [ ] In ACL handling, allow one non-headset ACL to become the phone candidate when no phone is registered.
- [ ] Route `AV_AUDIO_SNK` profile events to the phone context and `AV_AUDIO_SRC` events to the existing headset slots.
- [ ] Handle `BT_NOTIFY_A2DP_DATA_IND` synchronously in `app_bt_event_callback`; call `app_a2dp_share_on_media_data()` and then `bfree(media->data)`.
- [ ] Update `app_bt_update_media_for_links()` so media is desired only when the phone A2DP stream and at least one headset source stream are available and no talk mode is active.
- [ ] Ensure `talk start` stops the phone share and `talk stop` re-runs `app_a2dp_share_reconcile_streams()`.
- [ ] Keep all existing headset HFP/SCO functions and timer generation logic unchanged.
- [ ] Compile after this task; expected failure is only CLI/status references until Task 4.
- [ ] Commit with `feat: track phone a2dp link beside headset slots`.

## Task 4: AVRCP Roles, Volume and Media Controls

**Files:**
- Modify `src/app_bt.c`

- [ ] Replace the unconditional `bt_interface_set_avrcp_role_ext(..., AVRCP_TG)` call with role selection from the matching A2DP role.
- [ ] For the phone, use `AVRCP_CT`; for each headset, use `AVRCP_TG`.
- [ ] On phone absolute-volume events, update local volume and forward the raw AVRCP volume to both connected headset links.
- [ ] On headset play/pause/previous/next events, forward the command to the phone MAC only.
- [ ] Do not send AVRCP media commands during `APP_MODE_TALK_STARTING`, `APP_MODE_TALKING` or `APP_MODE_TALK_STOPPING`.
- [ ] Preserve the existing per-headset music/talk volume commands.
- [ ] Compile and run `git diff --check`.
- [ ] Commit with `feat: bridge phone and headset avrcp controls`.

## Task 5: CLI and Documentation

**Files:**
- Modify `src/app_cli.c`
- Modify `README.md`

- [ ] Add `intercom phone disconnect` to help and dispatch to `app_bt_phone_disconnect()`.
- [ ] Keep `intercom scan/connect/disconnect/unbond/talk/volume` behavior unchanged.
- [ ] Extend `app_bt_print_status()` output with phone ACL/A2DP/AVRCP/stream state and phone-share ring statistics.
- [ ] Replace the README I2S data path with the phone A2DP data path.
- [ ] Document three-link topology: one phone plus two headsets.
- [ ] Document that talk remains two-headset HFP/SCO intercom and phone HFP call relay is not included.
- [ ] Run `git diff --check`.
- [ ] Commit with `docs: document phone a2dp forwarding mode`.

## Task 6: Build and Size Verification

**Files:**
- Build output under `example/Dual_Bluetooth_Headset_Walkie-Talkie/project/build_spi-hdk_lb57bun3n6_hcpu`
- Modify code only if compilation or size constraints require it.

Commands:

```powershell
cd example\Dual_Bluetooth_Headset_Walkie-Talkie\project
scons --board=spi-hdk_lb57bun3n6_hcpu -j8
```

- [ ] Build must complete without I2S references or unresolved symbols.
- [ ] Capture `main.bin`, `main.map` and `text/data/bss`.
- [ ] Use `arm-none-eabi-size.exe` and `arm-none-eabi-nm.exe -S --size-sort` to record `app_a2dp_share.o`, `app_bt.o`, `mempool`, phone-ring and removed I2S symbols.
- [ ] Verify `main.bin` is below the configured main image limit.
- [ ] Confirm `CONFIG_CFG_AV_SNK=y`, `CONFIG_CFG_MAX_BT_ACL_NUM=3` and no `CONFIG_BSP_USING_I2S=y` in the generated `.config`.
- [ ] Run a source scan for `app_i2s_`, `app_i2s.h` and `AUDIO_TX_USING_I2S`; expected result is no application references.
- [ ] Commit any final size adjustment with `build: finalize phone a2dp forwarding size`.

## Self-Review Checklist

- [ ] Phone A2DP packets are copied into the share ring and SDK buffers are freed exactly once.
- [ ] Two headset A2DP streams are independently started/stopped and one disconnect does not stop the other.
- [ ] Existing HFP/SCO talk flow remains present and stops/resumes music around talk.
- [ ] AVRCP roles are dynamic: phone CT, headset TG.
- [ ] No I2S initialization or PCM read path remains.
- [ ] The final build size and memory symbols are recorded in the final report.
