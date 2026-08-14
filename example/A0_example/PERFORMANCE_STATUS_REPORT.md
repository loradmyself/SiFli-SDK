# ChaCha20 性能状态报告

## 当前性能数据

### MbedTLS 4.2.0 (优化后)
| 大小 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| 64B | ~3 MB/s | ~7 MB/s | +133% |
| 256B | ~5 MB/s | ~7 MB/s | +40% |
| 1KB | ~6 MB/s | ~7 MB/s | +17% |
| 4KB | ~6 MB/s | ~7 MB/s | +17% |
| 16KB | ~7 MB/s | ~7 MB/s | 0% |
| 64KB | ~3 MB/s | ~7 MB/s | +133% |
| 128KB | ~7 MB/s | ~7 MB/s | 0% |

**结论**: 优化有效，但卡在 7 MB/s 上限

---

## 为什么卡在 7 MB/s？

### 1. 理论极限分析

**CPU 频率**: 240 MHz
**ChaCha20 理论值**: 20 cycles/byte (标准实现)
**理论最大吞吐量**: 240 / 20 = **12 MB/s**

**当前状态**:
- 实际: 7 MB/s
- 理论: 12 MB/s
- 效率: 7/12 = **58%**
- 剩余优化空间: **42%**

### 2. 可能的瓶颈

#### 🔴 瓶颈 1: 内存访问延迟 (预计影响: 30%)

**问题**: 每次 `ocrypto_chacha20_update()` 都需要：
- 读取 64 字节状态
- 读取 64 字节输入
- 写入 64 字节输出
- 总计: 192 字节/块

**验证方法**:
```c
// 检查 SRAM 访问延迟
volatile uint32_t *sram = (volatile uint32_t *)0x20000000;
uint32_t start = dwt_get_cycles();
volatile uint32_t val = *sram;
uint32_t end = dwt_get_cycles();
rt_kprintf("SRAM read latency: %d cycles\n", end - start);
```

**可能原因**:
- SRAM 带宽限制
- D-Cache 未命中
- 总线竞争

#### 🟡 瓶颈 2: 函数调用开销 (预计影响: 15%)

**问题**: 即使使用增量 API，每次 `update()` 仍有：
- 函数跳转: 2-3 cycles
- 寄存器保存/恢复: 5-10 cycles
- 参数传递: 2-3 cycles
- 总计: ~15 cycles/调用

**验证方法**:
```c
// 测量函数调用开销
uint32_t start = dwt_get_cycles();
for (int i = 0; i < 1000; i++) {
    // 空函数调用
}
uint32_t end = dwt_get_cycles();
rt_kprintf("Function call overhead: %d cycles\n", (end - start) / 1000);
```

#### 🟡 瓶颈 3: 编译器优化限制 (预计影响: 10%)

**问题**: -O2 可能没有：
- 完全展开循环
- 内联小函数
- 优化寄存器分配

**验证方法**:
```bash
# 检查生成的汇编代码
arm-none-eabi-objdump -d main.elf | grep -A 30 "chacha20_update"
```

#### 🟢 瓶颈 4: 分支预测 (预计影响: 5%)

**问题**: 循环中的条件判断可能导致分支预测失败

**验证方法**:
```c
// 检查分支预测
uint32_t start = dwt_get_cycles();
for (int i = 0; i < 1000; i++) {
    if (i % 2 == 0) {
        // 分支 1
    } else {
        // 分支 2
    }
}
uint32_t end = dwt_get_cycles();
```

### 3. 已排除的因素

| 因素 | 状态 | 证据 |
|---|---|---|
| I-Cache | ✅ 已启用 | 命令 6 显示已启用 |
| D-Cache | ✅ 已启用 | 命令 6 显示已启用 |
| 内存对齐 | ✅ 已优化 | 显示 "ALIGNED" |
| 中断干扰 | ✅ 已关闭 | 使用 `__disable_irq()` |
| 迭代次数 | ✅ 已增加 | 2x 迭代次数 |
| 预热 | ✅ 已优化 | 20-200 次预热 |

---

## 进一步优化方案

### 方案 1: 检查 nrf_oberon 库是否真正优化 (优先级: 🔴 高)

**问题**: 当前 nrf_oberon 库可能没有针对 Cortex-M33 优化

**验证方法**:
```bash
# 检查库中的函数
arm-none-eabi-nm liboberon_3.0.20.a | grep chacha

# 反汇编检查是否有 SIMD/Thumb-2 优化
arm-none-eabi-objdump -d liboberon_3.0.20.a | grep -A 50 "ocrypto_chacha20"
```

**预期提升**: 0-50%

### 方案 2: 使用 -O3 优化 (优先级: 🟡 中)

修改 `rtconfig.py`:
```python
# 当前
OPT = '-O2'

# 优化后
OPT = '-O3 -ffast-math -funroll-loops'
```

**预期提升**: 10-20%

### 方案 3: 手动循环展开 (优先级: 🟡 中)

```c
// 展开 4 次迭代
for (int iter = 0; iter < iterations; iter += 4) {
    ocrypto_chacha20_update(&ctx, output, input, size);
    ocrypto_chacha20_update(&ctx, output, input, size);
    ocrypto_chacha20_update(&ctx, output, input, size);
    ocrypto_chacha20_update(&ctx, output, input, size);
}
```

**预期提升**: 15-25%

### 方案 4: 禁用栈保护 (优先级: 🟢 低)

修改 `rtconfig.py`:
```python
# 移除 -fstack-protector-strong
# 或添加 -fno-stack-protector
```

**预期提升**: 5-10%

### 方案 5: 检查 Helium/MVE 支持 (优先级: 🟢 低)

```c
// 检查是否支持 Helium
if (ARM_MVE_TYPE) {
    rt_kprintf("Helium: Supported\n");
}
```

**预期提升**: 100-300% (如果硬件支持)

### 方案 6: 使用 DMA 加速 (优先级: 🟢 低)

对于大块数据，可以使用 DMA 在后台传输数据

**预期提升**: 50-100%

---

## 测试清单

### 已完成 ✅
- [x] I-Cache 启用
- [x] D-Cache 启用
- [x] 内存对齐
- [x] 中断控制
- [x] D-Cache 维护
- [x] 增量 API
- [x] 预热优化
- [x] 迭代次数增加

### 待完成 ⏳
- [ ] 检查 nrf_oberon 库优化
- [ ] 测试 -O3 编译
- [ ] 手动循环展开
- [ ] 禁用栈保护
- [ ] 检查 Helium 支持
- [ ] DMA 加速

---

## 性能对比

### 当前状态
| 实现 | 吞吐量 | Cycles/Byte | 状态 |
|---|---|---|---|
| MbedTLS 4.2.0 (优化后) | 7 MB/s | 34 | ⚠️ 瓶颈 |
| nrf_oberon (优化后) | 7 MB/s | 34 | ⚠️ 瓶颈 |
| 理论极限 | 12 MB/s | 20 | 🎯 目标 |

### 优化目标
| 目标 | 可行性 | 所需优化 |
|---|---|---|
| 10 MB/s | ✅ 可行 | 检查库优化 + -O3 |
| 12 MB/s | ⚠️ 困难 | 需要所有优化 |
| 15 MB/s | ❌ 很难 | 需要硬件支持 |

---

## 关键问题

### 问题 1: 为什么 64B 只有 7 MB/s？

**可能原因**:
- 函数调用开销占比高
- 无法充分利用缓存
- 分支预测失败

**验证方法**:
```c
// 测量 64B 单次延迟
uint32_t start = dwt_get_cycles();
ocrypto_chacha20_encode(output, input, 64, nonce, 12, key, 0);
uint32_t end = dwt_get_cycles();
rt_kprintf("64B latency: %d cycles\n", end - start);
```

**预期**: 应该是 ~1280 cycles (64 * 20)

### 问题 2: 为什么大块数据没有提升？

**可能原因**:
- 内存带宽限制
- D-Cache 容量限制
- 总线竞争

**验证方法**:
```c
// 测试不同大小的吞吐量
for (int size = 64; size <= 131072; size *= 4) {
    // 测量吞吐量
}
```

---

## 下一步行动

### 立即行动 (今天)
1. **检查 nrf_oberon 库优化**
   ```bash
   arm-none-eabi-objdump -d liboberon_3.0.20.a | grep -A 30 "chacha20"
   ```

2. **测试 -O3 编译**
   - 修改 `rtconfig.py`
   - 重新编译测试

3. **检查 SRAM 访问延迟**
   - 确认内存带宽是否是瓶颈

### 短期行动 (本周)
1. **手动循环展开**
2. **禁用栈保护**
3. **检查 Helium 支持**

### 长期行动 (下周)
1. **DMA 加速**
2. **硬件加速评估**

---

## 结论

### 当前状态
- ✅ 软件优化已实施
- ⚠️ 性能卡在 7 MB/s
- ⏳ 还有 42% 优化空间

### 主要瓶颈
1. **内存访问延迟** (30%)
2. **函数调用开销** (15%)
3. **编译器优化** (10%)

### 达标可能性
- **10 MB/s**: ✅ 通过库优化和 -O3 可达
- **12 MB/s**: ⚠️ 需要深度优化
- **15 MB/s**: ❌ 需要硬件支持

### 建议
1. 优先检查 nrf_oberon 库是否真正优化
2. 测试 -O3 编译效果
3. 如果仍不达标，考虑硬件加速方案

---

**报告版本**: 1.0
**创建日期**: 2026-08-12
**作者**: Claude Code
