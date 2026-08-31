# 双蓝牙耳机对讲 Demo 交付说明

本 Demo 面向 SF32LB52B Nano N4 开发板。

## 功能概览

| **功能** | **当前实现** |
| --- | --- |
| 固件 | 两块 Nano N4 使用同一套固件 |
| 角色选择 | PA39 高电平为音源设备；低电平或悬空为测试主设备 |
| 测试音源 | 内置 /test1.mp3，44.1 kHz、双声道，手动开始、循环播放 |
| I2S | 音源设备为 TX master；测试/中继设备为 RX slave |
| 音乐播放 | PCM 编码一次后，调用 SDK sharing 逻辑向两副 A2DP Sink 耳机发送同一组 SBC 包 |
| 对讲 | 两路 HFP/SCO 之间进行全双工语音转发 |
| 设备管理 | 支持扫描、按扫描序号或 MAC 连接、按耳机槽位或 MAC 断开、状态查询 |
| 音量 | 音乐音量和对讲音量分别设置 |

## 运行角色

| **PA39 上电电平** | **运行角色** | **PA31/PA32** | **主要命令** |
| --- | --- | --- | --- |
| 接 3.3 V，高电平 | 音源设备 | LED1、LED2 均熄灭 | status、play、stop |
| 接 GND，或悬空 | 测试/中继设备 | 两个耳机槽位未就绪时闪烁，就绪后对应 LED 常亮 | scan、connect、disconnect、talk、volume、status |

程序总是使用当前编号最小的空闲耳机槽位；两个槽位均为空时，前两次连接命令依次占用槽位 1 和槽位 2：

| **指示灯** | **引脚** | **闪烁** | **常亮** |
| --- | --- | --- | --- |
| LED1 | PA31 | 耳机槽位 1 尚未同时完成 A2DP 和 HFP 连接 | 耳机槽位 1 已完成 A2DP 和 HFP 连接 |
| LED2 | PA32 | 耳机槽位 2 尚未同时完成 A2DP 和 HFP 连接 | 耳机槽位 2 已完成 A2DP 和 HFP 连接 |

音源设备始终关闭两个 LED。

## 数据路径

音乐路径如下：

```text
内置 /test1.mp3
  -> 音源设备：SDK MP3 解码和 Audio Server
  -> 44.1 kHz、双声道、16 bit PCM
  -> I2S1 TX master
  -> I2S1 RX slave
  -> 测试/中继设备：音乐音量处理和 SBC 编码
  -> SDK bt_avsrc_sharing()
  -> 耳机 1 / 耳机 2：A2DP Sink
```

测试/中继设备等待所有已连接的A2DP流进入streaming，并确认两路SBC配置一致。随后预存约200 ms的完整SBC媒体包，再启动SDK sharing发送。

连接第二副耳机前会先停止已有音乐流；新耳机完成 A2DP 和 HFP 连接后，两路音乐共同重启。因此第二副耳机接入时出现一次短暂停顿属于预期行为。对讲路径如下：

```text
耳机 1：HFP HF / SCO  <->  SDK voice relay  <->  HFP HF / SCO：耳机 2
```

进入对讲时，两路 A2DP 先停止，再分别建立 SCO。对讲期间 I2S 外设继续接收，退出对讲后，程序关闭两路 SCO，从当时正在到达的 I2S 数据重新编码并预存约 200 ms，然后恢复 A2DP。

## 硬件准备

1. 两块 SF32LB52B Nano N4 开发板，不使用 PSRAM。
1. 两副可作为独立经典蓝牙设备使用的耳机。每副耳机必须同时支持 A2DP Sink和 HFP HF。
1. 两条 USB 线，分别用于供电、下载和串口命令。
1. 三根 I2S 信号线、一根 GND 线，以及 PA39 角色选择跳线。
同一套 TWS 耳机的左右耳如果只对外呈现一个蓝牙设备，就不能占用两个耳机槽位。

## I2S 接线

先断开两块开发板电源，再完成接线。两块板必须共地。

| **音源设备：输出** | **测试/中继设备：输入** | **用途** |
| --- | --- | --- |
| PA25 I2S1_SDO | PA28 I2S1_SDI | PCM 数据 |
| PA29 I2S1_BCK | PA29 I2S1_BCK | 位时钟 |
| PA30 I2S1_LRCK | PA30 I2S1_LRCK | 左右声道时钟 |
| GND | GND | 信号参考地 |

## 串口设置与启动日志

音源设备的关键启动日志：

```text
[intercom] PA39=high, role=source
[intercom] source ready; use 'intercom play'
[intercom] command shell ready; run 'intercom status'
```

测试/中继设备的关键启动日志：

```text
[intercom] PA39=low, role=relay
[intercom] dual-headset relay starting
[intercom] relay ready; use 'intercom scan'
[intercom] Bluetooth stack and profiles are ready
```

## 串口命令

不带参数执行 intercom 会显示当前角色可用的命令。

### 音源设备

| **命令** | **行为** |
| --- | --- |
| `intercom status` | 显示音源状态、文件系统状态、歌曲和 I2S TX 引脚 |
| `intercom play` | 从头开始播放内置 /test1.mp3，歌曲结束后自动循环 |
| `intercom stop` | 停止内置歌曲；再次执行 play 时从头开始 |

音源设备不会上电自动播放。执行 intercom play 后应出现以下日志；由于播放命令由独立线程异步执行，日志顺序可能交错：

```text
[intercom] play accepted
[intercom] state: idle -> source-playing
[intercom] playing embedded song /test1.mp3 (looping)
```

play accepted 和 stop accepted 只表示命令已进入音源线程队列，最终结果以随后出现的状态和播放日志为准。

## 测试/中继设备

| **命令** | **行为** |
| --- | --- |
| `intercom status` | 显示运行状态、两个耳机槽位、I2S、SBC、sharing 缓冲和音量 |
| `intercom scan` | 扫描约 10 秒，发现设备时立即打印，结束时打印本轮汇总 |
| `intercom connect <扫描序号>` | 连接本轮扫描结果中的设备，例如 intercom connect 1 |
| `intercom connect <MAC>` | 按地址连接，例如 intercom connect 11:22:33:44:55:66 |
| `intercom disconnect <1\|2>` | 断开耳机槽位 1 或 2；（不是扫描序号） |
| `intercom disconnect <MAC>` | 按地址断开已分配的耳机 |
| `intercom talk start` | 停止两路 A2DP，建立两路 SCO 并开始双向对讲 |
| `intercom talk stop` | 关闭两路 SCO，并从当前 I2S 数据恢复音乐 |
| `intercom volume` | 查看全局音量、两副耳机各自的设置及最后音量上报 |
| `intercom volume music <耳机序号> <0-15>` | 通过 AVRCP 调节指定耳机的音乐音量 |
| `intercom volume talk <耳机序号> <0-15>` | 设置指定耳机的对讲接收音量 |

每次扫描都会清空上一轮扫描序号。

## 常用命令及行为

例如分别设置两副耳机：

```console
intercom volume music 1 5
intercom volume music 2 8

intercom volume talk 1 4
intercom volume talk 2 7
```

耳机序号是 intercom status 中的 headset1/headset2，不是扫描结果序号。音乐的分耳机调节要求对应 AVRCP 已连接；对讲分耳机调节通过对应 HFP 通道发送 VGS。

对讲未开始时设置的音量会先保存，建立 SCO 后应用；对讲期间设置则立即发送。这里只控制耳机听到的对讲音量，不控制麦克风发送增益。

**断连命令**

```console
intercom disconnect <1|2>
intercom disconnect <MAC>
```

例如：

```console
intercom disconnect 2
intercom disconnect E2:56:30:0A:80:AC
```

其中 1|2 是耳机槽位编号。

**音源模式命令**

PA39 拉高进入音源模式后：

```console
intercom play
intercom stop
intercom status
```

play 循环播放内置 test1.mp3 并通过 I2S 输出，直到执行 stop。

**连接命令**

```console
intercom scan
intercom connect <扫描序号>
intercom connect <MAC>
```

例如：

```console
intercom scan
intercom connect 1
intercom connect E2:56:30:0A:80:AC
```


## 提交时的SDK信息：
SDK branch: main
SDK commit: 4407380ca5207436cdc60d747e7a96b4609e297b
SDK version: 2.5.0
Build target: sf32lb52-nano_n4_hcpu
