/*
 * SPDX-FileCopyrightText: 2024 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief ChaCha20 性能测试主函数
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <stdlib.h>
#include "core_cm33.h"

/* 根据配置包含对应的头文件 */
#if defined(CHACHA20_USE_MBEDTLS_420)
#include "demos/MbedTLS_Test_Demo/ChaCha20_test.h"
#elif defined(CHACHA20_USE_OBERON)
#include "demos/MbedTLS_Test_Demo/ChaCha20_oberon.h"
#endif


int main(void)
{
    rt_kprintf("A0 Example - ChaCha20 Performance Test\n");
    rt_kprintf("System Clock: %d Hz\n", SystemCoreClock);
    rt_kprintf("RT-Thread Tick: %d Hz\n", RT_TICK_PER_SECOND);
    rt_kprintf("\n");

#if defined(CHACHA20_USE_MBEDTLS_420)
    rt_kprintf("Library: MbedTLS 4.2.0\n");
    rt_kprintf("Command: chacha20_test [1-4]\n");
#elif defined(CHACHA20_USE_OBERON)
    rt_kprintf("Library: Oberon PSA Crypto\n");
    rt_kprintf("Command: chacha20_oberon_test [1-4]\n");
#endif
    rt_kprintf("Options: 1=correctness, 2=throughput, 3=latency, 4=all\n");
    rt_kprintf("\n");

    while (1) {
        rt_thread_mdelay(1000);
    }

    return 0;
}
