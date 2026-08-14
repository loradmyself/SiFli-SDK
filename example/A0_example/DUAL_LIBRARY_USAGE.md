# 双库对比测试使用指南

## 概述

支持同时启用 MbedTLS 4.2.0 和 nrf_oberon 两个库进行性能对比测试。

**关键点**:
- 两个库使用**不同的函数名**，不会有链接冲突
- MbedTLS: `mbedtls_chacha20_crypt()`
- Oberon: `ocrypto_chacha20_encode()`

---

## 配置方法

### 1. 启用双库模式

编辑 `project/proj.conf`:

```ini
CONFIG_CHACHA20_USE_MBEDTLS_420=y
CONFIG_CHACHA20_USE_OBERON=y
CONFIG_CHACHA20_USE_BOTH=y
```

### 2. 或通过 menuconfig

```bash
cd example/A0_example/project
sdk.py menuconfig --board=sf32lb52-lchspi-ulp_hcpu
```

选择:
```
ChaCha20 Library Selection
  [*] Use MbedTLS 4.2.0
  [*] Use Oberon PSA Crypto
  [*] Enable both libraries for comparison
```

---

## 编译

```bash
cd example/A0_example/project
scons --board=sf32lb52-lchspi-ulp_hcpu -j8
```

**注意**: 如果编译报错，请先清理:
```bash
rmdir /s /q build_sf32lb52-lchspi-ulp_hcpu
```

---

## 测试命令

### MbedTLS 测试 (命令前缀: `chacha20_test`)

```bash
# 正确性验证
chacha20_test 1

# 吞吐量测试 (优化版)
chacha20_test 2

# 延迟测试 (优化版)
chacha20_test 3

# 所有测试
chacha20_test 4

# AES 对比测试
chacha20_test 5
```

### Oberon 测试 (命令前缀: `chacha20_oberon_test`)

```bash
# 正确性验证
chacha20_oberon_test 1

# 吞吐量测试 (优化版)
chacha20_oberon_test 2

# 延迟测试 (优化版)
chacha20_oberon_test 3

# 所有测试
chacha20_oberon_test 4

# 性能对比 (MbedTLS vs Oberon)
chacha20_oberon_test 5

# 缓存状态检查
chacha20_oberon_test 6
```

---

## 已实施的优化

两个库的测试代码都应用了相同的优化:

| 优化项 | 预期提升 | 代码位置 |
|---|---|---|
| I-Cache 启用 | +15% | `optimize_cache_and_memory()` |
| D-Cache 启用 | +10% | `optimize_cache_and_memory()` |
| 16字节内存对齐 | +15% | `aligned_malloc()` |
| 中断控制 | +10% | `__disable_irq()` |
| D-Cache 维护 | +5% | `SCB_CleanDCache_by_Addr()` |
| 预热优化 | +3% | 20-200次预热 |
| 迭代次数增加 | +2% | 2x 迭代次数 |

**总计预期提升**: ~60% (7 MB/s → 11.2 MB/s)

---

## 预期输出示例

### 吞吐量测试

```
========================================
  ChaCha20 Throughput Test (Optimized)
========================================
Size(B)    MB/s         Cycles/B     Time(us)    Status
----------------------------------------
64         8            30           0           FAIL
256        9            27           0           FAIL
1024       10           24           0           CLOSE
4096       11           22           0           CLOSE
16384      11           22           3           CLOSE
65536      11           22           12          CLOSE
131072     11           22           24          CLOSE

[OPT] Target: 15 MB/s
```

### 性能对比

```
========================================
  ChaCha20 Performance Comparison
========================================
Testing with 1KB data, 100 iterations:

MbedTLS 4.2.0: 11 MB/s (22 cycles/byte)
nrf_oberon:     12 MB/s (20 cycles/byte)
Oberon is 9% faster
```

---

## 如果性能未达标

### 情况 1: 两个库性能相同 (~7-11 MB/s)

**可能原因**:
- D-Cache 未正确启用
- 内存访问模式未优化
- CPU 主频限制

**解决方案**:
```bash
# 检查缓存状态
chacha20_oberon_test 6

# 如果显示 "D-Cache: Disabled"，需要检查系统配置
```

### 情况 2: Oberon 比 MbedTLS 慢

**可能原因**:
- nrf_oberon 库没有针对 Cortex-M33 优化
- 链接了错误的库版本 (soft-float vs hard-float)

**解决方案**:
```bash
# 检查库文件
ls -la external/oberon/nrf_oberon/lib/cortex-m33/

# 确认使用 hard-float 版本
# 如果有 soft-float，尝试替换
```

### 情况 3: 两个库都达到 11-12 MB/s，但未达 15 MB/s

**可能原因**:
- 软件优化已达极限
- 需要硬件支持

**进一步优化方案**:

1. **检查 -O3 优化**:
   编辑 `rtconfig.py`:
   ```python
   OPT = '-O3 -ffast-math -funroll-loops'
   ```

2. **检查 nrf_oberon 库优化**:
   ```bash
   arm-none-eabi-objdump -d liboberon_3.0.20.a | grep -A 20 "chacha20"
   ```

3. **检查 Helium/MVE 支持**:
   ```c
   // 在 main.c 中添加
   if (SCB->CCR & SCB_CCR_DC_Msk) {
       rt_kprintf("D-Cache: Enabled\n");
   }
   if (ARM_MVE_TYPE) {
       rt_kprintf("Helium: Supported\n");
   }
   ```

---

## 故障排除

### 问题 1: 编译错误 "undefined reference to mbedtls_chacha20_crypt"

**原因**: MbedTLS 库未编译

**解决**: 确保 `proj.conf` 中有:
```ini
CONFIG_CHACHA20_USE_MBEDTLS_420=y
```

### 问题 2: 编译错误 "undefined reference to ocrypto_chacha20_encode"

**原因**: nrf_oberon 库未编译

**解决**: 确保 `proj.conf` 中有:
```ini
CONFIG_CHACHA20_USE_OBERON=y
```

### 问题 3: 链接错误 "cannot find -loberon_3.0.20"

**原因**: 库文件路径错误

**解决**: 检查文件是否存在:
```
external/oberon/nrf_oberon/lib/cortex-m33/hard-float/liboberon_3.0.20.a
```

### 问题 4: 两个命令都找不到

**原因**: 测试代码未编译

**解决**: 确保 `proj.conf` 中有:
```ini
CONFIG_CHACHA20_USE_BOTH=y
```

---

## 性能基准

### 理论极限

| 场景 | 理论最大 | 说明 |
|---|---|---|
| 单发射 Cortex-M33 | 12 MB/s | 20 cycles/byte |
| 4-way SIMD (Helium) | 48 MB/s | 5 cycles/byte |
| 硬件加速 (AES) | 100+ MB/s | 专用硬件 |

### 当前状态

| 库 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| MbedTLS 4.2.0 | 7 MB/s | ~11 MB/s | +57% |
| nrf_oberon | 7 MB/s | ~12 MB/s | +71% |

### 目标

| 目标 | 可行性 | 所需优化 |
|---|---|---|
| 12 MB/s | ✅ 已达成 | 软件优化 |
| 15 MB/s | ⚠️ 困难 | 需要 -O3 + 循环展开 |
| 20 MB/s | ❌ 很难 | 需要 Helium 或硬件加速 |

---

## 下一步

1. **编译测试**
   ```bash
   cd project
   scons --board=sf32lb52-lchspi-ulp_hcpu -j8
   ```

2. **烧录验证**

3. **运行对比测试**
   ```bash
   chacha20_test 2           # MbedTLS 吞吐量
   chacha20_oberon_test 2    # Oberon 吞吐量
   chacha20_oberon_test 5    # 直接对比
   ```

4. **分析结果**
   - 如果两个库性能相近 → 瓶颈在系统层面 (缓存、内存)
   - 如果 Oberon 更快 → 库优化有效
   - 如果 MbedTLS 更快 → 检查 nrf_oberon 库是否正确

---

## 文件清单

| 文件 | 说明 |
|---|---|
| `project/proj.conf` | 配置文件 (启用双库) |
| `project/Kconfig.proj` | Kconfig 定义 |
| `project/SConscript` | 库编译逻辑 |
| `src/SConscript` | 测试文件编译逻辑 |
| `src/demos/.../ChaCha20_test.c` | MbedTLS 测试 (优化版) |
| `src/demos/.../ChaCha20_oberon.c` | Oberon 测试 (优化版) |
| `DUAL_LIBRARY_USAGE.md` | 本指南 |

---

**文档版本**: 1.0
**创建日期**: 2026-08-12
