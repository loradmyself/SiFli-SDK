/*
 * SPDX-FileCopyrightText: 2024 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief ChaCha20 性能测试
 *
 * 测试指标:
 * 1. 吞吐量 (Throughput) - MB/s
 * 2. 每字节周期数 (Cycles/Byte)
 * 3. 延迟 (Latency) - 微秒
 * 4. 内存占用 (Memory Usage)
 * 5. AES-128-CTR 对比测试
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <stdlib.h>
#include "core_cm33.h"

#ifdef PKG_USING_MBEDTLS
#include <mbedtls/chacha20.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/poly1305.h>
#include <mbedtls/aes.h>

/*===========================================================================
 * 性能测量工具
 *===========================================================================*/

/* 系统时钟频率 (从启动日志可知为 240MHz) */
#define SYS_CLOCK_HZ    SystemCoreClock

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

/**
 * @brief 获取微秒级时间戳
 */
static inline uint32_t get_us(void)
{
    return dwt_get_cycles() / (SYS_CLOCK_HZ / 1000000);
}

/**
 * @brief 获取毫秒级时间戳
 */
static inline uint32_t get_ms(void)
{
    return dwt_get_cycles() / (SYS_CLOCK_HZ / 1000);
}

/*===========================================================================
 * 测试数据
 *===========================================================================*/

/* 测试密钥 (256-bit) - RFC 7539 测试向量 */
static const uint8_t test_key[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 测试 Nonce (96-bit) - RFC 7539 测试向量 */
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
#define HUGE_ITERATIONS 10      /* 大数据块迭代次数 (256KB+) */

/*===========================================================================
 * ChaCha20 吞吐量测试
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20 加密吞吐量
 */
static void test_chacha20_throughput(void)
{
    dwt_init();  /* 初始化 DWT 周期计数器 */
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Throughput Test\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s %-12s\n", "Size(B)", "MB/s", "Cycles/B", "Time(us)");
    rt_kprintf("----------------------------------------\n");

    for (int i = 0; i < NUM_TEST_SIZES; i++) {
        size_t size = test_sizes[i];
        int iterations;
        if (size <= 1024) {
            iterations = TEST_ITERATIONS;
        } else if (size <= 65536) {
            iterations = LARGE_ITERATIONS;
        } else {
            iterations = 20;  /* 128KB: 20 iterations */
        }

        uint8_t *input = rt_malloc(size);
        uint8_t *output = rt_malloc(size);
        if (input == NULL || output == NULL) {
            rt_kprintf("Memory allocation failed for size %d\n", size);
            if (input) rt_free(input);
            if (output) rt_free(output);
            continue;
        }

        /* 填充测试数据 */
        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        /* 预热 */
        for (int j = 0; j < 10; j++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        /* 性能测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < iterations; iter++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        uint32_t end_cycles = dwt_get_cycles();
        uint32_t elapsed_cycles = end_cycles - start_cycles;

        /* 计算总处理数据量 */
        uint64_t total_bytes = (uint64_t)size * iterations;

        /* 吞吐量 (MB/s) */
        uint64_t bytes_per_sec = (uint64_t)total_bytes * SYS_CLOCK_HZ / elapsed_cycles;
        uint32_t throughput_mbps = (uint32_t)(bytes_per_sec / (1024 * 1024));

        /* 每字节周期数 */
        uint32_t cycles_per_byte = (uint32_t)(elapsed_cycles / total_bytes);

        /* 总耗时 (微秒) */
        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);

        rt_kprintf("%-10d %-12d %-12d %-12d\n",
                   size, throughput_mbps, cycles_per_byte, elapsed_us);

        rt_free(input);
        rt_free(output);
    }
}

/*===========================================================================
 * ChaCha20 延迟测试 (小数据块)
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20 小数据块延迟
 */
static void test_chacha20_latency(void)
{
    dwt_init();  /* 初始化 DWT 周期计数器 */
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Latency Test (Small Blocks)\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s\n", "Size(B)", "Latency(us)", "Cycles");
    rt_kprintf("----------------------------------------\n");

    /* 小数据块测试 */
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

        /* 预热 */
        for (int j = 0; j < 100; j++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        /* 延迟测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < small_iterations; iter++) {
            mbedtls_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        uint32_t end_cycles = dwt_get_cycles();
        uint32_t elapsed_cycles = end_cycles - start_cycles;

        /* 计算延迟 */
        uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);
        uint32_t latency_us = elapsed_us / small_iterations;
        uint32_t cycles_per_op = elapsed_cycles / small_iterations;

        rt_kprintf("%-10d %-12d %-12d\n", size, latency_us, cycles_per_op);

        rt_free(input);
        rt_free(output);
    }
}

/*===========================================================================
 * ChaCha20-Poly1305 AEAD 测试
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20-Poly1305 AEAD 吞吐量
 */
static void test_chachapoly_throughput(void)
{
    dwt_init();  /* 初始化 DWT 周期计数器 */
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20-Poly1305 AEAD Throughput Test\n");
    rt_kprintf("========================================\n");
    rt_kprintf("%-10s %-12s %-12s %-12s\n", "Size(B)", "MB/s", "Cycles/B", "Time(us)");
    rt_kprintf("----------------------------------------\n");

    for (int i = 0; i < NUM_TEST_SIZES; i++) {
        size_t size = test_sizes[i];
        int iterations;
        if (size <= 1024) {
            iterations = TEST_ITERATIONS;
        } else if (size <= 65536) {
            iterations = LARGE_ITERATIONS;
        } else {
            iterations = 20;  /* 128KB: 20 iterations */
        }

        uint8_t *input = rt_malloc(size);
        uint8_t *output = rt_malloc(size);
        uint8_t tag[16];

        if (input == NULL || output == NULL) {
            if (input) rt_free(input);
            if (output) rt_free(output);
            continue;
        }

        /* 填充测试数据 */
        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        /* 预热 */
        mbedtls_chachapoly_context ctx;
        mbedtls_chachapoly_init(&ctx);
        mbedtls_chachapoly_setkey(&ctx, test_key);
        mbedtls_chachapoly_encrypt_and_tag(&ctx, size, test_nonce,
                                           NULL, 0, input, output, tag);
        mbedtls_chachapoly_free(&ctx);

        /* 性能测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < iterations; iter++) {
            mbedtls_chachapoly_init(&ctx);
            mbedtls_chachapoly_setkey(&ctx, test_key);
            mbedtls_chachapoly_encrypt_and_tag(&ctx, size, test_nonce,
                                               NULL, 0, input, output, tag);
            mbedtls_chachapoly_free(&ctx);
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

/*===========================================================================
 * AES-128-CTR 对比测试
 *===========================================================================*/

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

    /* AES-128 密钥 */
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
            iterations = 20;
        }

        uint8_t *input = rt_malloc(size);
        uint8_t *output = rt_malloc(size);
        if (input == NULL || output == NULL) {
            if (input) rt_free(input);
            if (output) rt_free(output);
            continue;
        }

        /* 填充测试数据 */
        for (size_t j = 0; j < size; j++) {
            input[j] = j & 0xFF;
        }

        mbedtls_aes_context aes_ctx;
        uint8_t nonce_counter[16] = {0};  /* IV + Counter */
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

/*===========================================================================
 * AES-128-CTR 延迟测试
 *===========================================================================*/

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

/*===========================================================================
 * 内存使用测试
 *===========================================================================*/

/**
 * @brief 测试 ChaCha20 内存使用
 */
static void test_chacha20_memory(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Memory Usage Test\n");
    rt_kprintf("========================================\n");

    /* 上下文结构体大小 (静态) */
    rt_kprintf("Static Memory (ROM):\n");
    rt_kprintf("  mbedtls_chacha20_context:   %d bytes\n", (int)sizeof(mbedtls_chacha20_context));
    rt_kprintf("  mbedtls_chachapoly_context: %d bytes\n", (int)sizeof(mbedtls_chachapoly_context));

    /* 动态内存测试 */
    rt_uint32_t total1, used1, max1;
    rt_memory_info(&total1, &used1, &max1);

    /* 分配上下文 */
    mbedtls_chacha20_context *ctx = rt_malloc(sizeof(mbedtls_chacha20_context));
    if (ctx) {
        rt_uint32_t total2, used2, max2;
        rt_memory_info(&total2, &used2, &max2);
        rt_kprintf("\nDynamic Allocation (Heap):\n");
        rt_kprintf("  ChaCha20 context malloc: %d bytes\n", used2 - used1);
        rt_free(ctx);
    }

    /* ROM 使用 (从 MbedTLS 源码) */
    rt_kprintf("\nCode Size (approximate):\n");
    rt_kprintf("  chacha20.c:    ~1.5 KB\n");
    rt_kprintf("  chachapoly.c:  ~0.8 KB\n");
    rt_kprintf("  poly1305.c:    ~1.2 KB\n");
    rt_kprintf("  Total ROM:     ~3.5 KB\n");
}

/*===========================================================================
 * 正确性验证
 *===========================================================================*/

/**
 * @brief 验证 ChaCha20 加解密正确性
 * @note 使用 MbedTLS self-test 测试向量 (与 chacha20.c 中的 self_test 一致)
 */
static int verify_chacha20_correctness(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  ChaCha20 Correctness Verification\n");
    rt_kprintf("========================================\n");

    /*
     * MbedTLS self-test vector (chacha20.c test 0)
     * 与 MbedTLS 内部 mbedtls_chacha20_self_test() 使用相同的测试向量
     * Key:     全零 (32 bytes)
     * Nonce:   全零 (12 bytes)
     * Counter: 0
     * Input:   全零 (64 bytes)
     */
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

    /* RFC 7539 Section 2.3.2: 全零输入的 ChaCha20 输出 */
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
        rt_kprintf("Expected: ");
        for (int i = 0; i < 64; i++) {
            rt_kprintf("%02x", expected[i]);
            if ((i + 1) % 16 == 0) rt_kprintf("\n          ");
        }
        rt_kprintf("\nGot:      ");
        for (int i = 0; i < 64; i++) {
            rt_kprintf("%02x", output[i]);
            if ((i + 1) % 16 == 0) rt_kprintf("\n          ");
        }
        rt_kprintf("\n");
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

    /* 测试自检 */
    ret = mbedtls_chacha20_self_test(1);
    if (ret != 0) {
        rt_kprintf("❌ ChaCha20 self-test failed!\n");
        return -1;
    }
    rt_kprintf("✅ ChaCha20 self-test: PASSED\n");

    return 0;
}

/*===========================================================================
 * 测试菜单
 *===========================================================================*/

static void print_menu(void)
{
    rt_kprintf("\n");
    rt_kprintf("==========================================\n");
    rt_kprintf("    ChaCha20 Performance Test\n");
    rt_kprintf("==========================================\n");
    rt_kprintf("  1. Correctness Verification\n");
    rt_kprintf("  2. Throughput Test\n");
    rt_kprintf("  3. Latency Test\n");
    rt_kprintf("  4. ChaCha20-Poly1305 AEAD Test\n");
    rt_kprintf("  5. Memory Usage Test\n");
    rt_kprintf("  6. Run All Tests\n");
    rt_kprintf("  7. AES-128-CTR Throughput (Comparison)\n");
    rt_kprintf("  8. AES-128-CTR Latency (Comparison)\n");
    rt_kprintf("==========================================\n");
}

/**
 * @brief ChaCha20 性能测试命令
 */
static int chacha20_perf(int argc, char **argv)
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
        test_chachapoly_throughput();
        break;
    case 5:
        test_chacha20_memory();
        break;
    case 6:
        verify_chacha20_correctness();
        test_chacha20_throughput();
        test_chacha20_latency();
        test_chachapoly_throughput();
        test_chacha20_memory();
        break;
    case 7:
        test_aes_throughput();
        break;
    case 8:
        test_aes_latency();
        break;
    default:
        print_menu();
        break;
    }

    return 0;
}

/* 注册为 finsh 命令 */
MSH_CMD_EXPORT(chacha20_perf, ChaCha20 performance test);

/*===========================================================================
 * 主函数
 *===========================================================================*/

int main(void)
{
    rt_kprintf("ChaCha20 Performance Test Example\n");
    rt_kprintf("System Clock: %d Hz\n", SystemCoreClock);
    rt_kprintf("RT-Thread Tick: %d Hz\n", RT_TICK_PER_SECOND);
    rt_kprintf("\nType 'chacha20_perf' to run tests\n");
    rt_kprintf("  1-5: Individual tests\n");
    rt_kprintf("  6:   All ChaCha20 tests\n");
    rt_kprintf("  7:   AES-128-CTR Throughput\n");
    rt_kprintf("  8:   AES-128-CTR Latency\n");

    while (1) {
        rt_thread_mdelay(1000);
    }

    return 0;
}

#endif /* PKG_USING_MBEDTLS */
