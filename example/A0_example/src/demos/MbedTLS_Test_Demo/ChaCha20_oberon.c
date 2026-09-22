/**
 * @file ChaCha20_oberon.c
 * @brief ChaCha20 性能测试 (Oberon 库)
 *
 * 测试指标:
 * 1. 吞吐量 (Throughput) - MB/s
 * 2. 每字节周期数 (Cycles/Byte)
 * 3. 延迟 (Latency) - 微秒
 *
 * 优化目标: 15 MB/s
 */

#include "rtthread.h"
#include "rtdevice.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <stdlib.h>
#include "core_cm33.h"

/* Oberon ChaCha20 头文件（ocrypto 库） */
#include "ocrypto_chacha20.h"

/* MbedTLS 头文件 (用于对比测试) */
#include "mbedtls/private/chacha20.h"
#include "build_info.h"

/*===========================================================================
 * 性能测量工具 (DWT Cycle Counter)
 *===========================================================================*/
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004)
#define DEM_CR      (*(volatile uint32_t *)0xE000FC00)
#define DEM_CR_TRCENA   (1 << 24)

#define SYS_CLOCK_HZ    240000000UL  /* 240 MHz */

/* SF32LB52 内存映射 */
#define HPSYS_RAM0_BASE     (0x20000000)  /* DTCM - 最快 */
#define HPSYS_RAM0_SIZE     (128*1024)    /* 128KB */

/* 内存对齐宏 */
#define ALIGN_16BYTE(x)  (((uint32_t)(x) + 15) & ~15)
#define IS_ALIGNED_16(x) (((uint32_t)(x) & 15) == 0)

/* 检查地址是否在 DTCM 中 */
#define IS_IN_DTCM(addr) (((uint32_t)(addr) >= HPSYS_RAM0_BASE) && \
                          ((uint32_t)(addr) < HPSYS_RAM0_BASE + HPSYS_RAM0_SIZE))

static void dwt_init(void) {
    DEM_CR |= DEM_CR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= 0x1;
}

static inline uint32_t dwt_get_cycles(void) {
    return DWT_CYCCNT;
}

/*===========================================================================
 * 缓存管理优化
 *===========================================================================*/
static void optimize_cache_and_memory(void) {
    /* 1. 启用 I-Cache */
    if (!(SCB->CCR & SCB_CCR_IC_Msk)) {
        SCB_EnableICache();
        rt_kprintf("[OPT] I-Cache enabled\n");
    } else {
        rt_kprintf("[OPT] I-Cache already enabled\n");
    }

    /* 2. 启用 D-Cache */
    if (!(SCB->CCR & SCB_CCR_DC_Msk)) {
        SCB_EnableDCache();
        rt_kprintf("[OPT] D-Cache enabled\n");
    } else {
        rt_kprintf("[OPT] D-Cache already enabled\n");
    }

    /* 3. 检查 MPU 配置 (可选，用于优化关键区域) */
    rt_kprintf("[OPT] Cache optimization complete\n");
}

/*===========================================================================
 * 对齐内存分配 (确保在 DTCM 中)
 *===========================================================================*/
static uint8_t* aligned_malloc(size_t size) {
    /* 分配额外空间用于对齐 */
    uint8_t *raw = (uint8_t *)rt_malloc(size + 16 + sizeof(void*));
    if (raw == NULL) return NULL;

    /* 计算对齐地址 */
    uint32_t addr = (uint32_t)(raw + sizeof(void*));
    uint8_t *aligned = (uint8_t *)ALIGN_16BYTE(addr);

    /* 保存原始指针以便释放 */
    *((void**)(aligned - sizeof(void*))) = raw;

    /* 检查是否在 DTCM 中 */
    if (!IS_IN_DTCM(aligned)) {
        rt_kprintf("[WARN] Buffer not in DTCM! Address: 0x%08X\n", (uint32_t)aligned);
    }

    return aligned;
}

static void aligned_free(void *ptr) {
    if (ptr == NULL) return;
    void *raw = *((void**)((uint8_t*)ptr - sizeof(void*)));
    rt_free(raw);
}

/*===========================================================================
 * DTCM 静态缓冲区 (用于关键性能测试)
 *===========================================================================*/
/* 静态缓冲区 - 会自动放到 .bss 段 */
static uint8_t dtcm_input[131072];
static uint8_t dtcm_output[131072];

static int check_buffer_in_dtcm(void) {
    int in_dtcm = 1;

    rt_kprintf("[MEM] dtcm_input address:  0x%08X\n", (uint32_t)dtcm_input);
    rt_kprintf("[MEM] dtcm_output address: 0x%08X\n", (uint32_t)dtcm_output);
    rt_kprintf("[MEM] DTCM range: 0x%08X - 0x%08X\n", HPSYS_RAM0_BASE, HPSYS_RAM0_BASE + HPSYS_RAM0_SIZE);

    if (!IS_IN_DTCM(dtcm_input)) {
        rt_kprintf("[WARN] dtcm_input NOT in DTCM!\n");
        rt_kprintf("[WARN] Consider using .dtcm_data section in linker script\n");
        in_dtcm = 0;
    }

    if (!IS_IN_DTCM(dtcm_output)) {
        rt_kprintf("[WARN] dtcm_output NOT in DTCM!\n");
        in_dtcm = 0;
    }

    if (in_dtcm) {
        rt_kprintf("[OK] Buffers are in DTCM\n");
    } else {
        rt_kprintf("[INFO] Buffers may be in RAM1/RAM2 (slower)\n");
        rt_kprintf("[INFO] To force DTCM, add to linker script:\n");
        rt_kprintf("       .dtcm_data : { *(.dtcm_data) }\n");
    }

    return in_dtcm;
}

/*===========================================================================
 * 测试参数
 *===========================================================================*/
static const uint8_t test_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};

static const uint8_t test_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
    0x00, 0x00, 0x00, 0x00
};

static const size_t test_sizes[] = {64, 256, 1024, 4096, 16384, 65536, 131072};
#define NUM_TEST_SIZES  (sizeof(test_sizes) / sizeof(test_sizes[0]))

#define TEST_ITERATIONS     1000
#define LARGE_ITERATIONS    100
#define HUGE_ITERATIONS     10

/*===========================================================================
 * 正确性验证
 *===========================================================================*/
static int verify_chacha20_oberon_correctness(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Correctness Verification\n");
    rt_kprintf("========================================\n");

    /* MbedTLS self-test 向量 */
    const uint8_t key[32] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    const uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };

    const uint8_t plaintext[64] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    const uint8_t expected[64] = {
        0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90,
        0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
        0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a,
        0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
        0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d,
        0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
        0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c,
        0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86
    };

    uint8_t output[64];
    uint8_t decrypted[64];

    /* 测试加密 */
    ocrypto_chacha20_encode(output, plaintext, 64, nonce, 12, key, 0);

    if (memcmp(output, expected, 64) != 0) {
        rt_kprintf("❌ Oberon ChaCha20 encryption output mismatch!\n");
        return -1;
    }

    rt_kprintf("✅ Oberon ChaCha20 encryption: PASS\n");

    /* 测试解密（流密码，加密解密是同一操作） */
    ocrypto_chacha20_encode(decrypted, output, 64, nonce, 12, key, 0);

    if (memcmp(decrypted, plaintext, 64) != 0) {
        rt_kprintf("❌ Oberon ChaCha20 decryption output mismatch!\n");
        return -1;
    }

    rt_kprintf("✅ Oberon ChaCha20 decryption: PASS\n");
    rt_kprintf("✅ Oberon ChaCha20 correctness verification passed!\n");

    return 0;
}

/*===========================================================================
 * 吞吐量测试 (优化版)
 *===========================================================================*/
static void test_chacha20_oberon_throughput(void)
{
    dwt_init();

    /* 执行缓存和内存优化 */
    optimize_cache_and_memory();

    /* 检查 DTCM 缓冲区 */
    check_buffer_in_dtcm();

    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Throughput Test (DTCM Optimized)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s %-12s %-10s\n", "Size(B)", "MB/s", "Cycles/B", "Time(us)", "Status");
    rt_kprintf("----------------------------------------\n");

    for (int i = 0; i < NUM_TEST_SIZES; i++) {
        size_t size = test_sizes[i];

        /* 限制最大测试大小为 128KB (DTCM 大小) */
        if (size > sizeof(dtcm_input)) {
            rt_kprintf("%-10d %-12s %-12s %-12s %-10s\n",
                       size, "SKIP", "-", "-", "TOO LARGE");
            continue;
        }

        int iterations;
        if (size <= 1024) {
            iterations = TEST_ITERATIONS * 2;
        } else if (size <= 65536) {
            iterations = LARGE_ITERATIONS * 2;
        } else {
            iterations = HUGE_ITERATIONS * 2;
        }

        /* 使用 DTCM 静态缓冲区 */
        uint8_t *input = dtcm_input;
        uint8_t *output = dtcm_output;

        /* 填充测试数据 */
        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        /* 预热 */
        for (int j = 0; j < 20; j++) {
            ocrypto_chacha20_encode(output, input, size, test_nonce, 12, test_key, 0);
        }

        /* 清空 D-Cache */
        SCB_CleanDCache_by_Addr((void*)input, size);
        SCB_CleanDCache_by_Addr((void*)output, size);

        /* 性能测试 */
        uint32_t primask = __get_PRIMASK(); __disable_irq();

        uint32_t start_cycles = dwt_get_cycles();

        /* 增量 API */
        ocrypto_chacha20_ctx ctx;
        ocrypto_chacha20_init(&ctx, test_nonce, 12, test_key, 0);
        for (int iter = 0; iter < iterations; iter++) {
            ocrypto_chacha20_update(&ctx, output, input, size);
        }

        uint32_t end_cycles = dwt_get_cycles();

        __set_PRIMASK(primask);

        /* 使 D-Cache 失效 */
        SCB_InvalidateDCache_by_Addr((void*)output, size);

        uint32_t elapsed_cycles = end_cycles - start_cycles;

        uint64_t total_bytes = (uint64_t)size * iterations;
        uint64_t bytes_per_sec = (uint64_t)total_bytes * SYS_CLOCK_HZ / elapsed_cycles;
        uint32_t throughput_mbps = (uint32_t)(bytes_per_sec / (1024 * 1024));
        uint32_t cycles_per_byte = (uint32_t)(elapsed_cycles / total_bytes);
        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);

        const char *status = throughput_mbps >= 15 ? "PASS" : (throughput_mbps >= 10 ? "CLOSE" : "FAIL");

        rt_kprintf("%-10d %-12d %-12d %-12d %-10s\n",
                   size, throughput_mbps, cycles_per_byte, elapsed_us, status);
    }

    rt_kprintf("\n[INFO] Target: 15 MB/s\n");
}

/*===========================================================================
 * 延迟测试 (优化版)
 *===========================================================================*/
static void test_chacha20_oberon_latency(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Latency Test (64B, Optimized)\n");
    rt_kprintf("========================================\n");

    const size_t latency_size = 64;
    uint8_t input[64] __attribute__((aligned(16)));
    uint8_t output[64] __attribute__((aligned(16)));

    for (int i = 0; i < 64; i++) {
        input[i] = i & 0xFF;
    }

    /* 预热 (增加预热次数) */
    for (int i = 0; i < 200; i++) {
        ocrypto_chacha20_encode(output, input, latency_size, test_nonce, 12, test_key, 0);
    }

    /* 清空 D-Cache */
    SCB_CleanDCache_by_Addr((void*)input, latency_size);
    SCB_CleanDCache_by_Addr((void*)output, latency_size);

    /* 关闭中断测量单次加密延迟 */
    uint32_t primask = __get_PRIMASK(); __disable_irq();

    uint32_t start_cycles = dwt_get_cycles();
    ocrypto_chacha20_encode(output, input, latency_size, test_nonce, 12, test_key, 0);
    uint32_t end_cycles = dwt_get_cycles();

    __set_PRIMASK(primask);

    uint32_t elapsed_cycles = end_cycles - start_cycles;
    uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);

    rt_kprintf("Single 64B encrypt: %d cycles, %d us\n", elapsed_cycles, elapsed_us);
    rt_kprintf("Cycles/byte: %d\n", elapsed_cycles / latency_size);

    /* 多次测量取平均值 */
    rt_kprintf("\nMulti-measurement (100 iterations):\n");
    uint32_t total_cycles = 0;
    for (int i = 0; i < 100; i++) {
        SCB_CleanDCache_by_Addr((void*)input, latency_size);
        SCB_CleanDCache_by_Addr((void*)output, latency_size);

        uint32_t s = dwt_get_cycles();
        ocrypto_chacha20_encode(output, input, latency_size, test_nonce, 12, test_key, 0);
        uint32_t e = dwt_get_cycles();
        total_cycles += (e - s);
    }
    uint32_t avg_cycles = total_cycles / 100;
    uint32_t avg_us = avg_cycles / (SYS_CLOCK_HZ / 1000000);
    rt_kprintf("Average: %d cycles, %d us\n", avg_cycles, avg_us);
    rt_kprintf("Average Cycles/byte: %d\n", avg_cycles / latency_size);
}

/*===========================================================================
 * 性能对比测试 (MbedTLS vs Oberon)
 *===========================================================================*/
static void test_chacha20_comparison(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Performance Comparison\n");
    rt_kprintf("========================================\n");

    /* 注意: 此测试需要 MbedTLS 也编译时才能运行 */
    #if defined(MBEDTLS_CHACHA20_C)
    rt_kprintf("Testing with 1KB data, 100 iterations:\n\n");

    const size_t test_size = 1024;
    uint8_t *input = aligned_malloc(test_size);
    uint8_t *output = aligned_malloc(test_size);

    if (input == NULL || output == NULL) {
        rt_kprintf("Memory allocation failed\n");
        if (input) aligned_free(input);
        if (output) aligned_free(output);
        return;
    }

    /* 填充测试数据 */
    for (size_t j = 0; j < test_size; j++) {
        input[j] = j & 0xFF;
    }

    const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };

    const uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
        0x00, 0x00, 0x00, 0x00
    };

    /* 预热 */
    for (int j = 0; j < 20; j++) {
        mbedtls_chacha20_crypt(key, nonce, 0, test_size, input, output);
        ocrypto_chacha20_encode(output, input, test_size, nonce, 12, key, 0);
    }

    /* 测试 MbedTLS */
    SCB_CleanDCache_by_Addr((void*)input, test_size);
    SCB_CleanDCache_by_Addr((void*)output, test_size);
    uint32_t primask = __get_PRIMASK(); __disable_irq();
    uint32_t start = dwt_get_cycles();
    for (int j = 0; j < 100; j++) {
        mbedtls_chacha20_crypt(key, nonce, 0, test_size, input, output);
    }
    uint32_t end = dwt_get_cycles();
    __set_PRIMASK(primask);
    uint32_t mbedtls_cycles = end - start;

    /* 测试 Oberon */
    SCB_CleanDCache_by_Addr((void*)input, test_size);
    SCB_CleanDCache_by_Addr((void*)output, test_size);
    primask = __get_PRIMASK(); __disable_irq();
    start = dwt_get_cycles();
    for (int j = 0; j < 100; j++) {
        ocrypto_chacha20_encode(output, input, test_size, nonce, 12, key, 0);
    }
    end = dwt_get_cycles();
    __set_PRIMASK(primask);
    uint32_t oberon_cycles = end - start;

    /* 计算结果 */
    uint64_t total_bytes = (uint64_t)test_size * 100;
    uint64_t mbedtls_bytes_per_sec = (uint64_t)total_bytes * SYS_CLOCK_HZ / mbedtls_cycles;
    uint64_t oberon_bytes_per_sec = (uint64_t)total_bytes * SYS_CLOCK_HZ / oberon_cycles;

    rt_kprintf("MbedTLS 4.2.0: %d MB/s (%d cycles/byte)\n",
               (uint32_t)(mbedtls_bytes_per_sec / (1024 * 1024)),
               mbedtls_cycles / (uint32_t)total_bytes);
    rt_kprintf("nrf_oberon:     %d MB/s (%d cycles/byte)\n",
               (uint32_t)(oberon_bytes_per_sec / (1024 * 1024)),
               oberon_cycles / (uint32_t)total_bytes);

    if (oberon_cycles < mbedtls_cycles) {
        uint32_t improvement = (mbedtls_cycles - oberon_cycles) * 100 / mbedtls_cycles;
        rt_kprintf("Oberon is %d%% faster\n", improvement);
    } else {
        uint32_t slower = (oberon_cycles - mbedtls_cycles) * 100 / mbedtls_cycles;
        rt_kprintf("Oberon is %d%% slower\n", slower);
    }

    aligned_free(input);
    aligned_free(output);
    #else
    rt_kprintf("MbedTLS not compiled, comparison skipped\n");
    #endif
}

/*===========================================================================
 * 硬件信息检查
 *===========================================================================*/
static void check_hardware_info(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Hardware Information\n");
    rt_kprintf("========================================\n");

    /* CPU 信息 */
    rt_kprintf("CPU: Cortex-M33\n");
    rt_kprintf("Core Frequency: %d MHz\n", SystemCoreClock / 1000000);

    /* 内存信息 */
    rt_kprintf("\nMemory Map:\n");
    rt_kprintf("  DTCM:   0x%08X - 0x%08X (%d KB)\n",
               HPSYS_RAM0_BASE, HPSYS_RAM0_BASE + HPSYS_RAM0_SIZE - 1,
               HPSYS_RAM0_SIZE / 1024);
    rt_kprintf("  RAM1:   0x%08X - 0x%08X (%d KB)\n",
               0x20020000, 0x20020000 + 128*1024 - 1, 128);
    rt_kprintf("  RAM2:   0x%08X - 0x%08X (%d KB)\n",
               0x20040000, 0x20040000 + 256*1024 - 1, 256);

    /* 缓存信息 */
    rt_kprintf("\nCache Status:\n");
    rt_kprintf("  I-Cache: %s\n", (SCB->CCR & SCB_CCR_IC_Msk) ? "Enabled" : "Disabled");
    rt_kprintf("  D-Cache: %s\n", (SCB->CCR & SCB_CCR_DC_Msk) ? "Enabled" : "Disabled");

    /* 硬件加速器 */
    rt_kprintf("\nHardware Accelerators:\n");
    rt_kprintf("  AES: 0x%08X (Hardware AES-128/192/256)\n", 0x5000d000);
    rt_kprintf("  ChaCha20: Not supported in hardware\n");

    /* 性能提示 */
    rt_kprintf("\nOptimization Tips:\n");
    rt_kprintf("  1. Ensure buffers in DTCM (0x%08X)\n", HPSYS_RAM0_BASE);
    rt_kprintf("  2. Use AES-CTR instead of ChaCha20 if possible\n");
    rt_kprintf("  3. For large data, consider DMA transfer\n");
}

/*===========================================================================
 * 菜单和入口
 *===========================================================================*/
static void print_oberon_menu(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Test Menu\n");
    rt_kprintf("========================================\n");
    rt_kprintf("1. Correctness verification\n");
    rt_kprintf("2. Throughput test (DTCM Optimized)\n");
    rt_kprintf("3. Latency test (Optimized)\n");
    rt_kprintf("4. Run all tests\n");
    rt_kprintf("5. Performance comparison (MbedTLS vs Oberon)\n");
    rt_kprintf("6. Check cache/memory status\n");
    rt_kprintf("7. Hardware info (AES accelerator)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Enter option: ");
}

void chacha20_oberon_test(int argc, char **argv)
{
    if (argc < 2) {
        print_oberon_menu();
        return;
    }

    int option = atoi(argv[1]);

    switch (option) {
    case 1:
        verify_chacha20_oberon_correctness();
        break;
    case 2:
        test_chacha20_oberon_throughput();
        break;
    case 3:
        test_chacha20_oberon_latency();
        break;
    case 4:
        verify_chacha20_oberon_correctness();
        test_chacha20_oberon_throughput();
        test_chacha20_oberon_latency();
        break;
    case 5:
        test_chacha20_comparison();
        break;
    case 6:
        optimize_cache_and_memory();
        check_buffer_in_dtcm();
        break;
    case 7:
        check_hardware_info();
        break;
    default:
        print_oberon_menu();
        break;
    }
}

/* 注册为 finsh 命令 */
MSH_CMD_EXPORT(chacha20_oberon_test, Oberon ChaCha20 performance test);
