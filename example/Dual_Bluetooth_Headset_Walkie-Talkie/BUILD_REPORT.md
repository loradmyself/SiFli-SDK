# Phone A2DP Forwarding Build Report

目标板卡：`spi-hdk_lb57bun3n6_hcpu`  
构建时间：2026-10-15 11:22:35  
构建结果：成功

## 固件尺寸

| 指标 | 修改前 | 修改后 | 变化 |
| --- | ---: | ---: | ---: |
| `main.bin` | 914,508 B | 907,648 B | -6,860 B |
| ELF text | 未单独记录 | 900,944 B | - |
| ELF data | 未单独记录 | 6,700 B | - |
| ELF bss | 未单独记录 | 82,688 B | - |

`main.bin` 距离 1 MiB 上限剩余 140,928 B。当前 `ptab.json` 的 `HCPU_FLASH_CODE_SIZE` 为 8,388,608 B，实际镜像同样满足该分区限制。

## 关键模块尺寸

| 模块 | text | data | bss |
| --- | ---: | ---: | ---: |
| `app_a2dp_share.o` | 2,522 B | 1 B | 20,401 B |
| `app_bt.o` | 35,464 B | 0 B | 2,292 B |

## 内存符号

| 符号 | 大小 | 说明 |
| --- | ---: | --- |
| `g_share_ring_pool` | 20,160 B | 手机 A2DP 媒体包共享 Ring |
| `g_phone` | 73 B | 单手机连接上下文 |
| `g_slots` | 416 B | 两个耳机槽位 |

以下旧 I2S/PCM 静态符号已不存在：

- `g_rx_pool`
- `g_rx_dma_buffer`
- `g_i2s_block`
- `g_pcm_buffer`
- `g_producer_stack`
- `g_record_queue_pool`

## 配置确认

启用：

```text
CONFIG_CFG_HFP_AG=y
CONFIG_CFG_BT_VOICE_RELAY=y
CONFIG_CFG_AV_SRC=y
CONFIG_CFG_AV_SHARING=y
CONFIG_CFG_AV_SNK=y
CONFIG_CFG_MAX_BT_ACL_NUM=3
CONFIG_CFG_MAX_BT_BOND_NUM=3
```

生成配置中不存在：

```text
CONFIG_BSP_USING_I2S=y
CONFIG_BSP_ENABLE_I2S_CODEC=y
CONFIG_AUDIO_TX_USING_I2S=y
CONFIG_HCI_ON_LOG=y
CONFIG_LOG_ON_CONSOLE=y
```

## 源码扫描

以下应用源文件中不再引用 `app_i2s_`、`APP_I2S` 或 `AUDIO_TX_USING_I2S`：

- `src/main.c`
- `src/app_bt.c`
- `src/app_a2dp_share.c`
- `src/app_cli.c`

`src/SConscript` 明确排除 `app_i2s.c`，因此旧文件不参与编译。
