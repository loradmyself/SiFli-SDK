# Tasks：主动断连先 suspend AVDTP 再断 ACL

> 单变量改动，仅 `example/Dual_Bluetooth_Headset_Walkie-Talkie/src/app_bt.c`。

- [x] T1 — 确认头文件/符号可见性：`bts2_app_inc.h` 已包含 `bts2_app_av.h`，`bt_av_get_inst_data`/`bt_av_suspend_stream`/`bt_av_get_idx_from_addr`/`avconned_streaming`/`bts2s_av_inst_data`/`MAX_CONNS` 均可见；`bt_addr_convert_to_bts`/`bd_addr_t`/`BTS2S_BD_ADDR` 经 `bf0_sibles.h` 可见。无需新增 include。
- [x] T2 — `app_bt_slot_t` 新增 `bool disconnect_suspend_pending;`（L242）。
- [x] T3 — 新增 `app_bt_av_conn_idx()` 与 `app_bt_begin_suspend_then_disconnect()`（L1554-1602）。
- [x] T4 — `app_bt_handle_disconnect_command` 断连前插入 suspend-then-disconnect 判断（L2567-2572）。
- [x] T5 — `BT_NOTIFY_A2DP` 分发处处理 `BT_NOTIFY_A2DP_SUSPEND_CFM`，回复到达后断连（L4946-4974）。
- [x] T6 — `app_bt_handle_acl_disconnected` 清理 `disconnect_suspend_pending`（L2851）。
- [ ] T7 — 编译 + 烧录 + 回归验证（**需用户本地执行**：本环境缺少 arm-none-eabi-gcc 工具链，`export.ps1` 需 `uv` 联网下载依赖，无法在此完成编译/烧录/抓包）。

验证项（用户本地）：
1. 主动 `intercom disconnect` 断其中一副耳机，串口应依次打印 `suspend-then-disconnect pending` → `suspended, disconnecting ACL`。
2. 抓包确认新时序为 `AVDTP suspend` → `suspend cnf` → 之后才 `ACL closed`，不再出现 `ACL closed before A2DP L2CAP channel`。
3. 断开其中一副后，另一副仍正常 streaming（不回弹 `invalidate=1`）。
4. 对端拒不响应 suspend 时，3s 后 watchdog 强断，不挂死。