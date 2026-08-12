/**
 * @file oberon_chacha20.h
 * @brief Oberon ChaCha20 独立实现（oberon_ 前缀）
 */

#ifndef OBERON_CHACHA20_H
#define OBERON_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ChaCha20 常量 */
#define OBERON_CHACHA20_BLOCK_SIZE  64
#define OBERON_CHACHA20_KEY_SIZE    32
#define OBERON_CHACHA20_NONCE_SIZE  12

/* 上下文结构 */
typedef struct {
    uint32_t state[16];
    uint8_t keystream8[64];
    size_t keystream_bytes_used;
} oberon_chacha20_context;

/**
 * @brief 初始化 ChaCha20 上下文
 */
void oberon_chacha20_init(oberon_chacha20_context *ctx);

/**
 * @brief 释放 ChaCha20 上下文
 */
void oberon_chacha20_free(oberon_chacha20_context *ctx);

/**
 * @brief 一次性 ChaCha20 加密/解密
 *
 * @param key       32字节密钥
 * @param nonce     12字节 nonce
 * @param counter   初始计数器值
 * @param size      输入数据长度
 * @param input     输入数据
 * @param output    输出缓冲区
 * @return 0 成功，非0 失败
 */
int oberon_chacha20_crypt(const unsigned char key[32],
                          const unsigned char nonce[12],
                          uint32_t counter,
                          size_t size,
                          const unsigned char *input,
                          unsigned char *output);

#ifdef __cplusplus
}
#endif

#endif /* OBERON_CHACHA20_H */
