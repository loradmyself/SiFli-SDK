# 蓝牙底层（middleware/bluetooth）驱动修改汇总与应用层移植评估

本文档汇总本工程对蓝牙底层（`middleware/bluetooth` 子模块）的全部驱动级修改，
按提交逐项说明**删了什么、加了什么、源代码内容、功能作用**，并评估把这些修改
**全部移植到应用层**（`example/Dual_Bluetooth_Headset_Walkie-Talkie/src/`）的可能性。

## 0. 范围与基线

| 项目 | 内容 |
| --- | --- |
| 仓库 | `middleware/bluetooth`（git 子模块，独立于主仓提交） |
| 基线 | `437d7da`（SDK 官方 [int][opt][bt] Normalize naming...） |
| 当前版本 | `33f31a8`（HEAD） |
| 提交数 | 4 |
| 累计规模 | 9 个文件，+398 / −16 行 |
| 应用侧配套 | `app_bt.c` / `app_a2dp_share.c`（主仓 nuonuo 分支，不在本文统计内，仅在移植评估中引用） |

提交时间线（旧 → 新）：

| 提交 | 日期 | 说明 | 文件 | 规模 |
| --- | --- | --- | --- | --- |
| `34ba0cc` | 2026-09-28 | 勉强好点 | 3 | +59 / −6 |
| `f45f101` | 2026-10-08 | 复位重连 -断开重连 next：page 拒绝 | 7 | +273 / −16 |
| `6d247a5` | 2026-10-09 | readme buds 6 问题 | 3 | +31 / −6 |
| `33f31a8` | 2026-10-13 | log 显示bug | 3 | +72 / −25 |

涉及文件总览（净增删行，`git diff --numstat 437d7da..HEAD`）：

```
include/bt_connection_manager.h          |   +1 /  -0   ACL 信息结构体加 encrypted 字段
service/bt/bt_cm/bt_connection_manager.c | +112 / -15   连接管理器（ACL/pair/role/sniff）
service/bt/bt_finsh/bts2_app_av.c        |  +91 /  -1   AVDTP 应用层
service/bt/bt_finsh/bts2_app_av.h        |   +2 /  -0
service/bt/bt_finsh/bts2_app_av_src.c    |  +60 /  -0   A2DP Source 媒体转发
service/bt/bt_finsh/bts2_app_av_src.h    |   +6 /  -0
service/bt/bt_finsh/bts2_app_generic.c   |  +24 /  -0   通用 __WEAK 钩子
service/bt/bt_finsh/bts2_app_generic.h   |   +2 /  -0
service/bt/bt_finsh/bts2_app_hfp_ag.c    | +100 /  -0   HFP AG（净；含对前一提交插入行的就地改写）
```

---

## 1. 提交逐项详解

### 1.1 `34ba0cc`（2026-09-28）「勉强好点」

主题：**连接管理器健壮性 + 对端发起 AVDTP 的角色修复**。

#### 1.1.1 `include/bt_connection_manager.h` — ACL 信息加加密标志

```c
 typedef struct
 {
     uint8_t link_status_update;
     uint8_t link_mode;
     float   link_interval;
+    uint8_t encrypted; // link key verified (BTS2MU_GAP_ENCRYPTION_IND)
 } bt_cm_dev_acl_info_t;
```

**作用**：记录该 ACL 是否已完成链路密钥校验（加密）。应用层的
`app_bt_link_encrypted()` 等门控逻辑以此判断"链路已加密、可以发起
AVDTP"，避免慢配对耳机在未加密链路上发请求（请求会凭空消失）。

#### 1.1.2 `bt_connection_manager.c` — bond 表 NVDS 读取越界修复

删除（原实现）：

```c
-    uint16_t len = sizeof(g_bt_bonded_dev);
-    ret = sifli_nvds_read(SIFLI_NVDS_TYPE_BT_CM, &len, (uint8_t *)&g_bt_bonded_dev);
-    if (ret != NVDS_OK)
```

新增（`bt_cm_get_bond_info_from_flash()`）：

```c
+    static uint8_t raw[SIFLI_NVDS_KEY_LEN_BT_CM];
+    uint16_t len;
+    uint16_t copy_len;
+
+    memset(&g_bt_bonded_dev, 0, sizeof(g_bt_bonded_dev));
+    len = SIFLI_NVDS_KEY_LEN_BT_CM;
+    ret = sifli_nvds_read(SIFLI_NVDS_TYPE_BT_CM, &len, raw);
+    if (ret == NVDS_OK)
+    {
+        copy_len = (len < sizeof(g_bt_bonded_dev)) ? len : sizeof(g_bt_bonded_dev);
+        memcpy(&g_bt_bonded_dev, raw, copy_len);
+        if (g_bt_bonded_dev.last_bond_idx >= BT_CM_MAX_BOND)
+            g_bt_bonded_dev.last_bond_idx = 0;
+    }
```

**作用**：`sifli_nvds_read()` 按 flash 里**实际存储长度**整块拷贝。若记录
由 `BT_CM_MAX_BOND` 更大的固件写入，直接拷进 `g_bt_bonded_dev` 会越界写
BSS。改为固定 NVDS 尺寸暂存缓冲 + 限长拷贝 + `last_bond_idx` 钳位。纯内存
安全修复。

#### 1.1.3 `bt_connection_manager.c` — 加密指示置位

```c
     case BTS2MU_GAP_ENCRYPTION_IND:
         ...
+        conn->encrypted = 1;
          bt_cm_add_bonded_dev(&conn->info, 1);
```

**作用**：配合 1.1.1 的字段，ENCRYPTION_IND 到达时置位。

#### 1.1.4 `bt_connection_manager.c` — 拒绝对端的重复 page

`bt_cm_hci_connect_request_event_hdl()`（入向 ACL 请求）新增前置判断：

```c
+        /* A remote that pages us WHILE an established ACL to it is up ... */
+        if (conn && (conn->state == BT_CM_ACL_STATE_CONNECTED))
+        {
+            LOG_I("refusing duplicate page; ACL already up");
+            hcia_send_rej_conn_request(&ind->bd, HCI_ERR_REJ_BY_RMT_NO_RES);
+            return;
+        }
```

**作用**：TWS 耳机掉线后会疯狂 page。若已有存活 ACL，控制器会被迫做
"connection already exists"（0x0B）换链：旧 ACL 被杀、新 ACL 建立，无限
churn 还殃及其它连接。改为直接 `Reject_Connection_Request`，保干净的现
有链路。注意只拒绝**已建立**的重复 page，创建中的竞争 page 仍走正常 accept。

#### 1.1.5 `bt_connection_manager.c` — `a2dp_reset_req()` 条件化

断链处理（`bt_cm_hci_acl_disconnect_complete_event_hdl`）中：

```c
-    a2dp_reset_req();
+    /* Reset the AV request state only when the disconnected device owns
+     * an AV instance. An unconditional reset cancels the PENDING AV
+     * connect of an unrelated headset ... */
+    if (bt_av_get_idx_from_addr(bt_av_get_inst_data(), &ind->cur_bd) < MAX_CONNS)
+    {
+        a2dp_reset_req();
+    }
```

**作用**：`a2dp_reset_req()` 是**全局** AV 请求状态复位。任何 ACL 断开
（被拒的 page、耳机入仓）都会取消**另一个**耳机正在进行的 AVDTP 连接
（"a2dp connect failed 97"）。本版改为仅当断开方持有 AV 实例才复位。
（注：下一提交 `f45f101` 又把这里整个删掉，见 1.2.1f。）

#### 1.1.6 `bts2_app_av.c` — AVDTP 失败事件不再过滤

`bt_av_hdl_conn_cfm()` 删除一行过滤条件：

```c
     if (msg->res != AV_ACPT)
     {
-        if ((msg->res != AV_DUPLICATE_CONNECTING) && (msg->res != AV_CONNECT_CONFLICT))
         {
             ... bt_profile_update_connection_state(...)  /* 失败通知 */
         }
```

**作用**：原来 `9a/9b`（重复连接/连接冲突）被中间件吞掉，应用层失明，
只能盲目超时。现在**所有**失败都上报，应用可以区分"冲突（保链路重试）"
和"真失败（放弃）"（应用侧 `app_bt_handle_a2dp` 的
"A2DP conflict %u ignored; keeping the link" 分支即依赖此事件）。

#### 1.1.7 `bts2_app_av.c` — 对端发起 AVDTP 时补角色（**无声根因之一**）

`bt_av_hdl_conn_ind()`（对端打开 AVDTP 信令）新增：

```c
+#ifdef CFG_AV_SRC
+        /* Remote-opened AVDTP used to leave cfg at its init value
+         * (AV_AUDIO_NO_ROLE): the start confirmation then matched neither
+         * role, so the state never advanced to avconned_streaming and the
+         * sharing fan-out sent zero packets — "connected with name but no
+         * sound" for every remote-opened link. We are the A2DP Source
+         * regardless of who opened the signaling channel. */
+        inst->con[i].cfg = AV_AUDIO_SRC;
+#endif
```

**作用**：出向连接在 `bt_av_hdl_conn_cfm` 里 `cfg = msg->local_role`；入向
连接原来不设 `cfg`，停留在 `AV_AUDIO_NO_ROLE` → START 确认匹配不到角色
→ 状态永不进入 `avconned_streaming` → 共享扇出（按 streaming 过滤）零包。
现象就是"有设备名、显示已连接，但耳机完全无声"。无论谁发起信令，我们
都是 A2DP **Source**，直接置位。

---

### 1.2 `f45f101`（2026-10-08）「复位重连 -断开重连 next：page 拒绝」

主题：**复位/断开重连稳定性**（sniff 静音、key 丢失卡死、role 缓存、
accept 角色、早期 A2DP 上报、start 前 sniff 屏蔽、HFP SLC 碰撞守卫、
媒体诊断）。这是最大的一批（7 文件 +273/−16）。

#### 1.2.1 `bt_connection_manager.c`

**a. 不再把链路推回 Sniff**（`BTS2MU_GAP_MODE_CHANGE_IND` 处理）：

```c
             if (ind->mode == ACT_MODE && conn->link_status_update)
             {
+                /* Sniff is prohibited on this product's audio links ... The
+                 * old behaviour re-entered a 798-slot sniff here after every
+                 * forced exit — exactly the "reconnect then silent" trap. */
                 conn->link_status_update = 0;
-                hcia_sniff_mode(&ind->bd, BT_CM_SNIFF_INV, BT_CM_SNIFF_INV,
-                                BT_CM_SNIFF_ATTEMPT, BT_CM_SNIFF_TIMEOUT, NULL);
             }
```

**作用**：旧逻辑在强制退出 sniff 后又重新下发 `Sniff_Mode`（798 slot），
链路 5 秒空闲就进 sniff → 流"在跑但无声"。删除后只清标志、永不主动进 sniff。

**b. key 丢失立即断链**（`BTS2MU_GAP_KEYMISSING_IND` 处理）：

```c
         bt_cm_delete_bonded_devs_and_linkkey_by_addr(&ind->bd);
+        /* The key is gone — this link can never encrypt now, and keeping
+         * it up parked the app in "AVDTP deferred; waiting for link
+         * encryption" for ~16s ... Drop the ACL so the reconnect flow
+         * starts a clean SSP re-pair immediately instead. */
+        if (bt_cm_get_conn_by_addr(env, &ind->bd) != NULL)
+        {
+            gap_disconnect_req(&ind->bd);
+        }
```

**作用**：密钥丢失后链路永远无法加密，应用会一直卡在"等加密"门控（实测
~16s），两张 bond 表（本机/耳机）还失步。立即断 ACL，让重连流程走全新
SSP 配对。

**c. ROLE_SWITCH_IND 并入 CFM 处理 + 失败日志**：

```c
     case BTS2MU_GAP_ROLE_SWITCH_CFM:
+    case BTS2MU_GAP_ROLE_SWITCH_IND:
     {
+        /* ROLE_SWITCH_IND has the same layout as ROLE_SWITCH_CFM: it
+         * carries the completion of a role change GAP did not track as
+         * its own pending request (e.g. a raw hcia_switch_role bypass,
+         * or the remote's own switch). Update the cached role from it
+         * just like the CFM — dropping it left info.role stale. */
         ...
+        else
+        {
+            LOG_W("role switch failed, res:%d, role:%d", ind->res, ind->role);
+        }
```

**作用**：应用直接发 `hcia_switch_role()`（绕过 GAP）或对端切换角色时，
GAP 只发 IND 不发 CFM，bt_cm 的 role 缓存就过期了（status 显示错误角色）。
补上 IND 同等处理并给失败加日志（res 值可定位 0x21 拒绝等问题）。

**d. accept 角色可配置**（见 1.2.4 钩子）：

```c
-        hcia_send_acpt_conn_request(&ind->bd, BT_LINK_SLAVE);
+        hcia_send_acpt_conn_request(&ind->bd, bt_get_acpt_link_role());
```

**作用**：`Accept_Connection_Request` 的 role 参数原来硬编码 STAY_SLAVE。
改为查钩子（默认仍 slave，行为不变）。应用覆写返回 `BT_LINK_MASTER`
（become master）——被动回连的耳机成为 relay 的从设备，两个耳机落进同一
piconet 星型，避免"relay 双 slave scatternet 第二副耳机被饿死无声"。

**e. ACL-up 链路策略：只许 role-switch、禁 sniff**：

```c
-        hcia_wr_lp_settings_keep_sniff_interval(&ind->bd, HCI_LINK_POLICY_NO_CHANGE,
-                                                BT_CM_SNIFF_ENTER_TIME, BT_CM_SNIFF_INV,
-                                                BT_CM_SNIFF_INV, BT_CM_SNIFF_ATTEMPT,
-                                                BT_CM_SNIFF_TIMEOUT, NULL);
+        /* Audio link policy: role-switch ONLY — the SNIFF bit (0x0004) is
+         * deliberately cleared. ... With the bit cleared the LM refuses
+         * every Sniff entry — including the headset's own request. */
+        hcia_wr_lp_settings(&ind->bd, 0x0001, NULL);
```

**作用**：默认策略 5s 空闲进 sniff 是"重连后静音"陷阱。策略掩码 `0x0001`
（仅允许 role switch）让 LM 拒绝**一切** sniff 进入（含耳机自己请求）。
本产品音频链路全生命周期不进 sniff。

**f. ACL 断链不再 `a2dp_reset_req()`**（推翻 1.1.5 的条件版）：

```c
-    if (bt_av_get_idx_from_addr(...) < MAX_CONNS) { a2dp_reset_req(); }
+    /* Deliberately NO a2dp_reset_req() here: it is a GLOBAL reset of the
+     * shared AV request state, so any ACL drop ... cancels the PENDING
+     * AVDTP connect of an unrelated headset ... The application performs
+     * the stale-latch cleanup at the one safe moment instead: its connect
+     * timeout runs the reset only when no other connect is in flight
+     * (app_bt.c, APP_BT_CMD_CONNECT_TIMEOUT). */
```

**作用**：`a2dp_reset_req()` 是全局 latch，任何断链触发都会误杀别的耳机
的在飞连接——这是"第二个耳机永远连不完"的元凶。彻底删除；陈旧 latch
的清理挪到**应用层**的连接超时处理（`app_bt.c` 中仅当没有其它连接在飞时
才调 `a2dp_reset_req()`）。

**g. 连接结果日志拆分**（`DM_EN_ACL_OPENED_IND`）：

```c
-        LOG_I("link connected COD:%d Incoming:%d hdl %d res %d\r\n", ...);
+        if (ind->st == HCI_SUCC)
+            LOG_I("link connected COD:%d Incoming:%d hdl %d res %d\r\n", ...);
+        else
+            LOG_I("link connect failed COD:%d Incoming:%d hdl %d res %d\r\n", ...);
```

**作用**：该指示在**失败**时也会上报。旧日志把关机耳机的失败 page 打成
"link connected"（hdl 65535, res≠0），日志分析被严重误导。现在失败打
"link connect failed"。（日志约定：res 为十进制 HCI 错误码。）

#### 1.2.2 `bts2_app_av.c`

**a/b. 信令级 A2DP 立即上报**（出向 `bt_av_hdl_conn_cfm` + 入向
`bt_av_hdl_conn_ind` 各加一段）：

```c
+        /* Tell the application NOW, not only when the media transport
+         * (MUT) arrives: the MUT ind is the sole CONNECTED source and it
+         * is not guaranteed to fire for every flow — when it is silent
+         * the app never registers the link (A2DP=0 in status) while the
+         * AV layer is already streaming. The app ignores duplicates. */
+        {
+            bt_notify_profile_state_info_t profile_state = {0};
+            bt_addr_convert(&msg->bd, profile_state.mac.addr);
+            profile_state.profile_type = BT_NOTIFY_A2DP;
+            profile_state.profile_role = inst->con[i].cfg;
+            profile_state.res = BTS2_SUCC;
+            bt_profile_update_connection_state(BT_NOTIFY_A2DP,
+                    BT_NOTIFY_A2DP_PROFILE_CONNECTED, &profile_state);
+        }
```

**作用**：原来 `BT_NOTIFY_A2DP_PROFILE_CONNECTED` 只在 MUT（媒体通道
MTU 指示）时上报；某些流程 MUT 永远不来 → 应用 A2DP=0 不注册链路，而
AV 层已在流上发数据；应用的 grace 超时还会再发一路 AVDTP 撞出 9b 冲突。
现在**信令连接即上报**；MUT 到达时的重复上报由应用的
"duplicate A2DP success ... ignored" 分支吸收。这也是应用层 A2DP 状态
机（READY、起流、HFP defer）的驱动事件源。

**c. QoS 指示转发给诊断模块**（`BTS2MU_AV_QOS_IND` 分支）：

```c
+        /* Media-path liveness per stream (see bt_avsrc_get_media_stats). */
+        bt_avsrc_note_qos(msg->shdl, msg->buff_st);
```

**d. start 前屏蔽 sniff**（`bt_av_start_stream()`）：

```c
     if (inst->con[con_idx].st == avconned_open)
     {
         USER_TRACE(">> (%d)start the av stream\n", con_idx);
+        /* The prebuilt start path logs "before start stream should
+         * prohibit sniff mode!" but never does it — implement it here.
+         * Clear the SNIFF bit of the link policy (role-switch only) so
+         * the LM refuses every Sniff entry, then kick the link out of
+         * Sniff if it already slid in ... Policy first, exit second: the
+         * kick cannot land back in Sniff afterwards. */
+        hcia_wr_lp_settings(&inst->con[con_idx].av_rmt_addr, 0x0001, NULL);
+        hcia_exit_sniff_mode(&inst->con[con_idx].av_rmt_addr, NULL);
         av_start_req(1, ASSIGN_TLABEL, &inst->con[con_idx].stream_hdl);
```

**作用**：预编译库只打印 "before start stream should prohibit sniff
mode!" 却从不执行。这里补齐：先写策略（禁 sniff），再把已滑入 sniff
的链路踢回 active（耳机重连后空闲 5s 会进 sniff，起流即静音）。顺序
固定"先策略后踢"，踢回后无法再滑入。

#### 1.2.3 `bts2_app_av_src.c / .h` — 媒体转发诊断计数器

新增静态计数 + 两个 API（+66 行）：

```c
+static U32 s_media_req[MAX_CONNS];    /* 交给 av_stream_data_req 的包数 */
+static U32 s_media_nomem[MAX_CONNS];  /* bmalloc 失败丢弃的扇出副本 */
+static U32 s_media_qos[MAX_CONNS];    /* 该流的 AV QOS 指示数 */
+static U8  s_media_buff[MAX_CONNS];   /* 最近的流缓冲深度 0..10 */
+
+void bt_avsrc_note_qos(U16 shdl, U16 buff_st);
+BOOL bt_avsrc_get_media_stats(U8 idx, U32 *req, U32 *nomem, U32 *qos, U8 *buff);
```

扇出循环（`bt_avsrc_sharing`）里计数：

```c
                             s_media_req[i]++;
+                        }
+                        else
+                        {
+                            s_media_nomem[i]++;
```

**作用**：定位"一条流在动、另一条流停滞"的媒体路径僵死——流式连接的
req/qos 计数停走而另一连接继续前进，即 AVDTP 层内媒体路径 wedge。qos
计数只在媒体真正流动时增长，是媒体面活性的直接证据。应用 `intercom
status` 的 streaming 统计即取自这里。CFG_AV 关闭时提供空宏。

#### 1.2.4 `bts2_app_generic.c / .h` — accept 角色钩子

```c
+/* Link role used when ACCEPTING an incoming ACL connection
+ * (HCI Accept_Connection_Request role parameter):
+ * BT_LINK_MASTER(0) = become master (role switch at accept),
+ * BT_LINK_SLAVE(1)  = stay slave. Default keeps the historical
+ * stay-slave behaviour; override in the application ... */
+__WEAK uint8_t bt_get_acpt_link_role(void)
+{
+    return BT_LINK_SLAVE;
+}
```

**作用**：配合 1.2.1d。默认行为不变；产品策略（返回 MASTER）在应用层
覆写（`app_bt.c: bt_get_acpt_link_role()` → `BT_LINK_MASTER`）。

#### 1.2.5 `bts2_app_hfp_ag.c` — HFP SLC 碰撞守卫（+94 行）

背景：耳机常在 AVDTP 之后**自己**发起 HFP SLC（Anyka OWS T700 实测 ~3s）。
我们此时发 AG→HF 连接会撞进对端在飞的 SLC，RFCOMM 内两败俱伤
（CONN_CFM res 19），链路只剩音乐直到下次机会。

新增（文件头）：

```c
+#define BT_HFP_AG_INCOMING_SLC_WINDOW_MS 10000U
+
+static BTS2S_BD_ADDR g_hfp_ag_incoming_bd;
+static uint32_t g_hfp_ag_incoming_tick;
+static uint8_t g_hfp_ag_incoming_slc;
+
+static uint8_t bt_hfp_ag_bd_equal(BTS2S_BD_ADDR *a, BTS2S_BD_ADDR *b);
+static void bt_hfp_ag_incoming_slc_start(BTS2S_BD_ADDR *bd);   /* 置位+时间戳 */
+static void bt_hfp_ag_incoming_slc_clear(BTS2S_BD_ADDR *bd);   /* 清除 */
+static uint8_t bt_hfp_ag_incoming_slc_pending(BTS2S_BD_ADDR *bd); /* 窗口内=1 */
```

拦截点一（`BTS2MU_AG_CONN_REQ`，对端开 RFCOMM）：

```c
+        /* Remote is opening the SLC itself right now — remember it so our
+         * own AG->HF connect does not fire into this in-flight setup ... */
+        bt_hfp_ag_incoming_slc_start(&con_msg->bd);
         hfp_ag_connect_ind_res(&con_msg->bd, con_msg->rfcomm_chnl, 1); // accept
```

拦截点二（`bt_hfp_ag_connect_request`，我们发起 AG→HF 之前）：

```c
 int bt_hfp_ag_connect_request(BTS2S_BD_ADDR *bd)
 {
+    if (bt_hfp_ag_incoming_slc_pending(bd))
+    {
+        USER_TRACE(" -- hfp ag connect suppressed: remote SLC in progress\n");
+        return (int)BT_ERROR_IN_PROGRESS;
+    }
     hfp_ag_connect(bd, 0);
     return 0;
 }
```

清除点（`bt_hfp_ag_app_device_state_changed`）：

```c
+    if ((con_msg->device_state == HFP_DEVICE_CONNECTED) ||
+            (con_msg->device_state == HFP_DEVICE_DISCONNECTED))
+    {
+        /* Whatever direction opened the session, it has now resolved —
+         * our own outbound connect is free to fire again. */
+        bt_hfp_ag_incoming_slc_clear(&con_msg->bd);
+    }
```

失败上报（同函数，原来只报成功）：

```c
+        else if (con_msg->res != HFP_AG_SUCC)
+        {
+            /* A connect attempt resolved with an error (e.g. res 19 ...) Only
+             * CONNECTED/DISCONNECTED were reported before, so the app sat
+             * blind until its next blind retry. Report the failure as a
+             * CONNECTED event carrying the error so the app can back off
+             * briefly and retry into a clean window. */
+            bt_notify_profile_state_info_t profile_state;
+            ...
+            bt_profile_update_connection_state(BT_NOTIFY_HFP_AG,
+                    BT_NOTIFY_AG_PROFILE_CONNECTED, &profile_state);
+        }
```

**作用**：对端 SLC 在飞（窗口内）时抑制我们的连接请求，返回
`BT_ERROR_IN_PROGRESS`（应用侧收到后按"短轮询推迟、不计次"处理）；会话
一旦解析（无论方向、成功或断开）即解除；**失败也上报**，应用不再失明。
应用配套：`APP_BT_HFP_DEFER_GRACE_MS`（A2DP 起来后先等对端自开 SLC）、
`APP_BT_HFP_RETRY_FAST_MS` 抑制重试。

---

### 1.3 `6d247a5`（2026-10-09）「readme buds 6 问题」

主题：**入向 page 可由应用拒收并转主动外连**（配合复位回连→转外连的
"变主"策略）。

#### 1.3.1 `bts2_app_generic.c / .h` — 是否 accept 入向 page 的钩子

```c
+/* Decide whether an incoming ACL page is ACCEPTED (1, default) or
+ * declined at HCI level (0). Called from the BT task at connect-request
+ * time, before Accept/Reject Connection_Request is sent. Returning 0
+ * lets an application convert a passive reconnect into its own outgoing
+ * connect; the override is then responsible for starting that connect
+ * (the connect-request notification is still delivered afterwards). */
+__WEAK uint8_t bt_should_accept_incoming_conn(BTS2S_BD_ADDR *bd)
+{
+    (void)bd;
+    return 1;
+}
```

#### 1.3.2 `bt_connection_manager.c` — accept 路径接入钩子

`bt_cm_hci_connect_request_event_hdl()` 重构 accept 流程：

```c
-        if (conn == NULL)
-        {
-            conn = bt_cm_conn_alloc(env, &ind->bd, link_type);
-            if (conn == NULL)
-            {
-                hcia_send_rej_conn_request(&ind->bd, HCI_ERR_REJ_BY_RMT_NO_RES);
-                return;
-            }
-        }
-        hcia_send_acpt_conn_request(&ind->bd, bt_get_acpt_link_role());
+        if (!bt_should_accept_incoming_conn(&ind->bd))
+        {
+            /* Application declined the passive accept — it converts this
+             * page into its own outgoing connect. Reject so the remote
+             * stops paging, but still fall through to the notify below so
+             * the application can start that connect. */
+            LOG_I("declined incoming page; app converts to outgoing connect");
+            hcia_send_rej_conn_request(&ind->bd, HCI_ERR_REJ_BY_RMT_NO_RES);
+        }
+        else
+        {
+            if (conn == NULL)
+            {
+                conn = bt_cm_conn_alloc(env, &ind->bd, link_type);
+                if (conn == NULL)
+                {
+                    hcia_send_rej_conn_request(&ind->bd, HCI_ERR_REJ_BY_RMT_NO_RES);
+                    return;
+                }
+            }
+            hcia_send_acpt_conn_request(&ind->bd, bt_get_acpt_link_role());
+        }
```

**作用**：应用可以在 HCI 层**拒收**入向 page（对端停止 page），同时仍然
收到连接请求通知，由应用把它**转成自己的外连**（外连必为 master，且走
实测可靠的"主动连接"路径）。拒绝后不分配 conn，不 accept。

应用层的覆写策略（`app_bt.c: bt_should_accept_incoming_conn()`）：只在
"一副耳机已在放音且媒体面已稳定、对端是已配对/在队列中的设备、该槽位
空闲、转换尝试未超上限（`APP_BT_CONVERT_MAX_ATTEMPTS`）"时拒收；
拒收同时投递 `APP_BT_CMD_CONVERT_INCOMING` 立即外拨。talk/scan/error 模式、
未知设备、在连/在放设备一律照常 accept——保证原有功能不受影响。

---

### 1.4 `33f31a8`（2026-10-13）「log 显示bug」

主题：**入向 AVDTP 停滞接管（A1）+ HFP 半开 SLC 拆除（B1）**——针对
Redmi Buds 类耳机"开信令/RFCOMM 后既不 set-config 也不说 AT，10 秒后
监督超时断链"的死循环。

#### 1.4.1 `bts2_app_av.c / .h` — 新增 `bt_av_drive_stream_setup()`

```c
+/* Drive the stream-setup chain (discover -> set cfg -> open) on a signaling
+ * connection whose remote opened AVDTP but never followed through with the
+ * set-config (some headsets stall right after the signaling connect). The
+ * outgoing path runs this chain automatically from bt_av_hdl_conn_cfm; the
+ * incoming path parks at avconned waiting for the remote forever. No-op when
+ * the stream is already configured/open/streaming or the device has no AV
+ * connection. Returns 0 when a discover was queued, 1 when there was nothing
+ * to do, <0 when the device is not connected. */
+int bt_av_drive_stream_setup(BTS2S_BD_ADDR *bd)
+{
+    bts2s_av_inst_data *inst = bt_av_get_inst_data();
+    U8 con_idx;
+    ...
+    con_idx = bt_av_get_idx_from_addr(inst, bd);
+    if (con_idx > MAX_CONNS) return -1;
+
+    if (inst->con[con_idx].in_use && (inst->con[con_idx].st == avconned) &&
+            (inst->con[con_idx].role == ACPTOR))
+    {
+        /* Incoming only: the outgoing path already chains discover from
+         * bt_av_hdl_conn_cfm — re-driving it would double-send discover
+         * into a live chain. */
+        USER_TRACE(">> (%d)drive stalled stream setup: discover remote SEP\n", con_idx);
+        av_discover_req(inst->con[con_idx].conn_id, ASSIGN_TLABEL);
+        return 0;
+    }
+    return 1;
+}
```

头文件声明 + CFG_AV 关闭时的空宏：

```c
 extern void bt_av_start_stream(uint8_t con_idx);
+extern int bt_av_drive_stream_setup(BTS2S_BD_ADDR *bd);
 ...
 #define bt_av_start_stream(con_idx)
+#define bt_av_drive_stream_setup(bd) 0
```

**作用**：出向连接信令一建好，中间件自动走 discover→set-cfg→open 链；
**入向**连接（`bt_av_hdl_conn_ind`）设完 `st=avconned` 后就永远等对端
set-config。对端停滞时这条链没人拉——本函数就是补拉的人：只对入向
（`role == ACPTOR`）且仍停在 `avconned` 的连接发 `av_discover_req`，
后续 set-cfg/open 由原有 cfm 处理链自动完成。已 open/streaming 或无
AV 实例则安全空操作（返回 1）。

应用侧驱动（`app_bt.c`）：`APP_BT_AV_STALL_DRIVE_MS 5000U` 看门狗 +
`av_stall_watch` 标志。A2DP 首次（信令级）成功时武装；"duplicate A2DP
success"（= MUT 打开）到达即解除（健康链路只挂 ~200ms）；5 秒未见
MUT 则超时分支调用本函数（在 `!app_bt_connection_pending` 陈旧清理**之**
前判断，因为 READY 不算 pending）。A2DP 断链/槽位复位/发起新连接时清标志。

#### 1.4.2 `bts2_app_hfp_ag.c` — SLC 窗口收紧 + 过期拆除

窗口 10s → 4s（注释同步改写）：

```c
-#define BT_HFP_AG_INCOMING_SLC_WINDOW_MS 10000U
+ * The window only needs to cover a WORKING remote SLC (RFCOMM open ->
+ * SLC resolve is ~2s), but it must expire well before the 10s link
+ * supervision timeout: some headsets open the RFCOMM DLC and then never
+ * speak AT at all (Redmi Buds 4 Pro stalls exactly there), and the old
+ * 10s window outlived the link itself — our AG connect was suppressed
+ * every time and the headset died music-only. ...
+#define BT_HFP_AG_INCOMING_SLC_WINDOW_MS 4000U
```

`bt_hfp_ag_incoming_slc_pending()` 改为纯状态检查（过期逻辑上移）：

```c
 static uint8_t bt_hfp_ag_incoming_slc_pending(BTS2S_BD_ADDR *bd)
 {
-    if (!g_hfp_ag_incoming_slc) return 0;
-    if (!bt_hfp_ag_bd_equal(...)) return 0;
-    if ((rt_tick_get() - g_hfp_ag_incoming_tick) >= ...)
-    {
-        g_hfp_ag_incoming_slc = 0;   /* 静默过期 */
-        return 0;
-    }
-    return 1;
+    return g_hfp_ag_incoming_slc && bt_hfp_ag_bd_equal(&g_hfp_ag_incoming_bd, bd);
 }
```

`bt_hfp_ag_connect_request()` 增加过期拆除分支：

```c
     if (bt_hfp_ag_incoming_slc_pending(bd))
     {
+        if ((rt_tick_get() - g_hfp_ag_incoming_tick) <
+                rt_tick_from_millisecond(BT_HFP_AG_INCOMING_SLC_WINDOW_MS))
+        {
             USER_TRACE(" -- hfp ag connect suppressed: remote SLC in progress\n");
             return (int)BT_ERROR_IN_PROGRESS;
+        }
+        /* Window expired with the remote's SLC never resolving (it opened
+         * the RFCOMM DLC and then never spoke AT — Redmi Buds stalls exactly
+         * there). The half-open session is a dead end: tear it down so the
+         * caller's retry starts a fresh SLC instead of colliding with the
+         * zombie DLC (CONN_CFM res 19). */
+        g_hfp_ag_incoming_slc = 0;
+        USER_TRACE(" -- hfp ag stalled remote SLC torn down; retry shortly\n");
+        hfp_ag_disconnect(bd);
+        return (int)BT_ERROR_TIMEOUT;
     }
```

**作用**：Redmi 类耳机打开 RFCOMM DLC 后不说 AT——半开会话是死胡同，
而旧的 10 秒窗口比 10 秒监督链路还长，我们的 AG 连接每次都被抑制，链路
到点死亡、耳机全程无声。现在：窗口 4s 足够罩住**正常** SLC（RFCOMM 开→
解析 ~2s），过期未解析即**拆掉僵尸 DLC** 并返回 `BT_ERROR_TIMEOUT`；
应用侧（`app_bt.c`）映射为 500ms 快速重试
（`APP_BT_HFP_TEARDOWN_RETRY_MS`）发起全新 SLC，赶在 10s 监督断链前完成。
配套把 `APP_BT_HFP_DEFER_GRACE_MS` 8000→5000（首次请求提前；对端若仍在
开 SLC 会被 IN_PROGRESS 抑制，无碰撞风险）。

---

### 1.5 未提交（2026-10-14）「连接期电流音修复：SBC 编码器互斥 + 发送侧平滑」

针对「A 播音乐时连 B，A 出现电流音」的修复。机制分析见 `project/log`
分析结论：连接风暴期间 BT 任务的 `rt_enter_critical` 包住多毫秒 SBC
编码（调度器锁死 → i2srx 线程无法排空录音 pipe → PCM 截断拼接），
producer 线程优先级又低于 BT 任务被饿死（share ring 永久排空 →
"buffer empty"），再叠加发送侧 0ms 双发、负延迟转 U32 巨定时器、
`bmalloc` 失败整包丢弃。修复全部为通用缺陷修复，无耳机特定逻辑。

#### 1.5.1 `bts2_app_av_src.c / .h` — 导出 SBC 编码器互斥锁

SBC 编码器是全局状态、不可重入（`av_sbc_api.h`）。原保护是
`rt_enter_critical`（调度器锁）+ producer 优先级低于 BT 任务的"靠
优先级串行"。前者会阻塞 i2srx 排空录音 pipe（PCM 丢弃的根因），故
改为真正的互斥：

- 新增 `g_sbc_lock`（`rt_mutex`，递归，懒初始化关中断自保护）与
  导出接口 `bt_avsrc_sbc_lock()` / `bt_avsrc_sbc_unlock()`（头文件
  同步声明）；
- 三个编码器调用点全部包锁：`bt_avsrc_prepare_sbc` 的
  `bts2_sbc_encode_cfg`、`bt_avsrc_hdl_disc_handler` 的
  `bts2_sbc_encode_completed`、休眠路径 `bt_avsrc_send_data` 编码环。

应用侧（`app_a2dp_share.c`）编码调用点同步换锁，因此 producer 优先
级可以提到 BT 任务之上而无重入竞争。

#### 1.5.2 `bts2_app_av_src.c` — sharing 发送侧平滑（`bt_avsrc_sharing`）

- **负延迟地板**：空路径下 `m_sec_time_4_next_pkt`（S32）为负时按
  0 转 U32 会得到巨定时器、发送停摆（对应 227ms 级断流）；钳到最小
  1ms；
- **追赶下限**：`bts2_timer_ev_add(0, ...)` 0ms 双发抖动 → 改 1ms；
- **去 `bmalloc`**：每包 `bmalloc(pkt_len)` 失败即静默整包丢（所有
  耳机一起卡）→ 改静态暂存缓冲 `s_share_pkt_buf[1024]`，配
  `pkt_len` 合法性检查，失败仅跳帧不再丢分配；
- **限频日志**："buffer empty" 每 16 次打 1 条，避免风暴期间刷屏。

#### 1.5.3 应用侧配合（`app_a2dp_share.c`，不属子模块）

- producer 内 `rt_enter_critical` 包编码 → 换 `bt_avsrc_sbc_lock`；
- producer 提优先级 13 → 11（`RT_THREAD_PRIORITY_HIGH - 1`：高于
  BT 任务 12 不被风暴饿死，低于 i2srx 10 保证排空 pipe）；
- mq→share ring 的搬运从 BT 任务定时器移到 producer 线程（ring 唯
  一写者 SPSC；put 加短临界区防 BT 任务 get 撕裂环回簿记）；
- 3 处 `rt_ringbuffer32_reset` 包临界区；discard 改独立暂存缓冲
  `g_discard_record`，与 pump 互不踩。

#### 1.5.4 录音 pipe 加深（`rtos/.../audio.c` + Kconfig，不属子模块）

pdm/i2s 录音 pipe 从 `CFG_AUDIO_RECORD_PIPE_SIZE * 2`（约 3.7ms）改
为可配置倍数 `CFG_AUDIO_RECORD_PIPE_MULT`（Kconfig，默认 2 保持其它
产品不变），本工程 `proj.conf` 设 16（约 29ms）以扛住连接风暴期间
约 25ms 的调度停顿。只花 RAM，健康路径行为不变。

### 1.6 未提交（2026-10-14）「HFP 后台采集断开误回滚修复」（`app_bt.c`，不属子模块）

现象（日志 `窗口2_COM30_(2026-10-14-17-42-57)_ui.log`）：出向连接
后 A2DP 已放歌但 HFP=0；远端开 RFCOMM DLC 后不发 AT（SLC 卡死），
中间层 4s 窗口按设计拆除僵尸 SLC，拆除产生的 `HFP AG disconnected`
事件被 `app_bt_handle_hfp` 的"未完成连接回滚"分支（原 app_bt.c:3465）
误判为连接失败，对健康放歌链路执行 `app_bt_abort_connection`
（ACL reason 22）——即"播放中主动断开"，并且精确掐死前一行刚排好的
500ms 重连（"reconnecting fresh"）。两次回滚后耳机需重启才能回连。

修复（通用，仅 app_bt.c 两处，不碰 middleware 子模块）：
- DISCONNECTED 分支入口先取 `had_hfp = slot->hfp_connected`（清位之前）；
- 回滚条件前新增分支：`stage == READY && a2dp_connected && !had_hfp &&
  used && !release_pending`（HFP 从未连上、只是后台采集被拆/被远端
  丢弃）→ 不回滚，打印 "background HFP closed (never connected); keeping
  A2DP link"，仅在无在途重试时补排 500ms 重试；其余情形（真·未完成
  连接、HFP 曾连上后掉线、talk/SCO 受影响、ERROR 模式）行为全部不变。
  与 `APP_BT_CMD_HFP_TIMEOUT` 的既有设计一致："Never tear down a link
  that is playing music"，重试超 10 次自动落到 music-only 兜底。

预期：不再出现"播放中主动断开"；HFP 由 500ms 重试 + 远端迟来 SLC
补齐；耳机不再被拆到需重启。已知边界：若远端在每次拆除后 500ms 内
反复重开又卡死的僵尸 SLC，重试循环不计数（与改前 suppress/teardown
语义一致），代价为日志噪声而非断流。

附带观察（未改）：入向连接时远端自发 AVDTP 每 ~500ms 被 L2CAP 拒
（`conn rej psm: 19, flag: 0, idx 255`，字符串在预编译 bts2 库）×10
直到 8s grace 到期由我方主动连才建通；"等远端自开 AVDTP"的等待实际
纯付 8s。方向待下次复现配 btsnoop 定论，暂不动 grace（它是为双方同
开会两败俱伤的真实案例加的）。

### 1.7 未提交（2026-10-14）「扫描结果列表扩到 20 条 + 扫描汇总丢失修复」

现象（日志 `窗口3_COM14_(2026-10-14-15-34-58)_ui.log`）：`intercom scan`
后 `[intercom] inquiry complete: N device(s)` 及编号列表一次都没打印；
协议栈侧 `>> inquiry finished` 后的 `<< idx: NN--address` dump 也只剩
1 条（11 台被扫到却只回显 1 台）。

根因两处：
1. 协议栈结果表 `inquiry_list[]` 容量 `MAX_DISCOV_RESS = 10`
   （`bts2_app_demo.h`），且写入计数器满后**归零重填**（原
   `bts2_app_generic.c:1440`）——第 11 台起从 idx 00 覆盖旧条目，完成
   dump 只打后段残留（正好 20/10 台填满时计数归 0，dump 一条不打）；
   实现就在可见源码里，不在预编译核内（预编译核只逐台投递
   `BTS2MU_GAP_DISCOV_RES_IND`）。
2. `app_bt.c` ACL 连接完成处理里**无条件**清 `g_scanning`（原 2475）：
   `intercom scan` 发起时恰有一路 auto-reconnect page 在途，其**失败**
   的 connect-complete（res=2）落在 inquiry 启动后 ~3ms，把
   SCANNING→IDLE；10s 后 `INQUIRY_CMP` 因 `!g_scanning` 跳过
   `app_bt_dump_discovery()`，应用侧 20 条编号列表整个被吞。

修复（通用）：
- `bts2_app_demo.h`：`MAX_DISCOV_RESS` 10 → 20（`inquiry_list`/`bd_list`
  各多 10×~8B，约 +160B 静态内存）；
- `bts2_app_generic.c`：写入计数回绕 → **饱和保留前 20 条**（第 21 台起
  不再入表、不再覆盖），完成 dump 与实收条数一致；
- `app_bt.c`：清 `g_scanning` 的条件收紧为 `info->res == HCI_SUCC`
  ——只有**真建链成功**（入向 page 落地、inquiry 确被吞）才清扫描标志，
  失败/取消的 page 不再打断扫描。其余入连/拒连路径不动。

预期：`intercom scan` 后两份列表（协议栈 `<< idx` dump + 应用
`[intercom] inquiry complete` 编号列表）各最多 20 条、完整打印；扫描
期间在途 page 失败不再吞汇总。回归面：≤10 条结果时协议栈行为与改前
逐条一致（idx 编号、打印格式不变）；真·入向建链仍会清扫描（原防挂死
语义保留），且扫描命令入口的 stale-scan 自愈仍在兜底。

### 1.8 未提交（2026-10-20）「连接第二副耳机时转发卡顿：媒体泵迁出 bts2 定时器」

现象（Frontline `窗口2_COM47_(2026-10-20-13-50-03).pcap`）：A 转发
播放中连接 B，A 的媒体流出现① 360ms 空窗（13:51:18.851→19.211，
恢复后 seq 168/169 以 1ms 间距追发）② 连接窗口（34.4–37.0s）内
47–78ms 发包间隔抖动 ③ seq 556 整包消失。需求：连接全程不暂停、不
卡顿（1.5 之前的"先停流再连"策略已废弃）。

根因（①，本次修复目标）：媒体泵 `bt_avsrc_sharing_cb` 是 bts2 定时
器回调，与 L2CAP/RFCOMM/AVDTP/HFP 信令共用 "bts" 任务。反汇编
`lib_bt_gcc.a` 的 `bts2_task_run`：派发顺序为**先清空全部消息队列、
再派发到期定时器**，期间消息不断则不回头——HFP SLC 建链让队列持续
非空，29ms 发包定时器被结构性饿死 360ms。空窗期间信令 ACL 一直在
发（`av_stream_data_req` 的消息通路全程存活），发送能力在、只是定时
器没被调度；1ms 追发是饥饿签名而非成因。Frontline 的
"ACL credits: -1" 是控制器 4 个 ACL 缓冲持续打满的边界视图，非独立
根因。

修复（通用）：
- `bts2_app_av_src.c / .h`：新增 **"a2dptx" 线程**（优先级
  `HIGH-1`=11，高于 bts 12）承接媒体节奏：按 `m_sec_time_4_next_pkt`
  时刻睡眠 → 调 `bt_avsrc_sharing()` → 自重排。可行性依据：反汇编确
  认 `av_stream_data_req` = `bt_mem_alloc` + `bts2_msg_put`（其
  `BTS2_ENTER_CRITICAL` 为 `os_mutex_take`），任意线程可调；实际
  L2CAP 发送仍在 bts 任务消息优先级执行——**预编译核零改动**。原 5 处
  `bts2_timer_ev_add(..., bt_avsrc_sharing_cb, ...)` 重排点全部改
  `bt_avsrc_sharing_rearm()`，回调删除；追赶步长 1ms → 5ms 限速；新增
  `bt_avsrc_sharing_sched/cancel/armed`，audio close / A2DP_CMD_PAUSE /
  disc handler 三处停泵点配套 cancel（`stop_req` 粘滞，在途泵不再自
  续）；
- `bt_avsrc_tx_lock`（照抄 `g_sbc_lock` 懒初始化模式）包住
  `bt_avsrc_sharing` 与 `bt_avsrc_send_data` 全体（对 `src_data` 节拍
  字段读改写互斥），锁序恒为 tx_lock → sbc_lock。**对 AVDTP 状态机的
  `st/cfg/suspend_pending/stream_hdl` 写不加锁**（与 1.x 惯例不同，提
  前说明）：均为单字原子写，竞争最坏结果是状态转换瞬间多发/少发一包
  （协议栈丢弃），而包锁会经应用回调引入死锁面，收益不成比例；
- 应用侧（`app_a2dp_share.c`，不属子模块）：控制泵迁到自有线程
  "a2dpp"（优先级 12，与 bts 时间片轮转，风暴下有前向进度）；踢泵改
  `bt_avsrc_sharing_sched`（"a2dptx" 成为 share ring 唯一读者，
  `s_share_pkt_buf` 免跨线程锁）；新增断链重踢
  （`kicked && !armed && ring>0`）——suspend 门吞掉最后一次重排后 5ms
  内自愈，顺带修掉旧实现"kicked 后链死无人再踢"的隐患。

预期：① 类信令风暴空窗消除；暂停/关闭/断开停泵语义不变；官方
`example/bt/a2dp_sharing` 直接调 `bt_avsrc_sharing()` 的用法经包装函
数兼容并同样受益。未覆盖（后续项）：② seq 556 是 per-sink
`bmalloc(pkt_len)` 失败整包静默丢（`s_media_nomem` 计数可证，应改预
分配池）；③ 连接窗口抖动是 page/SSP 空口争用 + 4 ACL 缓冲打满，需连
接编排与耳机侧 jitter buffer。

## 2. 累计改动清单（按主题）

### 2.1 ACL / 连接管理器（`bt_connection_manager.c`，净 +112/−15）

| # | 修改 | 作用 | 提交 |
| --- | --- | --- | --- |
| 1 | bond NVDS 读取越界修复 | 内存安全（跨固件 bond 记录） | 34ba0cc |
| 2 | `encrypted` 字段 + 置位 | 链路加密状态暴露给门控 | 34ba0cc |
| 3 | 拒绝重复 page（ACL 已建） | 防 0x0B 换链 churn | 34ba0cc |
| 4 | `a2dp_reset_req` 条件化→**彻底删除** | 防全局 reset 误杀无关在飞连接；清理上移应用 | 34ba0cc→f45f101 |
| 5 | 禁 sniff 重入（删 `hcia_sniff_mode`） | 防"退出又进 sniff"静音 | f45f101 |
| 6 | KEYMISSING 即断链 | 防"等加密"卡死 16s | f45f101 |
| 7 | ROLE_SWITCH_IND 并入缓存更新 + 失败日志 | role 缓存不失效 | f45f101 |
| 8 | accept 角色走 `bt_get_acpt_link_role()` | 被动回连可变主 | f45f101 |
| 9 | ACL-up 策略 0x0001（仅 role-switch） | 全链路禁 sniff | f45f101 |
| 10 | 日志拆分 connected/failed | 日志不再误导 | f45f101 |
| 11 | 入向 page 可拒收（`bt_should_accept_incoming_conn`） | 被动回连转外连 | 6d247a5 |

### 2.2 AVDTP / A2DP Source（`bts2_app_av.c/.h`、`bts2_app_av_src.c/.h`，净 +159/−1）

| # | 修改 | 作用 | 提交 |
| --- | --- | --- | --- |
| 12 | 失败事件不过滤 9a/9b | 应用可见冲突/重复 | 34ba0cc |
| 13 | 入向连接 `cfg = AV_AUDIO_SRC` | 修"连接了但无声" | 34ba0cc |
| 14 | 信令级 CONNECTED 立即上报（出/入向） | 应用及时注册链路；MUT 缺失也不失明 | f45f101 |
| 15 | start 前策略 0x0001 + 踢出 sniff | 修起流静音 | f45f101 |
| 16 | 媒体诊断计数 req/nomem/qos/buff | 定位媒体面 wedge | f45f101 |
| 17 | `bt_av_drive_stream_setup()` | 入向停滞接管（A1） | 33f31a8 |

### 2.3 HFP AG（`bts2_app_hfp_ag.c`，净 +100/−0；33f31a8 对 f45f101 插入行有就地改写）

| # | 修改 | 作用 | 提交 |
| --- | --- | --- | --- |
| 18 | SLC 碰撞守卫（窗口/置位/清除/抑制） | 防 AG↔HF 对撞 res 19 | f45f101 |
| 19 | 连接失败也上报 | 应用不再盲重试 | f45f101 |
| 20 | 窗口 10s→4s + 过期拆僵尸 DLC + TIMEOUT | 修 Redmi 半开 SLC 死循环（B1） | 33f31a8 |

### 2.4 通用钩子（`bts2_app_generic.c/.h`，净 +26/−0）

| # | 修改 | 作用 | 提交 |
| --- | --- | --- | --- |
| 21 | `__WEAK bt_get_acpt_link_role()` | accept 角色策略点（默认 slave） | f45f101 |
| 22 | `__WEAK bt_should_accept_incoming_conn()` | 入向 accept/reject 策略点（默认 accept） | 6d247a5 |
| 23 | `MAX_DISCOV_RESS` 10→20（`bts2_app_demo.h`） | 扫描结果表 20 条 | 未提交 1.7 |
| 24 | inquiry_list 写入回绕→饱和 | 满表不再覆盖旧条目，dump 完整 | 未提交 1.7 |

产品策略均在应用层覆写：`app_bt.c:5520` 返回 `BT_LINK_MASTER`；
`app_bt.c:5582` 实现"转外连"判定并投递 `APP_BT_CMD_CONVERT_INCOMING`。

---

## 3. 应用层移植可行性评估

### 3.1 评估方法

把每条修改拆成两个问题：

1. **拦截点/事件源在哪里？** —— 若修改拦截的是中间件**内部消息**
   （如 `BTS2MU_AG_CONN_IND`、QOS_IND）或**内部执行路径**（accept 发包
   前、断链处理中、预编译库调用旁），应用层根本收不到/来不及，无法移植。
2. **动作是否在公开 API 上？** —— `hcia_*`（`hci_api.h`）、
   `bt_av_get_inst_data()`、`av_discover_req()`、`hfp_ag_disconnect` 等
   应用都能直接调用（`app_bt.c` 已在用 `hcia_switch_role`、
   `gap_wr_super_vision_timeout_req`、`a2dp_reset_req`），动作类修改可移植。

结论分三类：**✓ 可移植**（应用层等价实现）、**△ 折中**（需中间件留一个
极小钩子/或用轮询近似）、**✗ 不可移植**（必须留在中间件）。

### 3.2 逐项评估

| # | 修改 | 拦截点/事件源 | 移植 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | bond NVDS 越界修复 | bt_cm 内部函数 | **✗** | 纯内存安全修复，应用无任何钩子 |
| 2 | `encrypted` 标志 | bt_cm 状态表 | **✗** | ENCRYPTION_IND 不上报应用；应用无从得知加密完成 |
| 3 | 拒绝重复 page | HCI Connect Request | **△** | 判定策略可上移：用 #22 钩子在应用判"已有 ACL 则拒"；但 HCI 拒绝动作本身必须在钩子回调内（钩子一行留中间件） |
| 4 | 删除 `a2dp_reset_req` | bt_cm 断链路径 | **✗** | 应用**无法阻止**中间件调用它；只有删掉才消除误杀。"何时清理"已上移应用（超时分支），但"何时不许清理"必须留 |
| 5 | 失败事件不过滤 | 事件上报链 | **✗** | 被中间件吞掉的事件应用永远看不到 |
| 6 | 入向 `cfg=AV_AUDIO_SRC` | AV 实例状态写入 | **△** | 结构体公开，应用收到首次 CONNECTED 后可"戳" `inst->con[i].cfg`；但与 BT 任务存在竞争窗口，属 hack。正确做法留中间件（一行赋值） |
| 7 | 信令级 CONNECTED 上报 | 事件上报链 | **△** | 目的（尽早感知信令连接）可由应用**轮询** `inst->con[].st` 实现（share pump 已周期读实例状态）；事件级准确、零开销则必须留中间件 |
| 8 | start 前禁 sniff+踢出 | `bt_av_start_stream` 内 | **✓** | 应用在调 `bt_av_start_stream()` 之前自己发 `hcia_wr_lp_settings(0x0001)` + `hcia_exit_sniff_mode()`（`app_a2dp_share.c` 三处 `bt_av_start_stream` 调用点前插入），顺序可保持"先策略后踢" |
| 9 | `bt_av_drive_stream_setup` | 新增 API | **✓** | 全部由公开 API 组成：`bt_av_get_inst_data` + `bt_av_get_idx_from_addr` + `av_discover_req` + 公开的 `st/role/conn_id` 字段。可整体移入 `app_bt.c`（A1 看门狗本就在应用） |
| 10 | 媒体诊断计数 | AV QOS_IND + 扇出循环 | **✗** | QOS_IND 应用收不到；扇出（`bt_avsrc_sharing`）在中间件。应用只能在自己喂环形缓冲处近似计数，qos/buff 指标拿不到 |
| 11 | ACL-up 策略 0x0001 | bt_cm ACL-up | **✓** | 应用 ACL-up 处理已有同样写（`bt_interface_wr_link_policy_setting`），中间件这条是双重保障，可去掉而仅留应用侧 |
| 12 | 禁 sniff 重入 | bt_cm MODE_CHANGE | **✗** | `hcia_sniff_mode` 是直发 HCI 的 Sniff_Mode 命令，**绕过**链路策略；应用无法拦截，只能事后反复踢出（丑且有窗口）。必须留 |
| 13 | KEYMISSING 断链 | GAP KEYMISSING_IND | **✗** | 事件源在中间件且不上报应用；应用收不到触发就无从 `gap_disconnect_req` |
| 14 | ROLE_SWITCH_IND 缓存 | bt_cm 角色缓存 | **✗** | 应用读的角色来自 bt_cm 的表；修缓存本身必须在 bt_cm |
| 15 | `bt_get_acpt_link_role` 钩子 | HCI Accept 角色参数 | **△** | accept 发包在 BT 任务、通知之前，应用无法改 role 参数。等价替代：accept 后应用再 `hcia_switch_role` 抢主——存在短暂 slave 窗口且历史上本地被拒过（现缓存修复后可试）。钩子本身是通用一行、可回馈 SDK 主线，建议保留 |
| 16 | `bt_should_accept_incoming_conn` 钩子 | HCI Accept/Reject 决策点 | **△** | 同上：HCI 层拒收必须发生在 accept 流程里；应用"忽略通知"无法让对端停止 page。产品策略（转换判定）已在应用，钩子一行建议保留 |
| 17 | SLC 碰撞守卫 | `BTS2MU_AG_CONN_IND` + 连接入口 | **✗** | 触发源（对端开 RFCOMM）与拦截点（`bt_hfp_ag_connect_request`）都是中间件内部；应用只能"盲目 defer + 盲拆"近似，时序精度与安全性都差一截 |
| 18 | HFP 失败上报 | 事件上报链 | **✗** | 被吞事件（同 #5） |
| 19 | 过期拆僵尸 DLC（B1） | 守卫内部 | **△** | 拆除动作应用可调（`bt_interface_disc_ext` / `hfp_ag_*`）；但"窗口过期、半开未解析"的判断依据是 #17 的守卫状态，判断逻辑必须与守卫同处 |

### 3.3 汇总

| 类别 | 数量 | 条目 |
| --- | --- | --- |
| ✓ 可移植 | 3 | #8 start 前 sniff 屏蔽、#9 drive_stream_setup、#11 ACL-up 策略 |
| △ 折中（钩子/轮询近似） | 6 | #3 #6 #7 #15 #16 #19 |
| ✗ 必须留中间件 | 13 | #1 #2 #4 #5 #10 #12 #13 #14 #17 #18 等 |

**能否"全部"移植到应用层？——不能。** 约六成条目（13/22）的拦截点或
事件源在中间件内部，应用层收不到事件、拦不住动作、修不了内部状态：
被吞事件（#5/#18）、被删的有害调用（#4/#12）、内存安全修复（#1）、
bt_cm 状态缓存（#2/#14）、扇出循环内计数（#10）、HFP 守卫（#17）都
无法等价复现。强行用"轮询 + 盲拆 + 事后踢 sniff"近似会把毫秒级时序
退化到百毫秒级，且丢掉事件精度，得不偿失。

### 3.4 建议的分层策略

1. **拦截点在中间件的留在中间件**，但每条都压成"最小、通用、无产品
   策略"的改动（当前正是这个形态）：删有害调用、补丢失赋值、修越界、
   事件如实上报。这类改动本质是**SDK 缺陷修复**，最适合回馈官方主线，
   一旦合入就不再有 fork 维护成本。
2. **产品策略全部上移应用**（已经完成）：`bt_get_acpt_link_role` 返回
   什么角色、`bt_should_accept_incoming_conn` 何时拒收转外连、HFP defer
   时长、A1 看门狗时长、重试节奏——都在 `app_bt.c`。两个 `__WEAK` 钩子
   是中间件仅有的"策略点"，默认行为与原版一致，风险极小。
3. **可以立即移植的三条**（#8 #9 #11）若希望子模块 diff 更小，可移入
   `app_a2dp_share.c` / `app_bt.c`（调用点均为公开 API，顺序可控）；
   但 #11 保留双写更稳，#8/#9 移不移对维护影响都不大。
4. **维护形态**：子模块 fork 的全部差异集中在 9 个文件约 400 行、按
   bt_cm / av / av_src / hfp_ag / generic 五处主题集中，SDK 升级时 rebase
   冲突面小；配合"先删有害行为、再补丢失行为、最后加通用钩子"的分层，
   长期可收敛到只剩两个钩子 + 少数缺陷修复。

---

*文档对应版本：middleware/bluetooth `33f31a8`（基线 `437d7da`）+ 未提交的 1.5 电流音修复 + 1.6 HFP 误回滚修复 + 1.7 扫描列表 20 条/汇总丢失修复 + 1.8 媒体泵迁出 bts2 定时器。*
