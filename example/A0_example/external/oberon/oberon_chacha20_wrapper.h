/**
 * @file oberon_chacha20_wrapper.h
 * @brief Oberon ChaCha20 独立实现（带前缀避免冲突）
 */

#ifndef OBERON_CHACHA20_WRAPPER_H
#define OBERON_CHACHA20_WRAPPER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Oberon ChaCha20 上下文 */
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
 * @brief 设置加密密钥
 */
int oberon_chacha20_setkey(oberon_chacha20_context *ctx, const unsigned char key[32]);

/**
 * @brief 设置 nonce 和计数器
 */
int oberon_chacha20_starts(oberon_chacha20_context *ctx, const unsigned char nonce[12], uint32_t counter);

/**
 * @brief 加密/解密数据
 */
int oberon_chacha20_update(oberon_chacha20_context *ctx, size_t size,
                          const unsigned char *input, unsigned char *output);

/**
 * @brief 一次性加密/解密
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

#endif /* OBERON_CHACHA20_WRAPPER_H */
