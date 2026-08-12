/**
 * @file ChaCha20_test.h
 * @brief ChaCha20 性能测试头文件 (MbedTLS 4.2.0)
 */

#ifndef __CHACHA20_TEST_H__
#define __CHACHA20_TEST_H__

#include "rtthread.h"

/**
 * @brief 运行 ChaCha20 性能测试
 *
 * 测试内容:
 * 1. 正确性验证
 * 2. 吞吐量测试 (MB/s)
 * 3. 延迟测试 (微秒)
 * 4. AES-128-CTR 对比测试
 */
void chacha20_demo_run(void);

/**
 * @brief 注册 ChaCha20 测试命令到 finsh
 */
int chacha20_test_register(void);

#endif /* __CHACHA20_TEST_H__ */
