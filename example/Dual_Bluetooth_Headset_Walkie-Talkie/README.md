# 双蓝牙耳机对讲与手机音乐转发 Demo

本 Demo 面向 `spi-hdk_lb57bun3n6_hcpu`，保留原有两副蓝牙耳机之间的 HFP/SCO 对讲功能，并将音乐输入源从外接 I2S PCM 改为手机 A2DP 音频。

## 功能概览

| 功能 | 实现 |
| --- | --- |
| 连接拓扑 | 1 个手机 + 2 副蓝牙耳机 |
| 手机音乐 | 手机 A2DP Source -> 本机 A2DP Sink -> 两副耳机 A2DP Sink |
| 音频格式 | 转发手机下发的 SBC A2DP 媒体包，不在应用层解码或重新编码 |
| 对讲 | 耳机 1 HFP/SCO <-> 本机 voice relay <-> 耳机 2 HFP/SCO |
| 音量 | 手机 AVRCP Absolute Volume 同步到两个耳机；对讲音量独立设置 |
| 媒体控制 | 耳机播放、暂停、上一曲、下一曲转发给手机 |
| I2S | 已移除，不再初始化或读取 I2S PCM |

首版不包含手机 HFP 通话转发。这里的“对讲”指两副耳机之间通过两路 SCO 进行双向语音转发。

## 连接角色

| 对端 | 本机 A2DP 角色 | 本机 AVRCP 角色 | HFP 角色 |
| --- | --- | --- | --- |
| 手机 | Sink | Controller | 不启用 |
| 耳机 1 | Source | Target | AG |
| 耳机 2 | Source | Target | AG |

手机由手机侧主动搜索并连接本机。两个耳机可以通过串口命令扫描并连接。

## 音乐转发路径

```text
手机 A2DP Source
  -> 本机 A2DP Sink
  -> BT_NOTIFY_A2DP_DATA_IND
  -> SBC packet ring
  -> bt_avsrc_sharing()
  -> 耳机 1 A2DP Sink
  -> 耳机 2 A2DP Sink
```

测试/中继设备等待所有已连接的A2DP流进入streaming，并确认两路SBC配置一致。随后预存约250 ms的完整SBC媒体包，再启动SDK sharing发送。

第二副耳机连接期间不暂停已有音乐流：媒体发送节奏运行在专用 TX 线程（"a2dptx"）上，不与建链信令共用 bts 任务的定时器派发，连接时音频应保持连续无卡顿。对讲路径如下：

```text
耳机 1 HFP/SCO
  <-> SDK HFP voice relay
  <-> 耳机 2 HFP/SCO
```

进入对讲时，两路 A2DP 先停止，再分别建立 SCO。对讲期间 I2S 外设继续接收，退出对讲后，程序关闭两路 SCO，从当时正在到达的 I2S 数据重新编码并预存约 250 ms，然后恢复 A2DP。

1. 停止手机音乐共享并清空媒体缓存。
2. 挂起两个耳机的 A2DP 流。
3. 建立两路 SCO。
4. 注册双向 voice relay。

执行 `intercom talk stop` 时：

1. 关闭两路 SCO。
2. 清空旧媒体数据。
3. 等待手机和耳机 A2DP 状态满足条件后恢复音乐转发。

## 串口命令

不带参数执行 `intercom` 会显示命令帮助。

| 命令 | 行为 |
| --- | --- |
| `intercom status` | 显示手机、两个耳机、AVRCP、对讲和手机媒体转发状态 |
| `intercom scan` | 扫描耳机类蓝牙设备 |
| `intercom connect <scan-index\|MAC>` | 连接扫描结果或指定耳机 |
| `intercom disconnect <1\|2\|MAC>` | 断开并忘记指定耳机 |
| `intercom phone disconnect` | 断开当前手机，不加入耳机自动重连队列 |
| `intercom unbond <MAC>` | 删除指定配对记录 |
| `intercom talk start` | 开始两耳机双向对讲 |
| `intercom talk stop` | 停止对讲并恢复音乐 |
| `intercom volume` | 查看音乐和对讲音量 |
| `intercom volume music <1\|2> <0-15>` | 设置指定耳机音乐音量 |
| `intercom volume talk <1\|2> <0-15>` | 设置指定耳机对讲接收音量 |

示例：

```console
intercom scan
intercom connect 1
intercom connect 2
intercom phone disconnect
intercom talk start
intercom talk stop
intercom volume music 1 8
intercom volume talk 2 6
```

## 预期启动日志

```text
[intercom] dual-headset relay starting
[intercom] Bluetooth stack and profiles are ready
[intercom] relay ready; use 'intercom scan'
[intercom] command shell ready; run 'intercom status'
```

手机连接后应看到：

```text
[intercom] phone ACL connected: XX:XX:XX:XX:XX:XX
[intercom] phone A2DP Sink connected
```

## 配置要点

保留：

```text
CONFIG_CFG_HFP_AG=y
CONFIG_CFG_BT_VOICE_RELAY=y
CONFIG_CFG_AV=y
CONFIG_CFG_AV_SRC=y
CONFIG_CFG_AV_SHARING=y
CONFIG_CFG_AVRCP=y
```

启用：

```text
CONFIG_CFG_AV_SNK=y
CONFIG_CFG_MAX_BT_ACL_NUM=3
CONFIG_CFG_MAX_BT_BOND_NUM=3
```

关闭：

```text
CONFIG_BSP_USING_I2S
CONFIG_BSP_ENABLE_I2S_CODEC
CONFIG_AUDIO_TX_USING_I2S
CONFIG_CFG_AUDIO_RECORD_PIPE_MULT
CONFIG_HCI_ON_LOG
CONFIG_LOG_ON_CONSOLE
```

## 构建

在 SDK 根目录初始化环境后，进入本 Demo 的 `project` 目录：

```powershell
& ..\..\..\export.ps1
scons --board=spi-hdk_lb57bun3n6_hcpu -j8
```

产物位于：

```text
project/build_spi-hdk_lb57bun3n6_hcpu/
```

## 测试重点

1. 手机与两个耳机同时建立 A2DP。
2. 手机播放音乐时两个耳机同时听到音频。
3. 手机暂停、恢复和断开时两个耳机行为一致。
4. 任一耳机断开后，另一副耳机仍能接收。
5. 第二副耳机加入后，两副耳机都恢复播放。
6. `intercom talk start` 建立两路 SCO 和双向语音转发。
7. 对讲期间没有手机音乐混入。
8. `intercom talk stop` 后手机音乐恢复。
9. `intercom status` 不显示 I2S 状态，显示手机媒体包和 Ring 状态。
