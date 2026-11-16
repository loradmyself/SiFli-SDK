# 硬件优化指南

## SF32LB52 硬件特性

### 1. 内存架构

| 区域 | 地址范围 | 大小 | 速度 | 用途 |
|---|---|---|---|---|
| DTCM | 0x20000000 - 0x2001FFFF | 128 KB | ⚡ 最快 (0 wait) | 关键数据 |
| RAM1 | 0x20020000 - 0x2003FFFF | 128 KB | 快 | 通用数据 |
| RAM2 | 0x20040000 - 0x2007FFFF | 256 KB | 快 | 通用数据 |
| PSRAM | 0x60000000+ | 可变 | 🐢 慢 | 大数据 |

**关键点**: DTCM 是最快的内存，应该用于性能关键的数据缓冲区。

### 2. 硬件加速器

#### ✅ AES 加速器 (0x5000d000)
- **支持算法**: AES-128, AES-192, AES-256
- **支持模式**: ECB, CTR, CBC
- **性能**: ~50-100 MB/s (硬件加速)
- **数据要求**: 必须 16 字节对齐，不能在 ITCM/Retention RAM

#### ❌ ChaCha20
- **不支持硬件加速**
- **纯软件实现**
- **性能**: ~7-12 MB/s (受限于 CPU)

### 3. CPU 特性

- **架构**: ARMv8-M (Cortex-M33)
- **主频**: 240 MHz
- **缓存**: I-Cache, D-Cache (可选)
- **扩展**: 可能支持 Helium (MVE) - 需要确认

---

## 优化策略

### 策略 1: 内存位置优化 (当前实施)

**目标**: 确保输入输出缓冲区在 DTCM 中

**方法**:
```c
// 静态分配 (自动放到 .bss 段)
static uint8_t input[131072];
static uint8_t output[131072];

// 检查地址
if (IS_IN_DTCM(input)) {
    rt_kprintf("Buffer in DTCM\n");
}
```

**预期提升**: 10-20%

**验证**:
```bash
chacha20_oberon_test 6  # 检查内存状态
```

### 策略 2: 链接脚本优化 (推荐)

**目标**: 强制缓冲区在 DTCM 中

**方法**: 修改链接脚本 `link.sct`:
```scatter
RW_DTCM HPSYS_RAM0_BASE HPSYS_RAM0_SIZE {
    *(.dtcm_data)
    *(.dtcm_bss)
}
```

**代码修改**:
```c
static uint8_t input[131072] __attribute__((section(".dtcm_data")));
static uint8_t output[131072] __attribute__((section(".dtcm_data")));
```

**预期提升**: 15-25%

### 策略 3: 算法替换 (最佳方案)

**目标**: 使用硬件加速的 AES 替代软件 ChaCha20

**方法**:
```c
// 使用 AES-256-CTR (硬件加速)
HAL_AES_init(key, 32, iv, AES_MODE_CTR);
HAL_AES_run(1, input, output, size);  // ~50-100 MB/s
```

**预期提升**: 500-1000% (7 MB/s → 50-100 MB/s)

**注意**:
- AES-CTR 和 ChaCha20 是不同的算法
- 需要评估安全性差异
- 某些应用场景可能强制要求 ChaCha20

### 策略 4: Helium/MVE 优化 (如果支持)

**目标**: 使用 SIMD 指令并行处理

**检查方法**:
```c
if (ARM_MVE_TYPE) {
    rt_kprintf("Helium: Supported\n");
}
```

**预期提升**: 200-400%

---

## 当前测试命令

### 检查硬件状态
```bash
# 选项 6: 检查缓存和内存状态
chacha20_oberon_test 6

# 选项 7: 查看硬件信息
chacha20_oberon_test 7
```

### 性能测试
```bash
# 选项 2: 吞吐量测试 (DTCM 优化)
chacha20_oberon_test 2

# 选项 5: MbedTLS vs Oberon 对比
chacha20_oberon_test 5
```

---

## 性能基准

### 当前状态
| 实现 | 吞吐量 | 内存位置 | 状态 |
|---|---|---|---|
| ChaCha20 (软件) | 7 MB/s | DTCM? | ⚠️ 瓶颈 |
| AES-CTR (硬件) | 50+ MB/s | 任意 | ✅ 推荐 |

### 优化目标
| 目标 | 方法 | 可行性 |
|---|---|---|
| 10 MB/s | DTCM + -O3 | ✅ 可行 |
| 12 MB/s | DTCM + 循环展开 | ⚠️ 困难 |
| 15 MB/s | Helium 或硬件加速 | ❌ 需硬件 |
| 50+ MB/s | AES-CTR | ✅ 推荐 |

---

## 推荐方案

### 方案 A: 继续优化 ChaCha20 (如果必须用 ChaCha20)

1. **确保 DTCM**: 使用链接脚本强制缓冲区在 DTCM
2. **-O3 编译**: `OPT = '-O3 -ffast-math'`
3. **循环展开**: 手动展开 4 次迭代
4. **预期**: 10-12 MB/s

### 方案 B: 切换到 AES-CTR (如果允许)

1. **使用硬件 AES**: `HAL_AES_run()`
2. **性能**: 50-100 MB/s
3. **注意**: 算法不同，需要评估安全性

### 方案 C: 混合方案

1. **小数据**: ChaCha20 (软件)
2. **大数据**: AES-CTR (硬件)
3. **根据数据大小自动选择**

---

## 故障排除

### 问题 1: 缓冲区不在 DTCM

**症状**: 测试显示 "Buffer NOT in DTCM"

**解决方案**:
1. 检查链接脚本是否包含 `.dtcm_data` 段
2. 使用 `__attribute__((section(".dtcm_data")))`
3. 或者使用静态分配 (通常在 .bss 段)

### 问题 2: 性能没有提升

**可能原因**:
1. D-Cache 未正确配置
2. 缓冲区虽然在 DTCM，但 D-Cache 未启用
3. 其他系统开销

**验证方法**:
```bash
chacha20_oberon_test 6  # 检查缓存状态
chacha20_oberon_test 7  # 查看硬件信息
```

### 问题 3: 想要使用硬件 AES

**步骤**:
```c
#include "bf0_hal_aes.h"

// 初始化
uint32_t key[8] = {...};
uint32_t iv[4] = {...};
HAL_AES_init(key, 32, iv, AES_MODE_CTR);

// 加密
HAL_AES_run(1, input, output, size);
```

---

## 下一步行动

### 立即行动
1. ✅ 运行 `chacha20_oberon_test 7` 查看硬件信息
2. ✅ 运行 `chacha20_oberon_test 2` 测试 DTCM 优化
3. ⏳ 检查缓冲区是否在 DTCM 中

### 短期行动
1. 修改链接脚本，强制缓冲区在 DTCM
2. 测试 -O3 编译优化
3. 评估是否可以使用 AES-CTR

### 长期行动
1. 评估 Helium/MVE 支持
2. 实现混合加密方案
3. 考虑 DMA 加速

---

## 参考资料

- [SF32LB52 数据手册](https://github.com/OpenSiFli/SiFli-SDK)
- [ARM Cortex-M33 技术参考手册](https://developer.arm.com/documentation/100166/0001)
- [AES HAL 驱动](drivers/Include/bf0_hal_aes.h)
- [内存映射](drivers/cmsis/sf32lb52x/mem_map.h)

---

**文档版本**: 1.0
**创建日期**: 2026-08-12
