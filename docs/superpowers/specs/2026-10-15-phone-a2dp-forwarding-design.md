# 手机 A2DP 音频转发移植设计

日期：2026-10-15  
目标工程：`example/Dual_Bluetooth_Headset_Walkie-Talkie`  
目标板卡：`spi-hdk_lb57bun3n6_hcpu`

## 1. 目标

在保留现有两副蓝牙耳机 HFP/SCO 对讲功能的前提下，将音乐输入链路由外接 I2S PCM 改为手机 A2DP 音频：

```text
手机 A2DP Source
  -> 本机 A2DP Sink
  -> BT_NOTIFY_A2DP_DATA_IND
  -> SBC packet ring
  -> bt_avsrc_sharing()
  -> 耳机 1 / 耳机 2 A2DP Sink
```

对讲链路保持不变：

```text
耳机 1 HFP/SCO <-> 现有 voice relay <-> 耳机 2 HFP/SCO
```

## 2. 范围

### 2.1 保留

- 两副耳机的扫描、连接、断开和自动重连。
- 两副耳机的 HFP AG 连接。
- 两路 SCO 的建立、关闭和双向语音转发。
- `intercom talk start` / `intercom talk stop`。
- 对讲期间暂停音乐，对讲结束后恢复音乐。
- 耳机槽位、AVRCP 音量和现有异常恢复逻辑。
- A2DP Source 到两个耳机的共享发送。

### 2.2 移除

- I2S RX 初始化和采集。
- PCM 环形缓冲。
- PCM 音量斜坡和 PCM 暂存缓冲。
- 应用层 SBC 编码线程和编码队列。
- I2S 相关状态、统计和串口状态输出。

### 2.3 新增

- 单个手机连接上下文。
- 手机侧 A2DP Sink。
- 手机侧 AVRCP Controller。
- 耳机侧 AVRCP Target。
- 手机 A2DP SBC 包接收、缓存和共享发送。
- 手机绝对音量到两个耳机的同步。
- 耳机播放/暂停、上一曲、下一曲到手机的转发。
- `intercom` 命令中的手机状态显示和断开能力。

### 2.4 不在本次范围

- 手机 HFP 通话转发到耳机。
- 手机通话状态、来电号码和通话 SCO 中继。
- AAC 编码转发；首版按两个 SBC A2DP 流实现。
- 运行时 I2S/手机双输入切换。

## 3. 连接模型

| 对端 | 本机 A2DP 角色 | 本机 AVRCP 角色 | HFP 角色 |
| --- | --- | --- | --- |
| 手机 | Sink | Controller | 不启用 |
| 耳机 1 | Source | Target | AG |
| 耳机 2 | Source | Target | AG |

手机连接与耳机槽位分离：

- 手机最多一台。
- 耳机槽位仍为两个。
- 手机不进入耳机自动重连队列。
- 手机断开后停止共享发送并清空转发 Ring。
- 耳机断开时只释放对应耳机槽位；另一副耳机继续接收可发送的数据。

## 4. 软件结构

### 4.1 `app_a2dp_share`

将现有 PCM/SBC 编码模块改造为手机媒体包共享模块，同时保留 `app_bt.c` 使用的主要接口：

- `app_a2dp_share_init`
- `app_a2dp_share_start`
- `app_a2dp_share_stop`
- `app_a2dp_share_on_a2dp_event`
- `app_a2dp_share_reconcile_streams`
- 状态和统计查询

新增手机媒体入口：

```c
void app_a2dp_share_on_media_data(const bt_notify_a2dp_data_t *media);
```

行为：

1. 仅接受已注册手机连接的 A2DP 数据。
2. 校验长度不超过 `AV_MTU_SIZE`。
3. 将 `uint16_t length + packet` 写入共享 Ring。
4. 调用方负责释放 SDK 提供的媒体数据缓冲。
5. Ring 满时丢弃最新包并计数，不能阻塞蓝牙任务。
6. 当两个耳机流允许发送且 Ring 达到阈值时调用 `bt_avsrc_sharing()`。
7. 手机 suspend/disconnect、对讲开始或共享停止时清空 Ring。
8. 保持现有媒体流 reconcile、超时和单链路 wedge 恢复能力；无法直接保留的 PCM 诊断项替换为手机包诊断项。

### 4.2 `app_bt`

- 新增独立手机连接状态。
- ACL 未匹配耳机槽位时允许一个链接进入手机候选流程。
- `AV_AUDIO_SNK` 的 A2DP 连接绑定到手机上下文。
- `AV_AUDIO_SRC` 的 A2DP 连接继续绑定耳机槽位。
- AVRCP 连接根据对应 A2DP 角色动态设置：
  - `AV_AUDIO_SNK`：`AVRCP_CT`
  - `AV_AUDIO_SRC`：`AVRCP_TG`
- `BT_NOTIFY_A2DP_DATA_IND` 只从手机链路接收，转发后立即 `bfree`。
- `intercom status` 增加手机 ACL/A2DP/AVRCP/媒体状态。
- 增加 `intercom phone disconnect`。
- 保持现有两个耳机 HFP/SCO 对讲状态机不变。

### 4.3 配置

保留：

```text
CONFIG_CFG_HFP_AG=y
CONFIG_CFG_BT_VOICE_RELAY=y
CONFIG_CFG_AV=y
CONFIG_CFG_AV_SRC=y
CONFIG_CFG_AV_SHARING=y
CONFIG_CFG_AVRCP=y
```

新增：

```text
CONFIG_CFG_AV_SNK=y
CONFIG_CFG_MAX_BT_ACL_NUM=3
CONFIG_CFG_MAX_BT_BOND_NUM=3
```

移除或关闭：

```text
CONFIG_BSP_USING_I2S
CONFIG_BSP_ENABLE_I2S_CODEC
CONFIG_AUDIO_TX_USING_I2S
CONFIG_CFG_AUDIO_RECORD_PIPE_MULT
CONFIG_HCI_ON_LOG
CONFIG_LOG_ON_CONSOLE
```

## 5. 对讲与音乐互斥

- `talk start`：停止手机共享发送，清空 Ring，挂起两个耳机 A2DP 流，再建立两路 SCO。
- 对讲期间手机可保持 A2DP 连接，但不能向耳机发送媒体。
- `talk stop`：关闭两路 SCO，清空旧数据，等待手机和耳机流状态重新满足后再启动共享发送。
- 新耳机加入对讲或音乐时，复用现有连接 generation 和超时机制，避免旧事件影响新连接。

## 6. 内存与尺寸预算

当前构建基线：

| 指标 | 当前值 |
| --- | ---: |
| `main.bin` | 914,508 bytes |
| 静态 BSS/Data | 约 128 KiB |
| `app_a2dp_share.o` text | 5,768 bytes |
| `app_i2s.o` text | 1,482 bytes |
| `app_a2dp_share.o` BSS | 39,757 bytes |
| `app_i2s.o` BSS | 31,431 bytes |

预计删除约 7.2 KiB text 和 71 KiB 静态 RAM。手机共享 Ring 使用约 20 KiB。

预期新增约 8～11 KiB text，最终 `main.bin` 预计 918～928 KiB，静态 BSS/Data 预计 78～85 KiB。

## 7. 验收条件

1. `spi-hdk_lb57bun3n6_hcpu` 构建成功。
2. 固件尺寸不超过可用镜像分区，并记录最终 `main.bin` 大小。
3. 手机连接后可与两个耳机同时建立 A2DP。
4. 手机播放音乐时两个耳机可同时听到转发音频。
5. 手机暂停、恢复和断开时两个耳机行为一致。
6. 任一耳机断开后，另一个耳机仍能正常接收。
7. 第二副耳机连接后，两副耳机都恢复播放。
8. 两副耳机执行 `intercom talk start` 后建立双向 SCO 对讲。
9. 对讲期间手机音乐不混入对讲。
10. `intercom talk stop` 后手机音乐转发恢复。
11. 音乐和对讲音量命令继续有效。
12. 不再初始化 I2S，不再读取 I2S PCM。
13. `intercom status` 能显示手机、两个耳机、对讲和共享 Ring 状态。
14. 构建报告包含 text/data/bss、`main.bin` 大小和关键内存符号大小。

## 8. 实施顺序

1. 修改 Kconfig/`proj.conf`，启用 A2DP Sink 和三 ACL，关闭 I2S/HCI 日志。
2. 将 `app_a2dp_share.c` 改为手机媒体包共享模块。
3. 在 `app_bt.c` 增加手机连接、媒体事件、AVRCP 动态角色和状态命令。
4. 移除 `app_i2s` 构建入口及其应用引用。
5. 更新 `README.md` 的连接、命令和数据路径。
6. 编译目标板卡并记录尺寸。
7. 根据编译结果缩减或调整缓冲区，不改变核心行为。
