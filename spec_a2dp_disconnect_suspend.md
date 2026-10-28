# Spec：主动断连先 suspend AVDTP 再断 ACL

## 1. 背景与目标

双蓝牙耳机对讲工程 `Dual_Bluetooth_Headset_Walkie-Talkie` 主动断连时残留「电流音」。

**已确认根因**（抓包 + 代码双重验证）：

- 抓包（Frontline）每次主动断连时序完全一致：
  `ACL closed before A2DP L2CAP channel` → `Signal channel closed before streaming channel` → `A2DP disconnected`。
- 即：**先断 ACL，媒体流被硬切**，导致 gap（208/265ms）→ AVDTP packet lost → underrun → 电流音。
- 代码证实主动断连链路没有先 suspend AVDTP：
  `app_bt_handle_disconnect_command` → `app_bt_issue_disconnect` → `bt_interface_disconnect_req` → `bt_disconnect_req` → `gap_disconnect_req`（GAP 层直接断 ACL）。

**用户澄清**（必须遵守）：

1. 断连都是**主动发起**，时序固件可控 → 可以修。
2. SBC CRC error **偶发**，是次要问题，不在本次修复范围。

**目标**：主动断连时，先对目标耳机发出 AVDTP SUSPEND，**等 SUSPEND_CFM 回复确认**后再断 ACL。

## 2. 方案（已与用户确认）

- 范围：**仅断连目标那一副耳机**（另一副健康耳机的共享流不动；与之前 `invalidate = (connected_count == 0)` 改动同逻辑）。
- 等待方式：**等 suspend 回复确认**（SUSPEND_CFM 到达、`suspend_pending == FALSE`）后再断 ACL；对端拒绝 suspend 也视为回复已到（照常断）。
- 兜底：复用现有 disconnect watchdog（`APP_BT_DISCONNECT_TIMEOUT_MS = 3000`），suspend 3s 内未回复则强断。

## 3. 精确改动点

所有改动都在 [app_bt.c](file:///f:/SiFli/SDK/SiFli-SDK/main/example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_bt.c)。

### 3.1 `app_bt_slot_t` 结构新增字段

在现有 `disconnect_watchdog_active` / `disconnect_blocks_media` 附近（约 L240-241）新增：

```c
bool disconnect_suspend_pending;  /* 已发 AVDTP suspend，等 SUSPEND_CFM 后断 ACL */
```

### 3.2 新增辅助函数（放在 `app_bt_issue_disconnect` 附近，约 L1553 前）

```c
/* 判断 slot 对应 AVDTP 连接是否处于 streaming，并返回其 av 连接索引
 * con_idx（<MAX_CONNS 才有效）。 */
static int app_bt_av_conn_idx(const app_bt_slot_t *slot, uint8_t *con_idx_out)
{
    bts2s_av_inst_data *inst = bt_av_get_inst_data();
    BTS2S_BD_ADDR addr;
    U8 con_idx;
    if (inst == NULL || slot == NULL || con_idx_out == NULL) return -1;
    if (bt_addr_convert_to_bts((bd_addr_t *)&slot->mac, &addr) != 0U) return -1;
    con_idx = bt_av_get_idx_from_addr(inst, &addr);
    if (con_idx > MAX_CONNS) return -1;
    *con_idx_out = con_idx;
    return 0;
}
```

```c
/* 目标耳机正在 streaming 时，先 suspend 并挂起断连，返回 true。 */
static bool app_bt_begin_suspend_then_disconnect(int slot_index)
{
    app_bt_slot_t *slot = &g_slots[slot_index];
    uint8_t con_idx;

    if (app_bt_av_conn_idx(slot, &con_idx) != 0) return false;
    bts2s_av_inst_data *inst = bt_av_get_inst_data();
    if (inst->con[con_idx].st != avconned_streaming) return false;

    slot->disconnect_suspend_pending = true;
    bt_av_suspend_stream(con_idx);
    /* 兜底：suspend 3s 无回复时由 DISCONNECT_TIMEOUT 强断 */
    app_bt_disconnect_timer_start(slot_index);
    return true;
}
```

### 3.3 `app_bt_handle_disconnect_command`（约 L2510-2521）插入挂起判断

原代码：

```c
app_bt_connect_timer_stop(slot_index);
g_slots[slot_index].connection_stage = APP_BT_CONN_ABORTING;
g_slots[slot_index].release_pending = true;
if (!g_slots[slot_index].disconnect_watchdog_active)
{
    g_slots[slot_index].disconnect_attempts = 0U;
    if (app_bt_issue_disconnect(slot_index) != 0) { ... }
}
```

改为：

```c
app_bt_connect_timer_stop(slot_index);
g_slots[slot_index].connection_stage = APP_BT_CONN_ABORTING;
g_slots[slot_index].release_pending = true;
if (!g_slots[slot_index].disconnect_watchdog_active)
{
    g_slots[slot_index].disconnect_attempts = 0U;
    if (app_bt_begin_suspend_then_disconnect(slot_index))
    {
        rt_kprintf("[intercom] headset %u suspend-then-disconnect pending\n",
                   slot_index + 1U);
        return;   /* 等 SUSPEND_CFM 再断 */
    }
    if (app_bt_issue_disconnect(slot_index) != 0) { ... }
}
```

### 3.4 在 `BT_NOTIFY_A2DP` 分发处处理 `BT_NOTIFY_A2DP_SUSPEND_CFM`（约 L4866-4890）

在 `if (type == BT_NOTIFY_A2DP)` 分支内、调用 `app_a2dp_share_on_a2dp_event(event_id)` 之前/之后，新增：

```c
if (event_id == BT_NOTIFY_A2DP_SUSPEND_CFM)
{
    for (uint8_t i = 0; i < APP_HEADSET_COUNT; i++)
    {
        app_bt_slot_t *slot = &g_slots[i];
        uint8_t con_idx;
        if (!slot->disconnect_suspend_pending) continue;
        if (app_bt_av_conn_idx(slot, &con_idx) != 0) continue;
        bts2s_av_inst_data *inst = bt_av_get_inst_data();
        if (inst->con[con_idx].suspend_pending) continue; /* 回复未到 */
        slot->disconnect_suspend_pending = false;
        app_bt_issue_disconnect(i);
        rt_kprintf("[intercom] headset %u suspended, disconnecting ACL\n",
                   i + 1U);
    }
}
```

### 3.5 清理挂起标记

- `app_bt_handle_acl_disconnected`（约 L2783-2794）清 slot 状态处，追加
  `g_slots[slot_index].disconnect_suspend_pending = false;`
- `APP_BT_CMD_DISCONNECT_TIMEOUT`（约 L4539-4569）执行路径已能兜底强断（`app_bt_issue_disconnect` 会 attempts++），无需额外分支；但需保证 suspend 挂起期间 watchdog 已 active 且 attempts 从 0 起步，使超时走 L4561 的 retry 分支强断。

## 4. 需要头文件/符号确认（执行时）

- `bt_av_get_inst_data`、`bt_av_suspend_stream`、`avconned_streaming`、`bts2s_av_inst_data`：声明于 `bts2_app_av.h`。执行时确认 app_bt.c 已直接/间接（经 `bts2_app_inc.h`）可见；否则需补 `#include "bts2_app_av.h"`。
- `bt_av_get_idx_from_addr`、`MAX_CONNS`：同上。

## 5. 验证步骤

1. 编译：进入 `example/Dual_Bluetooth_Headset_Walkie-Talkie/project/hcpu`，`scons --board=<board> -j8`。
2. 烧录后主动 `intercom disconnect` 断其中一副耳机，观察：
   - 串口出现 `suspend-then-disconnect pending` → 随后 `suspended, disconnecting ACL`。
   - 抓包确认新时序为 `AVDTP suspend` → `suspend cnf` → 之后才 `ACL closed`，不再出现 `ACL closed before A2DP L2CAP channel`。
   - 耳机端电流音消除/明显减小。
3. 回归：断开其中一副后，另一副仍正常 streaming（不出现 `invalidate=1`、不断流）。
4. 回归：对端拒不响应 suspend 时，3s 后 watchdog 强断，不挂死。

## 6. 批准要求

- 本方案为单变量改动（仅 `app_bt.c` 一个文件、一个新增字段 + 两个辅助函数 + 三处插入点）。
- 待用户批准后进入执行；执行期间不动 SBC/CRC 相关代码。