# ChaCha20 性能分析与优化目标

## 1. 当前性能状况

### 1.1 测试环境
- **芯片**: SF32LB52 (Cortex-M33)
- **主频**: 240 MHz
- **编译器**: ARM GCC 14.2.1
- **优化级别**: -O2
- **测试库**:
  - MbedTLS 4.2.0 (标准 C 实现)
  - nrf_oberon (Nordic 优化库，包含汇编优化)

### 1.2 当前性能数据
| 库 | 最大吞吐量 | Cycles/Byte | 效率 |
|---|---|---|---|
| MbedTLS 4.2.0 | ~7 MB/s | ~34 | 2.9% |
| nrf_oberon | ~7 MB/s | ~34 | 2.9% |

### 1.3 目标性能
- **目标吞吐量**: 20 MB/s
- **目标 Cycles/Byte**: ≤12
- **目标效率**: ≥8.3%

---

## 2. 理论分析

### 2.1 ChaCha20 算法特性
- **分组大小**: 64 字节 (512 bits)
- **轮数**: 20 轮 (10 次双轮)
- **核心操作**: 32 位加法、异或、循环移位
- **并行性**: 4 个 32 位字独立更新 (4-way SIMD)

### 2.2 理论计算
假设每个字需要 4 次操作 (ADD, XOR, ROTL, ASSIGN)：
- 每个 64 字节块需要: 20 轮 × 4 操作/轮 × 16 字 = 1280 操作
- 如果 1 操作/周期: 1280 周期/64 字节 = 20 周期/字节
- **理论最大**: 240 MHz / 20 = 12 MB/s (单发射)

如果使用 4-way SIMD (Helium/MVE):
- 理论最大: 12 MB/s × 4 = 48 MB/s

### 2.3 为什么当前只有 7 MB/s?
实际 7 MB/s 对应 ~34 周期/字节，是理论值的 1.7 倍，说明存在显著开销。

---

## 3. 瓶颈分析

### 3.1 主要瓶颈 (按影响排序)

#### 🔴 瓶颈 1: 数据内存访问 (预计影响: 40%)
**问题**: ChaCha20 需要频繁读写状态矩阵 (64 字节)
- 每个块需要读取 key (32B) + nonce (12B) + counter (4B) + input (64B)
- 写入 output (64B)
- 状态矩阵在寄存器和 SRAM 之间频繁切换

**证据**:
- Cortex-M33 SRAM 访问延迟: 1-2 周期 (零等待状态)
- 但如果同时有其他总线主机 (DMA, LCPU)，延迟会增加
- 缓存未命中会导致 3-8 周期延迟

**验证方法**:
```c
// 检查 D-Cache 状态
if (SCB->CCR & SCB_CCR_DC_Msk) {
    rt_kprintf("D-Cache: Enabled\n");
} else {
    rt_kprintf("D-Cache: Disabled\n");
}
```

#### 🟡 瓶颈 2: 指令缓存 (预计影响: 20%)
**问题**: ChaCha20 的轮函数代码可能无法完全放入 I-Cache
- 20 轮展开的代码较大
- 循环跳转可能导致流水线冲刷

**证据**:
- Cortex-M33 I-Cache 通常为 4-16 KB
- ChaCha20 优化实现可能需要 2-4 KB 代码

#### 🟡 瓶颈 3: 函数调用开销 (预计影响: 15%)
**问题**: 每次调用 `ocrypto_chacha20_encode()` 都有开销
- 参数压栈/出栈
- 函数跳转
- 寄存器保存/恢复

**证据**:
- 当前测试中每次迭代都调用函数
- 如果内联，可能提升 10-15%

#### 🟢 瓶颈 4: 编译器优化限制 (预计影响: 10%)
**问题**: -O2 可能不是最优选择
- 某些循环展开可能被阻止
- SIMD 指令可能未被使用

**证据**:
- ARM GCC 的 -O3 可能生成更好的代码
- 特定函数属性可以强制优化

#### 🟢 瓶颈 5: 内存对齐 (预计影响: 10%)
**问题**: 缓冲区可能未对齐到 4/8 字节边界
- 非对齐访问需要额外周期
- 某些 SIMD 指令要求对齐

**证据**:
- `rt_malloc` 返回的地址可能不是 16 字节对齐
- 测试代码未显式对齐

### 3.2 次要因素

#### 中断延迟
- RT-Thread 调度器可能在测试期间切换任务
- 解决方案: 关闭中断进行测试

#### 栈溢出保护
- `-fstack-protector-strong` 会增加开销
- 可以在性能测试时禁用

---

## 4. 优化方案

### 4.1 短期优化 (预期提升: 30-50%)

#### 方案 1: 启用/优化缓存
```c
// 在 main 函数开头添加
SCB_EnableICache();
SCB_EnableDCache();
```
**预期提升**: 20-30%
**风险**: 低

#### 方案 2: 内存对齐分配
```c
// 使用对齐的内存分配
uint8_t *input = (uint8_t *)rt_malloc_align(size, 16);
uint8_t *output = (uint8_t *)rt_malloc_align(size, 16);
```
**预期提升**: 10-15%
**风险**: 低

#### 方案 3: 关闭中断测试
```c
rt_base_t level = rt_hw_interrupt_disable();
// 性能测试代码
rt_hw_interrupt_enable(level);
```
**预期提升**: 5-10%
**风险**: 中 (可能影响系统响应)

#### 方案 4: 使用 -O3 优化
修改 `rtconfig.py`:
```python
OPT = '-O3 -ffast-math'
```
**预期提升**: 10-15%
**风险**: 中 (可能增加代码大小)

### 4.2 中期优化 (预期提升: 50-100%)

#### 方案 5: 手动循环展开
```c
// 展开 4 次迭代
for (int iter = 0; iter < iterations; iter += 4) {
    ocrypto_chacha20_encode(output, input, size, test_nonce, 12, test_key, 0);
    ocrypto_chacha20_encode(output, input, size, test_nonce, 12, test_key, 0);
    ocrypto_chacha20_encode(output, input, size, test_nonce, 12, test_key, 0);
    ocrypto_chacha20_encode(output, input, size, test_nonce, 12, test_key, 0);
}
```
**预期提升**: 15-25%
**风险**: 低

#### 方案 6: 使用增量 API (ocrypto_chacha20_init + update)
```c
ocrypto_chacha20_ctx ctx;
ocrypto_chacha20_init(&ctx, test_nonce, 12, test_key, 0);
for (int iter = 0; iter < iterations; iter++) {
    ocrypto_chacha20_update(&ctx, output, input, size);
    // 重置 counter
    ctx.x[12] = 0;  // 假设 counter 在 x[12]
}
```
**预期提升**: 10-20%
**风险**: 中 (需要了解内部结构)

#### 方案 7: 检查 nrf_oberon 库优化
```bash
# 检查库中是否有针对 Cortex-M33 的优化
arm-none-eabi-objdump -d liboberon_3.0.20.a | grep -i "chacha"
```
**预期提升**: 0-50% (取决于库是否真正优化)
**风险**: 低

### 4.3 长期优化 (预期提升: 100-300%)

#### 方案 8: 使用 Helium/MVE (如果可用)
- Cortex-M33 可能支持 Helium 扩展
- 需要检查芯片是否启用 MVE
- 使用 CMSIS-DSP 或手写 SIMD

**预期提升**: 200-300%
**风险**: 高 (硬件限制)

#### 方案 9: DMA 加速
- 使用 DMA 在后台传输数据
- CPU 可以并行处理其他任务
- 适合大块数据传输

**预期提升**: 50-100%
**风险**: 高 (复杂度增加)

#### 方案 10: 混合硬件/软件
- 检查 SF32LB52 是否有硬件加密加速器
- 如果有 AES 硬件，可以考虑 AES-GCM 替代
- 或者使用硬件辅助的 ChaCha20

**预期提升**: 300-1000%
**风险**: 中 (需要硬件支持)

---

## 5. 实施优先级

### 第一阶段: 快速验证 (1-2天)
1. ✅ 启用 I-Cache 和 D-Cache
2. ✅ 检查内存对齐
3. ✅ 关闭中断进行测试
4. ✅ 检查 nrf_oberon 库是否真正优化

### 第二阶段: 编译器优化 (2-3天)
1. ✅ 测试 -O3 优化
2. ✅ 添加函数属性 (`__attribute__((hot))`)
3. ✅ 手动循环展开
4. ✅ 检查 RT-Thread 配置 (禁用栈保护)

### 第三阶段: 深度优化 (1-2周)
1. ✅ 分析 nrf_oberon 库的汇编代码
2. ✅ 检查 Helium/MVE 支持
3. ✅ 实现 DMA 加速 (如果适用)
4. ✅ 考虑混合硬件方案

---

## 6. 目标达成评估

### 6.1 乐观情况 (20 MB/s)
- ✅ 启用缓存 (+25%)
- ✅ 内存对齐 (+15%)
- ✅ -O3 优化 (+15%)
- ✅ 循环展开 (+20%)
- ✅ 关闭中断 (+10%)
- **总计**: 7 MB/s × 1.85 = ~13 MB/s (接近但未达)

### 6.2 悲观情况 (12 MB/s)
- ✅ 启用缓存 (+20%)
- ✅ 内存对齐 (+10%)
- ✅ -O3 优化 (+10%)
- ✅ 循环展开 (+15%)
- **总计**: 7 MB/s × 1.6 = ~11 MB/s

### 6.3 达标情况 (20 MB/s)
需要:
- ✅ 所有上述优化 (+85%)
- ✅ Helium/MVE 支持 (+100%)
- 或者
- ✅ 混合硬件方案 (+200%)

**结论**: 纯软件优化很难达到 20 MB/s，需要硬件支持或算法替换。

---

## 7. 替代方案

### 7.1 算法替换
如果 ChaCha20 性能无法满足需求，考虑:

1. **AES-GCM** (如果硬件支持 AES)
   - SF32LB52 可能有 AES 硬件加速器
   - 硬件 AES 可达 50-100 MB/s

2. **ChaCha20-Poly1305** (如果需要认证)
   - 硬件加速的 AEAD
   - 同时提供加密和认证

3. **流式加密优化**
   - 使用 `ocrypto_chacha20_init` + `ocrypto_chacha20_update`
   - 减少每次调用的开销

### 7.2 架构优化
1. **双缓冲**
   - 使用两个缓冲区交替处理
   - DMA 传输时 CPU 处理另一个

2. **流水线**
   - 将加密过程分成多个阶段
   - 每个阶段在不同的缓冲区上工作

3. **并行处理**
   - 如果有多核，可以并行处理多个块
   - SF32LB52 有 HCPU/LCPU，可以分工

---

## 8. 测试建议

### 8.1 新增测试项
在现有测试基础上，添加:

```c
// 1. 缓存状态检查
void check_cache_status(void);

// 2. 内存对齐测试
void test_alignment_impact(void);

// 3. 不同优化级别对比
void test_optimization_levels(void);

// 4. 中断影响测试
void test_interrupt_impact(void);

// 5. 增量 API 测试
void test_incremental_api(void);
```

### 8.2 性能计数器
使用 DWT 和 PMU 进行详细分析:

```c
// 启用性能计数器
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

// 读取缓存未命中 (如果支持)
// DWT->CBCVT 等
```

---

## 9. 结论

### 9.1 当前状态
- **实际性能**: 7 MB/s (34 cycles/byte)
- **理论极限**: 12-48 MB/s (取决于并行度)
- **瓶颈**: 主要是内存访问和指令缓存

### 9.2 目标可行性
- **15 MB/s**: ✅ 可行 (通过软件优化)
- **20 MB/s**: ⚠️ 困难 (需要硬件支持或深度优化)
- **30+ MB/s**: ❌ 不可行 (需要 Helium 或硬件加速)

### 9.3 建议
1. **短期**: 实施第一、二阶段优化，目标 12-15 MB/s
2. **中期**: 评估硬件加速支持，目标 15-20 MB/s
3. **长期**: 考虑算法替换 (AES-GCM) 或架构优化

### 9.4 风险提示
- 某些优化可能影响系统稳定性
- 硬件加速可能不可用
- 编译器优化可能导致代码大小增加

---

## 附录

### A. 参考资料
- [ARM Cortex-M33 Technical Reference Manual](https://developer.arm.com/documentation/100166/0001)
- [ChaCha20 Specification (RFC 8439)](https://datatracker.ietf.org/doc/html/rfc8439)
- [Nordic ocrypto Library Documentation](https://github.com/nordic-play/ocrypto)
- [SiFli SDK Documentation](https://github.com/OpenSiFli/SiFli-SDK)

### B. 文件清单
- 测试代码: `example/A0_example/src/demos/MbedTLS_Test_Demo/ChaCha20_oberon.c`
- 库配置: `example/A0_example/external/oberon/SConscript`
- 编译配置: `example/A0_example/project/rtconfig.py`

### C. 版本信息
- **文档版本**: 1.0
- **创建日期**: 2026-08-12
- **最后更新**: 2026-08-12
- **作者**: Claude Code
