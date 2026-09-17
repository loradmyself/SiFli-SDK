# SiFli 充电器寄存器参考手册

本文档描述 SiFli SF32LB52X/SF32LB57X 系列芯片的充电器寄存器和充电状态机。

## 一、充电状态机 (PMUC_CHG_SR.CHG_STATE)

| 状态值 | 名称 | 说明 | CHG_SR 位[8:14] |
|--------|------|------|-----------------|
| `0x01` | OFF | 关闭 | `0000001` |
| `0x02` | PWRUP | 上电 | `0000010` |
| `0x04` | IDLE | 空闲 | `0000100` |
| `0x08` | **PRECC** | **预充电** | `0001000` |
| `0x10` | CC | 恒流充电 | `0010000` |
| `0x20` | CV | 恒压充电 | `0100000` |
| `0x40` | EOC | 充电结束 | `1000000` |

## 二、充电状态详细说明

### 2.1 OFF (关闭) - CHG_STATE=0x01

- **状态**: 充电器关闭
- **CHG_SR 位**: CC_MODE=0, CV_MODE=0, EOC_MODE=0, VBUS_RDY=0
- **充电电流**: 0
- **说明**: 未充电或充电器未使能

### 2.2 PWRUP (上电) - CHG_STATE=0x02

- **状态**: 充电器上电初始化
- **CHG_SR 位**: CC_MODE=0, CV_MODE=0, EOC_MODE=0, VBUS_RDY=1
- **充电电流**: 0
- **说明**: USB已插入，充电器正在初始化

### 2.3 IDLE (空闲) - CHG_STATE=0x04

- **状态**: 充电器空闲等待
- **CHG_SR 位**: CC_MODE=0, CV_MODE=0, EOC_MODE=0, VBUS_RDY=1
- **充电电流**: 0
- **说明**: 充电器就绪，等待开始充电

### 2.4 PRECC (预充电) - CHG_STATE=0x08

- **触发条件**: 电池电压 < 3.0V
- **退出条件**: 电池电压 ≥ 3.0V
- **CHG_SR 位**: CC_MODE=0, CV_MODE=0, EOC_MODE=0, VBUS_RDY=1
- **充电电流**: 0.5mA ~ 56mA (由 PRECC_ICTRL 配置，默认0.5mA)
- **目的**: 保护深度放电的电池，以小电流缓慢充电
- **对应电量**: 0-2%

### 2.5 CC (恒流充电) - CHG_STATE=0x10

- **触发条件**: 电池电压 ≥ 3.0V 且 < 满充电压 (4.2V)
- **退出条件**: 电池电压接近满充电压 (4.2V)
- **CHG_SR 位**: CC_MODE=1, CV_MODE=0, EOC_MODE=0, VBUS_RDY=1
- **充电电流**: 5mA ~ 560mA (由 CC_ICTRL 配置，默认65mA)
- **特点**: 快速充电阶段，电流恒定
- **对应电量**: 2-95%
- **寄存器配置**:
  - CC_ICTRL: 充电电流控制 (CHG_CR1[7:2])
  - CC_VCTRL: 电压控制 (CHG_CR1[13:8])

### 2.6 CV (恒压充电) - CHG_STATE=0x20

- **触发条件**: 电池电压接近满充电压 (4.2V)
- **退出条件**: 充电电流 < CC电流 × EOC百分比
- **CHG_SR 位**: CC_MODE=0, CV_MODE=1, EOC_MODE=0, VBUS_RDY=1
- **充电电压**: 恒定 4.2V (由 CV_VCTRL 配置)
- **充电电流**: 逐渐减小
- **特点**: 充电速度变慢，电压恒定
- **对应电量**: 95-99%
- **寄存器配置**:
  - CV_VCTRL: 满充电压控制 (CHG_CR1[31:26])，默认0x27=4200mV
  - REP_VCTRL: REP电压控制 (CHG_CR2[17:12])，默认0x20=4065mV

### 2.7 EOC (充电结束) - CHG_STATE=0x40

- **触发条件**: 充电电流 < CC电流 × EOC百分比 (默认10%)
- **CHG_SR 位**: CC_MODE=0, CV_MODE=0, EOC_MODE=1, VBUS_RDY=1
- **充电电流**: 接近 0
- **特点**: 充电完成，电池基本充满
- **对应电量**: 99-100%
- **寄存器配置**:
  - BM_EOC: EOC电流控制 (CHG_CR2[26:24])
  - RANGE_EOC: EOC范围选择 (CHG_CR2[27])

#### EOC 配置说明

EOC 百分比计算公式：
```
EOC百分比 = start + BM_EOC × step
```

其中：
- 当 RANGE_EOC = 0 时：start=4%, step=2%, 范围 4%~18%
- 当 RANGE_EOC = 1 时：start=8%, step=4%, 范围 8%~36%

## 三、寄存器定义

### 3.1 CHG_SR - 充电状态寄存器

| Bit | 名称 | 说明 | 读/写 |
|-----|------|------|-------|
| [0] | VBUS_RDY_OUT | USB电源就绪 (1=已插入) | R |
| [1] | VBAT_HIGH_OUT | 电池电压过高 (1=超压) | R |
| [2] | VBAT_ABOVE_REP_OUT | 电池电压高于REP电压 | R |
| [3] | VBAT_ABOVE_CC_OUT | 电池电压高于CC电压 | R |
| [4] | CC_MODE | 恒流充电模式标志 | R |
| [5] | CV_MODE | 恒压充电模式标志 | R |
| [6] | EOC_MODE | 充电结束模式标志 | R |
| [8:14] | CHG_STATE | 充电状态机 | R |

### 3.2 CHG_CR1 - 充电控制寄存器1

| Bit | 名称 | 说明 | 读/写 | 默认值 |
|-----|------|------|-------|--------|
| [0] | EN | 充电使能 | R/W | 0 |
| [1] | LOOP_EN | 充电环路使能 | R/W | 0 |
| [5:0] | CC_ICTRL | CC电流控制 | R/W | 见电流表 |
| [13:8] | CC_VCTRL | CC电压控制 | R/W | - |
| [18:14] | CC_MP | 校准MP | R/W | 0x14 |
| [23:19] | CC_MN | 校准MN | R/W | 0x08 |
| [25:24] | CC_RANGE | CC电流范围 | R/W | - |
| [31:26] | CV_VCTRL | CV电压控制 | R/W | 0x27 |

### 3.3 CHG_CR2 - 充电控制寄存器2

| Bit | 名称 | 说明 | 读/写 | 默认值 |
|-----|------|------|-------|--------|
| [3:0] | BG_PROG_V1P2 | 带隙基准电压 | R/W | 0x07 |
| [5:4] | PRECC_RANGE | 预充电范围 | R/W | - |
| [11:6] | PRECC_ICTRL | 预充电电流控制 | R/W | 0x00 |
| [17:12] | REP_VCTRL | REP电压控制 | R/W | 0x20 |
| [23:18] | HIGH_VCTRL | 过压保护电压控制 | R/W | - |
| [26:24] | BM_EOC | EOC电流控制 | R/W | - |
| [27] | RANGE_EOC | EOC范围选择 | R/W | - |
| [31:28] | VBAT_RANGE | 电池电压范围 | R/W | - |

### 3.4 CHG_CR3 - 充电控制寄存器3

| Bit | 名称 | 说明 | 读/写 | 默认值 |
|-----|------|------|-------|--------|
| [5:0] | DLY1 | 延时1 | R/W | - |
| [10:6] | DLY2 | 延时2 | R/W | - |
| [30] | FORCE_RST | 强制复位 | R/W | - |
| [31] | FORCE_CTRL | 强制控制 | R/W | - |

### 3.5 CHG_CR4 - 充电中断使能寄存器

| Bit | 名称 | 说明 | 读/写 |
|-----|------|------|-------|
| [0] | IE_VBUS_RDY | VBUS就绪中断使能 | R/W |
| [1] | IE_VBAT_HIGH | 电池高压中断使能 | R/W |
| [2] | IE_ABOVE_REP | 高于REP电压中断使能 | R/W |
| [3] | IE_ABOVE_CC | 高于CC电压中断使能 | R/W |
| [4] | IE_CC_MODE | CC模式中断使能 | R/W |
| [5] | IE_CV_MODE | CV模式中断使能 | R/W |
| [6] | IE_EOC_MODE | EOC模式中断使能 | R/W |
| [7] | IE_EOC | EOC中断使能 | R/W |
| [13:11] | IM_VBUS_RDY | VBUS就绪中断掩码 | R/W |
| [16:14] | IM_VBAT_HIGH | 电池高压中断掩码 | R/W |
| [19:17] | IM_ABOVE_REP | 高于REP电压中断掩码 | R/W |
| [22:20] | IM_ABOVE_CC | 高于CC电压中断掩码 | R/W |
| [25:23] | IM_CC_MODE | CC模式中断掩码 | R/W |
| [28:26] | IM_CV_MODE | CV模式中断掩码 | R/W |
| [31:29] | IM_EOC_MODE | EOC模式中断掩码 | R/W |

### 3.6 CHG_CR5 - 充电中断控制寄存器

| Bit | 名称 | 说明 | 读/写 |
|-----|------|------|-------|
| [0] | IC_VBUS_RDY | VBUS就绪中断清除 | R/W |
| [1] | IC_VBAT_HIGH | 电池高压中断清除 | R/W |
| [2] | IC_ABOVE_REP | 高于REP电压中断清除 | R/W |
| [3] | IC_ABOVE_CC | 高于CC电压中断清除 | R/W |
| [4] | IC_CC_MODE | CC模式中断清除 | R/W |
| [5] | IC_CV_MODE | CV模式中断清除 | R/W |
| [6] | IC_EOC_MODE | EOC模式中断清除 | R/W |
| [7] | IC_EOC | EOC中断清除 | R/W |
| [16] | IS_VBUS_RDY | VBUS就绪中断状态 | R |
| [17] | IS_VBAT_HIGH | 电池高压中断状态 | R |
| [18] | IS_ABOVE_REP | 高于REP电压中断状态 | R |
| [19] | IS_ABOVE_CC | 高于CC电压中断状态 | R |
| [20] | IS_CC_MODE | CC模式中断状态 | R |
| [21] | IS_CV_MODE | CV模式中断状态 | R |
| [22] | IS_EOC_MODE | EOC模式中断状态 | R |
| [23] | IS_EOC | EOC中断状态 | R |

## 四、电流配置表

### 4.1 CC电流 (CHG_CR1.CC_ICTRL)

| CC_ICTRL | 电流(mA) | CC_ICTRL | 电流(mA) | CC_ICTRL | 电流(mA) |
|----------|---------|----------|---------|----------|---------|
| 0x00 | 5 | 0x10 | 90 | 0x20 | 260 |
| 0x01 | 10 | 0x11 | 100 | 0x21 | 280 |
| 0x02 | 15 | 0x12 | 110 | 0x22 | 300 |
| 0x03 | 20 | 0x13 | 120 | 0x23 | 320 |
| 0x04 | 25 | 0x14 | 130 | 0x24 | 340 |
| 0x05 | 30 | 0x15 | 140 | 0x25 | 360 |
| 0x06 | 35 | 0x16 | 150 | 0x26 | 380 |
| 0x07 | 40 | 0x17 | 160 | 0x27 | 400 |
| 0x08 | 45 | 0x18 | 170 | 0x28 | 420 |
| 0x09 | 50 | 0x19 | 180 | 0x29 | 440 |
| 0x0A | 55 | 0x1A | 190 | 0x2A | 460 |
| 0x0B | 60 | 0x1B | 200 | 0x2B | 480 |
| **0x0C** | **65** | 0x1C | 210 | 0x2C | 500 |
| 0x0D | 70 | 0x1D | 220 | 0x2D | 520 |
| 0x0E | 75 | 0x1E | 230 | 0x2E | 540 |
| 0x0F | 80 | 0x1F | 240 | 0x2F | 560 |

> **默认值**: 0x0C = 65mA (`CHARGE_CC_CURRENT_LEVEL0`)

### 4.2 PRECC电流 (CHG_CR2.PRECC_ICTRL)

| PRECC_ICTRL | 电流(mA) | PRECC_ICTRL | 电流(mA) | PRECC_ICTRL | 电流(mA) |
|-------------|---------|-------------|---------|-------------|---------|
| 0x00 | **0.5** | 0x10 | 9.0 | 0x20 | 26 |
| 0x01 | 1.0 | 0x11 | 10 | 0x21 | 28 |
| 0x02 | 1.5 | 0x12 | 11 | 0x22 | 30 |
| 0x03 | 2.0 | 0x13 | 12 | 0x23 | 32 |
| 0x04 | 2.5 | 0x14 | 13 | 0x24 | 34 |
| 0x05 | 3.0 | 0x15 | 14 | 0x25 | 36 |
| 0x06 | 3.5 | 0x16 | 15 | 0x26 | 38 |
| 0x07 | 4.0 | 0x17 | 16 | 0x27 | 40 |
| 0x08 | 4.5 | 0x18 | 17 | 0x28 | 42 |
| 0x09 | 5.0 | 0x19 | 18 | 0x29 | 44 |
| 0x0A | 5.5 | 0x1A | 19 | 0x2A | 46 |
| 0x0B | 6.0 | 0x1B | 20 | 0x2B | 48 |
| 0x0C | 6.5 | 0x1C | 21 | 0x2C | 50 |
| 0x0D | 7.0 | 0x1D | 22 | 0x2D | 52 |
| 0x0E | 7.5 | 0x1E | 23 | 0x2E | 54 |
| 0x0F | 8.0 | 0x1F | 24 | 0x2F | **56** |

> **默认值**: 0x00 = 0.5mA

## 五、电压配置

### 5.1 电压配置表

| 参数 | 默认值 | 寄存器 | 说明 |
|------|--------|--------|------|
| 满充电压 | 4200mV | CV_VCTRL | CHG_CR1[31:26] |
| REP电压 | 4065mV | REP_VCTRL | CHG_CR2[17:12] |
| 过压保护 | 4800mV | HIGH_VCTRL | CHG_CR2[23:18] |

### 5.2 电压计算公式

```
实际电压 = 4200mV + (寄存器值 - 校准值) × 步进值
```

其中步进值默认为 20mV。

## 六、各状态下的典型寄存器值

| 状态 | CHG_SR | CC_MODE | CV_MODE | EOC_MODE | VBUS_RDY | 电流 |
|------|--------|---------|---------|----------|----------|------|
| OFF | 0x01 | 0 | 0 | 0 | 0 | 0 |
| PWRUP | 0x02 | 0 | 0 | 0 | 1 | 0 |
| IDLE | 0x04 | 0 | 0 | 0 | 1 | 0 |
| PRECC | 0x08 | 0 | 0 | 0 | 1 | 0.5~56mA |
| CC | 0x10 | 1 | 0 | 0 | 1 | 5~560mA |
| CV | 0x20 | 0 | 1 | 0 | 1 | 逐渐减小 |
| EOC | 0x40 | 0 | 0 | 1 | 1 | <10% CC |

## 七、锂电池电压-电量对照表

| 电压 (V) | 电量百分比 | 充电模式 | 说明 |
|---------|-----------|---------|------|
| 4.20V | 100% | CV → EOC | 满电 |
| 4.10V | 90% | CC | |
| 4.00V | 80% | CC | |
| 3.90V | 60% | CC | |
| 3.80V | 40% | CC | |
| 3.70V | 20% | CC | 标称电压 |
| 3.60V | 10% | CC | 低电量警告 |
| 3.40V | 5% | CC | |
| **3.00V** | **0-2%** | **PRECC → CC** | **预充电阈值** |
| 2.80V | 0% | PRECC | 过放电保护 |

## 八、Finsh 命令

### 8.1 读取充电状态

```
charger hw_state
```

返回值说明：
- `0x01` - OFF
- `0x02` - PWRUP
- `0x04` - IDLE
- `0x08` - PRECC (预充电)
- `0x10` - CC (恒流充电)
- `0x20` - CV (恒压充电)
- `0x40` - EOC (充电结束)

### 8.2 配置预充电电流

```
charger precc_i <current_ma>
```

示例：设置预充电电流为 10mA
```
charger precc_i 10
```

### 8.3 配置CC电流

```
charger cc_i <current_ma>
```

示例：设置CC电流为 100mA
```
charger cc_i 100
```

### 8.4 配置满充电压

```
charger tgt_volt <voltage_mv>
```

示例：设置满充电压为 4200mV
```
charger tgt_volt 4200
```

### 8.5 配置REP电压

```
charger rep_volt <voltage_mv>
```

示例：设置REP电压为 4065mV
```
charger rep_volt 4065
```

### 8.6 配置过压保护电压

```
charger high_volt <voltage_mv>
```

示例：设置过压保护电压为 4800mV
```
charger high_volt 4800
```

### 8.7 使能/禁止充电

```
charger enable <0|1>
```

### 8.8 强制充电

```
charger force_enable <0|1>
```

### 8.9 查看校准参数

```
charger list_cal
```

## 九、API 函数

### 9.1 设置预充电电流

```c
#include "charge.h"

// 设置预充电电流为 10mA
rt_charge_set_precc_current(10.0f);
```

### 9.2 设置CC电流

```c
#include "charge.h"

// 设置CC电流为 100mA
rt_charge_set_cc_current(100);
```

### 9.3 设置满充电压

```c
#include "charge.h"

// 设置满充电压为 4200mV
rt_charge_set_target_volt(4200);
```

### 9.4 获取充电状态

```c
#include "charge.h"

rt_charge_hw_state_t hw_state;
rt_charge_get_hw_status(&hw_state);

if (hw_state & RT_CHARGE_HW_PRECC) {
    // 处于预充电模式
}
```

## 十、默认配置

### 10.1 代码中的默认值

```c
// charge.h 中的定义
#define CHARGE_DEFAULT_FULL_VOLTAGE (4200)     // 满充电压 4200mV
#define CHARGE_DEFAULT_REP_VOLTAGE  (4200 - 135) // REP电压 4065mV
#define CHARGE_DEFAULT_VBAT_HIGH_VOLTAGE (4800) // 过压保护 4800mV
#define CHARGE_CC_CURRENT_LEVEL0   (65)         // CC电流 65mA
#define CHARGE_CC_CURRENT_LEVEL1   (300)        // CC电流 300mA
#define CHARGE_DEFAULT_EOC_CC       (10)         // EOC电流比例 10%
```

### 10.2 sifli_charge_init() 中的配置

```c
void sifli_charge_init(void)
{
    // ...
    HAL_PMU_ChgConfigTargetVolt(&sifli_charge_handle, CHARGE_DEFAULT_FULL_VOLTAGE);  // 4200mV
    HAL_PMU_ChgConfigRepVolt(&sifli_charge_handle, CHARGE_DEFAULT_REP_VOLTAGE);      // 4065mV
    HAL_PMU_ChgConfigVbatHighVolt(&sifli_charge_handle, CHARGE_DEFAULT_VBAT_HIGH_VOLTAGE);  // 4800mV
    HAL_PMU_ChgConfigCcCurrent(&sifli_charge_handle, CHARGE_CC_CURRENT_LEVEL0);       // 65mA
    HAL_PMU_ChgConfigEocCc(&sifli_charge_handle, CHARGE_DEFAULT_EOC_CC);             // 10%
    // 注意：未设置预充电电流，使用芯片默认值 0.5mA
}
```

## 十一、BM_EOC 和 RANGE_EOC 详细对照表

### 11.1 BM_EOC 说明

BM_EOC 是 CHG_CR2[26:24] 的 3 位字段，用于设置 EOC (End of Charge) 的电流阈值。

**计算公式**：
```
EOC百分比 = start + BM_EOC × step
```

### 11.2 RANGE_EOC = 0 时 (范围 4%~18%)

| BM_EOC | 十进制 | EOC百分比 | 说明 |
|--------|--------|-----------|------|
| 0x0 | 0 | **4%** | 最严格，充电最满 |
| 0x1 | 1 | **6%** | 较严格 |
| 0x2 | 2 | **8%** | |
| **0x3** | **3** | **10%** | **默认值** |
| 0x4 | 4 | **12%** | |
| 0x5 | 5 | **14%** | |
| 0x6 | 6 | **16%** | |
| 0x7 | 7 | **18%** | 最宽松 |

### 11.3 RANGE_EOC = 1 时 (范围 8%~36%)

| BM_EOC | 十进制 | EOC百分比 | 说明 |
|--------|--------|-----------|------|
| 0x0 | 0 | **8%** | 最严格 |
| 0x1 | 1 | **12%** | |
| 0x2 | 2 | **16%** | |
| **0x3** | **3** | **20%** | |
| 0x4 | 4 | **24%** | |
| 0x5 | 5 | **28%** | |
| 0x6 | 6 | **32%** | |
| 0x7 | 7 | **36%** | 最宽松 |

### 11.4 BM_EOC 寄存器定义

从 `pmuc.h`:
```c
#define PMUC_CHG_CR2_BM_EOC_Pos         (24U)
#define PMUC_CHG_CR2_BM_EOC_Msk         (0x7UL << PMUC_CHG_CR2_BM_EOC_Pos)
#define PMUC_CHG_CR2_BM_EOC             PMUC_CHG_CR2_BM_EOC_Msk
```

### 11.5 RANGE_EOC 寄存器定义

从 `pmuc.h`:
```c
#define PMUC_CHG_CR2_RANGE_EOC_Pos      (27U)
#define PMUC_CHG_CR2_RANGE_EOC_Msk      (0x1UL << PMUC_CHG_CR2_RANGE_EOC_Pos)
#define PMUC_CHG_CR2_RANGE_EOC          PMUC_CHG_CR2_RANGE_EOC_Msk
```

### 11.6 代码中的 EOC 配置

从 `bf0_hal_pmu.c`:
```c
uint8_t HAL_PMU_ChgConfigEocCc(PMU_ChgHandleTypeDef *handle, uint8_t percent)
{
    uint8_t step;
    uint8_t start;
    uint8_t range_eoc;
    uint8_t bm_eoc;

    // EOC 百分比范围: 4% ~ 36%
    if ((percent > 36) || (percent < 4))
    {
        percent = 0;
        goto __EXIT;
    }

    /*
     * range_eoc, 0: 0.5x, 1: 1x
     * bm_eoc, range from 4%~18% (if range_eoc=0, step=2%)
     *         range from 8%~36% (if range_eoc=1, step=4%)
     */
    if (percent >= 20)
    {
        step = 4;
        start = 8;
        range_eoc = 1;
    }
    else
    {
        step = 2;
        start = 4;
        range_eoc = 0;
    }

    bm_eoc = (percent - start) / step;

    MODIFY_REG(hwp_pmuc->CHG_CR2, PMUC_CHG_CR2_BM_EOC_Msk | PMUC_CHG_CR2_RANGE_EOC_Msk,
               MAKE_REG_VAL(bm_eoc, PMUC_CHG_CR2_BM_EOC_Msk, PMUC_CHG_CR2_BM_EOC_Pos)
               | MAKE_REG_VAL(range_eoc, PMUC_CHG_CR2_RANGE_EOC_Msk, PMUC_CHG_CR2_RANGE_EOC_Pos));

    percent = bm_eoc * step + start;

__EXIT:
    return percent;
}
```

### 11.7 BM_EOC 与 RANGE_EOC 组合对照表

| RANGE_EOC | BM_EOC | EOC百分比 | RANGE_EOC | BM_EOC | EOC百分比 |
|-----------|--------|-----------|-----------|--------|-----------|
| 0 | 0x0 | 4% | 1 | 0x0 | 8% |
| 0 | 0x1 | 6% | 1 | 0x1 | 12% |
| 0 | 0x2 | 8% | 1 | 0x2 | 16% |
| **0** | **0x3** | **10%** | **1** | **0x3** | **20%** |
| 0 | 0x4 | 12% | 1 | 0x4 | 24% |
| 0 | 0x5 | 14% | 1 | 0x5 | 28% |
| 0 | 0x6 | 16% | 1 | 0x6 | 32% |
| 0 | 0x7 | 18% | 1 | 0x7 | 36% |

### 11.8 解决 91% 就停止充电的问题

如果当前 `BM_EOC=0x3`，需要确认 `RANGE_EOC` 的值：

| 当前配置 | RANGE_EOC | BM_EOC | EOC百分比 | 建议修改 |
|----------|-----------|--------|-----------|----------|
| 情况1 | 0 | 0x3 | 10% | 改为 4% |
| 情况2 | 1 | 0x3 | 20% | 改为 4% |

**修改方法**：
```c
// 方法一：使用 API
HAL_PMU_ChgConfigEocCc(&sifli_charge_handle, 4);  // 设置 EOC 为 4%

// 方法二：直接写寄存器
uint32_t reg_val = hwp_pmuc->CHG_CR2;
reg_val &= ~PMUC_CHG_CR2_BM_EOC_Msk;  // 清除 BM_EOC
reg_val |= (0x00 << PMUC_CHG_CR2_BM_EOC_Pos);  // 设置为 0 (对应 4%)
hwp_pmuc->CHG_CR2 = reg_val;
```

## 十二、源代码参考

- 寄存器定义: `drivers/cmsis/sf32lb57x/pmuc.h`
- HAL实现: `drivers/hal/bf0_hal_pmu.c`
- HAL头文件: `drivers/Include/bf0_hal_pmu.h`
- 充电驱动: `customer/peripherals/charger/sifli_charge/sifli_charge.c`
- 充电头文件: `customer/peripherals/charger/charge.h`
