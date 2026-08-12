/**
 * @file ChaCha20_oberon.c
 * @brief ChaCha20 性能测试 (Oberon 库)
 *
 * 测试指标:
 * 1. 吞吐量 (Throughput) - MB/s
 * 2. 每字节周期数 (Cycles/Byte)
 * 3. 延迟 (Latency) - 微秒
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <stdlib.h>
#include "core_cm33.h"

/* Oberon ChaCha20 头文件（oberon_ 前缀） */
#include "oberon_chacha20.h"

/*===========================================================================
 * 性能测量工具 (DWT Cycle Counter)
 *===========================================================================*/
#define DWT_CTRL    (*(volatile uint32_t *)0xE0001000)
#define DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004)
#define DEM_CR      (*(volatile uint32_t *)0xE000FC00)
#define DEM_CR_TRCENA   (1 << 24)

#define SYS_CLOCK_HZ    240000000UL  /* 240 MHz */

static void dwt_init(void) {
    DEM_CR |= DEM_CR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= 0x1;
}

static inline uint32_t dwt_get_cycles(void) {
    return DWT_CYCCNT;
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
    int ret;

    /* 测试加密 */
    ret = oberon_chacha20_crypt(key, nonce, 0, 64, plaintext, output);
    if (ret != 0) {
        rt_kprintf("❌ Oberon ChaCha20 encryption failed: %d\n", ret);
        return -1;
    }

    if (memcmp(output, expected, 64) != 0) {
        rt_kprintf("❌ Oberon ChaCha20 encryption output mismatch!\n");
        return -1;
    }

    rt_kprintf("✅ Oberon ChaCha20 encryption: PASS\n");

    /* 测试解密（流密码，加密解密是同一操作） */
    uint8_t decrypted[64];
    ret = oberon_chacha20_crypt(key, nonce, 0, 64, output, decrypted);
    if (ret != 0) {
        rt_kprintf("❌ Oberon ChaCha20 decryption failed: %d\n", ret);
        return -1;
    }

    if (memcmp(decrypted, plaintext, 64) != 0) {
        rt_kprintf("❌ Oberon ChaCha20 decryption output mismatch!\n");
        return -1;
    }

    rt_kprintf("✅ Oberon ChaCha20 decryption: PASS\n");
    rt_kprintf("✅ Oberon ChaCha20 correctness verification passed!\n");

    return 0;
}

/*===========================================================================
 * 吞吐量测试
 *===========================================================================*/
static void test_chacha20_oberon_throughput(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Throughput Test\n");
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
            iterations = HUGE_ITERATIONS;
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
            oberon_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
        }

        /* 性能测试 */
        uint32_t start_cycles = dwt_get_cycles();

        for (int iter = 0; iter < iterations; iter++) {
            oberon_chacha20_crypt(test_key, test_nonce, 0, size, input, output);
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
 * 延迟测试
 *===========================================================================*/
static void test_chacha20_oberon_latency(void)
{
    dwt_init();
    rt_kprintf("\n========================================\n");
    rt_kprintf("  Oberon ChaCha20 Latency Test (64B)\n");
    rt_kprintf("========================================\n");

    const size_t latency_size = 64;
    uint8_t input[64];
    uint8_t output[64];

    for (int i = 0; i < 64; i++) {
        input[i] = i & 0xFF;
    }

    /* 预热 */
    for (int i = 0; i < 100; i++) {
        oberon_chacha20_crypt(test_key, test_nonce, 0, latency_size, input, output);
    }

    /* 测量单次加密延迟 */
    uint32_t start_cycles = dwt_get_cycles();
    oberon_chacha20_crypt(test_key, test_nonce, 0, latency_size, input, output);
    uint32_t end_cycles = dwt_get_cycles();

    uint32_t elapsed_cycles = end_cycles - start_cycles;
    uint32_t elapsed_us = elapsed_cycles / (SYS_CLOCK_HZ / 1000000);

    rt_kprintf("Single 64B encrypt: %d cycles, %d us\n", elapsed_cycles, elapsed_us);
    rt_kprintf("Cycles/byte: %d\n", elapsed_cycles / latency_size);
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
    rt_kprintf("2. Throughput test\n");
    rt_kprintf("3. Latency test\n");
    rt_kprintf("4. Run all tests\n");
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
    default:
        print_oberon_menu();
        break;
    }
}

/* 注册为 finsh 命令 */
MSH_CMD_EXPORT(chacha20_oberon_test, Oberon ChaCha20 performance test);
