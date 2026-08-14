/**
 * @file ChaCha20_test.c
 * @brief ChaCha20 性能测试 (MbedTLS 4.2.0) - 优化版
 *
 * 测试指标:
 * 1. 吞吐量 (Throughput) - MB/s
 * 2. 每字节周期数 (Cycles/Byte)
 * 3. 延迟 (Latency) - 微秒
 * 4. AES-128-CTR 对比测试
 *
 * 优化: 缓存、内存对齐、中断控制
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <stdlib.h>
#include "core_cm33.h"

/* MbedTLS 4.2.0 ChaCha20 头文件 */
#include "mbedtls/private/chacha20.h"
#include "build_info.h"

/* 如果需要 AES 对比测试 */
#if defined(MBEDTLS_AES_C)
#include "mbedtls/private/aes.h"
#endif

/*===========================================================================
 * 性能测量工具
 *===========================================================================*/

/* 系统时钟频率 */
#define SYS_CLOCK_HZ    SystemCoreClock

/* 内存对齐宏 */
#define ALIGN_16BYTE(x)  (((uint32_t)(x) + 15) & ~15)
#define IS_ALIGNED_16(x) (((uint32_t)(x) & 15) == 0)

/**
 * @brief 初始化 DWT 周期计数器
 */
static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
 * @brief 获取 DWT 周期计数
 */
static inline uint32_t dwt_get_cycles(void)
{
    return DWT->CYCCNT;
}

/*===========================================================================
 * 缓存管理优化 (与 Oberon 测试相同)
 *===========================================================================*/
static void optimize_cache_and_memory(void)
{
    /* 启用 I-Cache */
    if (!(SCB->CCR & SCB_CCR_IC_Msk)) {
        SCB_EnableICache();
        rt_kprintf("[OPT] I-Cache enabled\n");
    } else {
        rt_kprintf("[OPT] I-Cache already enabled\n");
    }

    /* 启用 D-Cache */
    if (!(SCB->CCR & SCB_CCR_DC_Msk)) {
        SCB_EnableDCache();
        rt_kprintf("[OPT] D-Cache enabled\n");
    } else {
        rt_kprintf("[OPT] D-Cache already enabled\n");
    }

    rt_kprintf("[OPT] Cache optimization complete\n");
}

/*===========================================================================
 * 对齐内存分配
 *===========================================================================*/
static uint8_t* aligned_malloc(size_t size)
{
    uint8_t *raw = (uint8_t *)rt_malloc(size + 16 + sizeof(void*));
    if (raw == NULL) return NULL;

    uint32_t addr = (uint32_t)(raw + sizeof(void*));
    uint8_t *aligned = (uint8_t *)ALIGN_16BYTE(addr);

    *((void**)(aligned - sizeof(void*))) = raw;

    return aligned;
}

static void aligned_free(void *ptr)
{
    if (ptr == NULL) return;
    void *raw = *((void**)((uint8_t*)ptr - sizeof(void*)));
    rt_free(raw);
}

/*===========================================================================
 * 测试数据
 *===========================================================================*/

/* 测试密钥 (256-bit) */
static const uint8_t test_key[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 测试 Nonce (96-bit) */
static const uint8_t test_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* 不同测试数据大小 */
static const size_t test_sizes[] = {
    64,         /* 1 block */
    256,        /* 4 blocks */
    1024,       /* 1 KB */
    4096,       /* 4 KB */
    16384,      /* 16 KB */
    65536,      /* 64 KB */
    131072,     /* 128 KB */
};

#define NUM_TEST_SIZES  (sizeof(test_sizes) / sizeof(test_sizes[0]))
#define TEST_ITERATIONS 1000    /* 小数据块迭代次数 */
#define LARGE_ITERATIONS 100    /* 中等数据块迭代次数 */
#define HUGE_ITERATIONS 10      /* 大数据块迭代次数 (128KB) */

/*===========================================================================
 * ChaCha20 正确性验证
 *===========================================================================*/

/**
 * @brief 验证 ChaCha20 加解密正确性
 */
static int verify_chacha20_correctness(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Correctness Verification\n");
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
    int ret;

    /* 测试加密 */
    ret = mbedtls_chacha20_crypt(key, nonce, 0, 64, plaintext, output);
    if (ret != 0) {
        rt_kprintf("❌ ChaCha20 encryption failed: -0x%04x\n", -ret);
        return -1;
    }

    if (memcmp(output, expected, 64) != 0) {
        rt_kprintf("❌ ChaCha20 encryption output mismatch!\n");
        return -1;
    }
    rt_kprintf("✅ ChaCha20 encryption: PASSED\n");

    /* 测试解密 (流密码，加密解密相同) */
    uint8_t decrypted[64];
    ret = mbedtls_chacha20_crypt(key, nonce, 0, 64, output, decrypted);
    if (ret != 0) {
        rt_kprintf("❌ ChaCha20 decryption failed: -0x%04x\n", -ret);
        return -1;
    }

    if (memcmp(decrypted, plaintext, 64) != 0) {
        rt_kprintf("❌ ChaCha20 decryption output mismatch!\n");
        return -1;
    }
    rt_kprintf("✅ ChaCha20 decryption: PASSED\n");

    return 0;
}

/*===========================================================================
 * ChaCha20 吞吐量测试 (优化版)
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20 加密吞吐量
 */
static void test_chacha20_throughput(void)
{
    dwt_init();

    /* 执行缓存和内存优化 */
    optimize_cache_and_memory();

    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Throughput Test (Optimized)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s %-12s %-10s\n", "Size(B)", "MB/s", "Cycles/B", "Time(us)", "Status");
    rt_kprintf("----------------------------------------\n");

    for (int i = 0; i < NUM_TEST_SIZES; i++) {
        size_t size = test_sizes[i];
        int iterations;
        if (size <= 1024) {
            iterations = TEST_ITERATIONS * 2;
        } else if (size <= 65536) {
            iterations = LARGE_ITERATIONS * 2;
        } else {
            iterations = HUGE_ITERATIONS * 2;
        }

        /* 使用对齐的内存分配 */
        uint8_t *input = aligned_malloc(size);
        uint8_t *output = aligned_malloc(size);
        if (input == NULL || output == NULL) {
            rt_kprintf("Memory allocation failed for size %d\n", size);
            if (input) aligned_free(input);
            if (output) aligned_free(output);
            continue;
        }

        /* 检查对齐状态 */
        const char *align_status = IS_ALIGNED_16(input) && IS_ALIGNED_16(output) ? "ALIGNED" : "UNALIGNED";

        /* 填充测试数据 */
        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        /* 预热 (增加预热次数) */
        for (int j = 0; j < 20; j++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        /* 清空 D-Cache */
        SCB_CleanDCache_by_Addr((void*)input, size);
        SCB_CleanDCache_by_Addr((void*)output, size);

        /* 性能测试 - 使用增量 API */
        uint32_t primask = __get_PRIMASK(); __disable_irq();

        uint32_t start_cycles = dwt_get_cycles();

        /* MbedTLS 增量 API */
        mbedtls_chacha20_context ctx;
        mbedtls_chacha20_init(&ctx);
        mbedtls_chacha20_setkey(&ctx, test_key);
        mbedtls_chacha20_starts(&ctx, test_nonce, 0);
        for (int iter = 0; iter < iterations; iter++) {
            mbedtls_chacha20_update(&ctx, size, input, output);
        }
        mbedtls_chacha20_free(&ctx);

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

        /* 判断是否达到目标 */
        const char *status = throughput_mbps >= 15 ? "PASS" : (throughput_mbps >= 10 ? "CLOSE" : "FAIL");

        rt_kprintf("%-10d %-12d %-12d %-12d %-10s\n",
                   size, throughput_mbps, cycles_per_byte, elapsed_us, status);

        aligned_free(input);
        aligned_free(output);
    }

    rt_kprintf("\n[OPT] Target: 15 MB/s\n");
}

/*===========================================================================
 * ChaCha20 延迟测试 (优化版)
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20 小数据块延迟
 */
static void test_chacha20_latency(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Latency Test (Optimized)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s\n", "Size(B)", "Latency(us)", "Cycles");
    rt_kprintf("----------------------------------------\n");

    const size_t small_sizes[] = {1, 8, 16, 32, 64};
    const int small_iterations = 10000;

    for (int i = 0; i < 5; i++) {
        size_t size = small_sizes[i];

        uint8_t *input = aligned_malloc(size);
        uint8_t *output = aligned_malloc(size);
        if (input == NULL || output == NULL) {
            if (input) aligned_free(input);
            if (output) aligned_free(output);
            continue;
        }

        memset(input, 0x5A, size);

        /* 预热 (增加预热次数) */
        for (int j = 0; j < 200; j++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        /* 清空 D-Cache */
        SCB_CleanDCache_by_Addr((void*)input, size);
        SCB_CleanDCache_by_Addr((void*)output, size);

        /* 延迟测试 - 关闭中断 */
        uint32_t primask = __get_PRIMASK(); __disable_irq();

        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < small_iterations; iter++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        uint32_t end_cycles = dwt_get_cycles();

        __set_PRIMASK(primask);

        uint32_t elapsed_cycles = end_cycles - start_cycles;

        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);
        uint32_t latency_us = elapsed_us / small_iterations;
        uint32_t cycles_per_op = elapsed_cycles / small_iterations;

        rt_kprintf("%-10d %-12d %-12d\n", size, latency_us, cycles_per_op);

        aligned_free(input);
        aligned_free(output);
    }
}

/*===========================================================================
 * AES-128-CTR 对比测试
 *===========================================================================*/

#if defined(MBEDTLS_AES_C)
/**
 * @brief 测试 AES-128-CTR 吞吐量 (与 ChaCha20 对比)
 */
static void test_aes_throughput(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  AES-128-CTR Throughput Test\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s %-12s\n", "Size(B)", "MB/s", "Cycles/B", "Time(us)");
    rt_kprintf("----------------------------------------\n");

    const uint8_t aes_key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };

    for (int i = 0; i < NUM_TEST_SIZES; i++) {
        size_t size = test_sizes[i];
        int iterations;
        if (size <= 1024) {
            iterations = TEST_ITERATIONS;
        } else if (size <= 65536) {
            iterations = LARGE_ITERATIONS;
        } else {
            iterations = HUGE_ITERATIONS;
        }

        uint8_t *input = rt_malloc(size);
        uint8_t *output = rt_malloc(size);
        if (input == NULL || output == NULL) {
            if (input) rt_free(input);
            if (output) rt_free(output);
            continue;
        }

        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        mbedtls_aes_context aes_ctx;
        uint8_t nonce_counter[16] = {0};
        uint8_t stream_block[16] = {0};

        /* 预热 */
        mbedtls_aes_init(&aes_ctx);
        mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
        for (int j = 0; j < 10; j++) {
            size_t nc_off = 0;
            memcpy(nonce_counter, aes_key, 16);
            mbedtls_aes_crypt_ctr(&aes_ctx, size, &nc_off,
                                  nonce_counter, stream_block, input, output);
        }
        mbedtls_aes_free(&aes_ctx);

        /* 性能测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < iterations; iter++) {
            mbedtls_aes_init(&aes_ctx);
            mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
            size_t nc_off = 0;
            memcpy(nonce_counter, aes_key, 16);
            mbedtls_aes_crypt_ctr(&aes_ctx, size, &nc_off,
                                  nonce_counter, stream_block, input, output);
            mbedtls_aes_free(&aes_ctx);
        }

        uint32_t end_cycles = dwt_get_cycles();
        uint32_t elapsed_cycles = end_cycles - start_cycles;

        uint64_t total_bytes = (uint64_t)size * iterations;
        uint64_t bytes_per_sec = (uint64_t)total_bytes * SYS_CLOCK_HZ / elapsed_cycles;
        uint32_t throughput_mbps = (uint32_t)(bytes_per_sec / (1024 * 1024));
        uint32_t cycles_per_byte = (uint32_t)(elapsed_cycles / total_bytes);
        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);

        rt_kprintf("%-10d %-12d %-12d %-12d\n",
                   size, throughput_mbps, cycles_per_byte, elapsed_us);

        rt_free(input);
        rt_free(output);
    }
}

/**
 * @brief 测试 AES-128-CTR 小数据块延迟
 */
static void test_aes_latency(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  AES-128-CTR Latency Test (Small Blocks)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s\n", "Size(B)", "Latency(us)", "Cycles");
    rt_kprintf("----------------------------------------\n");

    const uint8_t aes_key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };

    const size_t small_sizes[] = {1, 8, 16, 32, 64};
    const int small_iterations = 10000;

    for (int i = 0; i < 5; i++) {
        size_t size = small_sizes[i];

        uint8_t *input = rt_malloc(size);
        uint8_t *output = rt_malloc(size);
        if (input == NULL || output == NULL) {
            if (input) rt_free(input);
            if (output) rt_free(output);
            continue;
        }

        memset(input, 0x5A, size);

        mbedtls_aes_context aes_ctx;
        uint8_t nonce_counter[16] = {0};
        uint8_t stream_block[16] = {0};

        /* 预热 */
        for (int j = 0; j < 100; j++) {
            mbedtls_aes_init(&aes_ctx);
            mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
            size_t nc_off = 0;
            memcpy(nonce_counter, aes_key, 16);
            mbedtls_aes_crypt_ctr(&aes_ctx, size, &nc_off,
                                  nonce_counter, stream_block, input, output);
            mbedtls_aes_free(&aes_ctx);
        }

        /* 延迟测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < small_iterations; iter++) {
            mbedtls_aes_init(&aes_ctx);
            mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
            size_t nc_off = 0;
            memcpy(nonce_counter, aes_key, 16);
            mbedtls_aes_crypt_ctr(&aes_ctx, size, &nc_off,
                                  nonce_counter, stream_block, input, output);
            mbedtls_aes_free(&aes_ctx);
        }

        uint32_t end_cycles = dwt_get_cycles();
        uint32_t elapsed_cycles = end_cycles - start_cycles;

        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);
        uint32_t latency_us = elapsed_us / small_iterations;
        uint32_t cycles_per_op = elapsed_cycles / small_iterations;

        rt_kprintf("%-10d %-12d %-12d\n", size, latency_us, cycles_per_op);

        rt_free(input);
        rt_free(output);
    }
}
#endif /* MBEDTLS_AES_C */

/*===========================================================================
 * 测试菜单
 *===========================================================================*/

static void print_menu(void)
{
    rt_kprintf("\n");
    rt_kprintf("==========================================\n");
    rt_kprintf("    ChaCha20 Performance Test (v4.2.0)\n");
    rt_kprintf("==========================================\n");
    rt_kprintf("  1. Correctness Verification\n");
    rt_kprintf("  2. Throughput Test\n");
    rt_kprintf("  3. Latency Test\n");
    rt_kprintf("  4. Run All Tests\n");
#if defined(MBEDTLS_AES_C)
    rt_kprintf("  5. AES-128-CTR Throughput (Comparison)\n");
    rt_kprintf("  6. AES-128-CTR Latency (Comparison)\n");
#endif
    rt_kprintf("==========================================\n");
}

/*===========================================================================
 * 公共接口
 *===========================================================================*/

void chacha20_demo_run(void)
{
    rt_kprintf("ChaCha20 Performance Test Example (MbedTLS 4.2.0)\n");
    rt_kprintf("System Clock: %d Hz\n", SystemCoreClock);
    rt_kprintf("RT-Thread Tick: %d Hz\n", RT_TICK_PER_SECOND);
    rt_kprintf("\nType 'chacha20_test' to run tests\n");

    print_menu();
}

/**
 * @brief ChaCha20 性能测试命令
 */
static int chacha20_test(int argc, char **argv)
{
    if (argc < 2) {
        print_menu();
        return 0;
    }

    int choice = atoi(argv[1]);

    switch (choice) {
    case 1:
        verify_chacha20_correctness();
        break;
    case 2:
        test_chacha20_throughput();
        break;
    case 3:
        test_chacha20_latency();
        break;
    case 4:
        verify_chacha20_correctness();
        test_chacha20_throughput();
        test_chacha20_latency();
        break;
#if defined(MBEDTLS_AES_C)
    case 5:
        test_aes_throughput();
        break;
    case 6:
        test_aes_latency();
        break;
#endif
    default:
        print_menu();
        break;
    }

    return 0;
}

/* 注册为 finsh 命令 */
MSH_CMD_EXPORT(chacha20_test, ChaCha20 performance test (MbedTLS 4.2.0));

int chacha20_test_register(void)
{
    /* 命令已通过 MSH_CMD_EXPORT 自动注册 */
    return 0;
}
