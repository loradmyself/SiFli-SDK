# MbedTLS 使用指南

本文档提供了在 SiFli SDK 中使用 MbedTLS 的完整指南，包括架构概述、配置方法、文件加密/解密示例以及验证方法。

## 1. 概述

SiFli SDK 包含**两个独立的 MbedTLS 发行版**，用于不同的应用场景：

| 特性 | Boot 版 (`external/mbedtls/`) | 完整版 (`external/mbedtls_228/`) |
|------|-------------------------------|----------------------------------|
| 版本 | 2.6.0 | 2.28.1 |
| Kconfig | `PKG_SIFLI_MBEDTLS_BOOT` | `PKG_USING_MBEDTLS` |
| 用途 | 安全启动、DFU、固件验证 | TLS网络、WiFi、HTTPS |
| SSL/TLS | 不支持 | TLS 1.0-1.2, DTLS |
| 对称加密 | AES (CBC/CTR), CMAC | AES, DES, ARC4, Camellia, Blowfish, XTEA, GCM, CCM |
| 哈希 | SHA-256, SHA-512 | MD5, SHA-1, SHA-256/384/512, RIPEMD-160 |
| 非对称 | RSA, SM2/SM3 (可选) | RSA, ECDH, ECDSA, DHM, ECP |
| 硬件加速 | 否 | 是 (通过 hwcrypto `_ALT` 模块) |
| 证书 | 否 | 是 (12个内置根CA + 用户证书) |

### 1.1 Boot 版 (MbedTLS 2.6.0)

位于 `external/mbedtls/`，这是一个精简版本，主要用于：
- **安全启动**: 使用 RSA 公钥验证固件签名
- **DFU (设备固件更新)**: SHA-256 镜像哈希验证和 AES 解密加密固件
- **SM2/SM3 支持**: 可选的中国国密算法

关键源文件：
- `middleware/dfu/dfu_sec.c` - DFU 安全模块
- `middleware/boot/secboot.c` - 安全启动模块

### 1.2 完整版 (MbedTLS 2.28.1)

位于 `external/mbedtls_228/`，提供完整的 TLS/加密功能：
- **TLS 客户端/服务器**: 完整的 TLS 1.0-1.2 和 DTLS 支持
- **WiFi 安全**: WPA supplicant 集成
- **HTTPS**: WebClient 和 MicroPython SSL 模块
- **硬件加速**: AES、SHA、RSA 通过 RT-Thread hwcrypto API

## 2. 配置方法

### 2.1 启用 MbedTLS

在项目的 `proj.conf` 中添加以下配置：

**启用 Boot 版 (用于 DFU/安全启动):**
```
PKG_SIFLI_MBEDTLS_BOOT=y
```

**启用完整版 (用于 TLS/网络):**
```
PKG_USING_MBEDTLS=y
```

```{note}
两个版本互斥。启用 `PKG_USING_MBEDTLS` 将禁用 `PKG_SIFLI_MBEDTLS_BOOT`。
```

### 2.2 Kconfig 选项

**Boot 版选项:**
- `PKG_SIFLI_MBEDTLS_BOOT` - 启用精简版 MbedTLS
- `PKG_USING_SM` - 启用 SM2/SM3 国密算法 (依赖 `PKG_SIFLI_MBEDTLS_BOOT`)

**完整版选项:**
- `PKG_USING_MBEDTLS` - 主开关
- `PKG_USING_MBEDTLS_DEBUG` - 启用调试日志
- `PKG_USING_MBEDTLS_USE_ALL_CERTS` - 打包所有默认 CA 证书
- `PKG_USING_MBEDTLS_USER_CERTS` - 允许用户自定义证书

### 2.3 硬件加速

完整版通过 `_ALT` 模块支持硬件加速。启用对应的 RT-Thread hwcrypto 选项：

```
RT_HWCRYPTO_USING_AES=y      # AES 硬件加速
RT_HWCRYPTO_USING_SHA2_256=y  # SHA-256 硬件加速
RT_HWCRYPTO_USING_BIGNUM=y    # RSA 硬件加速
```

## 3. 文件加密/解密

### 3.1 AES-256-CBC 加密/解密示例

以下是完整的 AES-256-CBC 加密和解密示例：

```c
#include <mbedtls/aes.h>
#include <string.h>
#include <rtthread.h>

#define BLOCK_SIZE 16

/**
 * AES-256-CBC 加密
 * @param key       32字节加密密钥
 * @param iv        16字节初始化向量 (会被修改)
 * @param input     明文数据
 * @param input_len 明文长度
 * @param output    密文缓冲区 (必须是16字节的倍数)
 * @return 成功返回0，失败返回负错误码
 */
int aes_encrypt(const uint8_t *key, uint8_t *iv,
                const uint8_t *input, size_t input_len,
                uint8_t *output)
{
    mbedtls_aes_context aes;
    int ret;

    mbedtls_aes_init(&aes);

    /* 设置加密密钥 (256-bit) */
    ret = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (ret != 0) {
        rt_kprintf("setkey_enc failed: -0x%04x\n", -ret);
        goto cleanup;
    }

    /* PKCS7 填充 */
    size_t padded_len = ((input_len / BLOCK_SIZE) + 1) * BLOCK_SIZE;
    uint8_t *padded = rt_malloc(padded_len);
    if (padded == NULL) {
        ret = -1;
        goto cleanup;
    }

    memcpy(padded, input, input_len);
    uint8_t pad_val = padded_len - input_len;
    memset(padded + input_len, pad_val, pad_val);

    /* CBC 加密 */
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                 padded_len, iv, padded, output);
    rt_free(padded);

    if (ret != 0) {
        rt_kprintf("aes_crypt_cbc encrypt failed: -0x%04x\n", -ret);
    }

cleanup:
    mbedtls_aes_free(&aes);
    return ret;
}

/**
 * AES-256-CBC 解密
 * @param key       32字节解密密钥
 * @param iv        16字节初始化向量 (会被修改)
 * @param input     密文数据
 * @param input_len 密文长度 (必须是16字节的倍数)
 * @param output    明文缓冲区
 * @return 成功返回0，失败返回负错误码
 */
int aes_decrypt(const uint8_t *key, uint8_t *iv,
                const uint8_t *input, size_t input_len,
                uint8_t *output)
{
    mbedtls_aes_context aes;
    int ret;

    mbedtls_aes_init(&aes);

    /* 设置解密密钥 (256-bit) */
    ret = mbedtls_aes_setkey_dec(&aes, key, 256);
    if (ret != 0) {
        rt_kprintf("setkey_dec failed: -0x%04x\n", -ret);
        goto cleanup;
    }

    /* CBC 解密 */
    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                 input_len, iv, input, output);
    if (ret != 0) {
        rt_kprintf("aes_crypt_cbc decrypt failed: -0x%04x\n", -ret);
        goto cleanup;
    }

    /* 去除 PKCS7 填充 */
    if (input_len > 0) {
        uint8_t pad_val = output[input_len - 1];
        if (pad_val > 0 && pad_val <= BLOCK_SIZE) {
            /* 验证填充有效性 */
            for (size_t i = 0; i < pad_val; i++) {
                if (output[input_len - 1 - i] != pad_val) {
                    rt_kprintf("Invalid PKCS7 padding!\n");
                    ret = -1;
                    goto cleanup;
                }
            }
            output[input_len - pad_val] = 0;  /* 添加字符串终止符 */
        }
    }

cleanup:
    mbedtls_aes_free(&aes);
    return ret;
}
```

### 3.2 文件加密示例

```c
#include <dfs_posix.h>
#include <mbedtls/aes.h>

#define BLOCK_SIZE 16
#define READ_BUF_SIZE 4096

/**
 * 使用 AES-256-CBC 加密文件
 * @param input_path    源文件路径
 * @param output_path   目标文件路径
 * @param key           32字节加密密钥
 * @param iv            16字节 IV (会被修改)
 * @return 成功返回0，失败返回负错误码
 */
int encrypt_file(const char *input_path, const char *output_path,
                 const uint8_t *key, uint8_t *iv)
{
    int ret = 0;
    mbedtls_aes_context aes;
    uint8_t *read_buf = NULL;
    uint8_t *write_buf = NULL;
    int fd_in = -1, fd_out = -1;

    fd_in = open(input_path, O_RDONLY, 0);
    if (fd_in < 0) {
        rt_kprintf("Failed to open input file: %s\n", input_path);
        return -1;
    }

    fd_out = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        rt_kprintf("Failed to open output file: %s\n", output_path);
        close(fd_in);
        return -1;
    }

    read_buf = rt_malloc(READ_BUF_SIZE);
    write_buf = rt_malloc(READ_BUF_SIZE + BLOCK_SIZE);
    if (read_buf == NULL || write_buf == NULL) {
        ret = -1;
        goto cleanup;
    }

    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (ret != 0) {
        rt_kprintf("setkey_enc failed: -0x%04x\n", -ret);
        goto cleanup;
    }

    /* 将 IV 写入输出文件头部 */
    write(fd_out, iv, BLOCK_SIZE);

    size_t total_read = 0;
    ssize_t bytes_read;

    while ((bytes_read = read(fd_in, read_buf, READ_BUF_SIZE)) > 0) {
        size_t remaining = bytes_read;

        /* 处理完整块 */
        size_t offset = 0;
        while (remaining >= BLOCK_SIZE) {
            ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                         BLOCK_SIZE, iv,
                                         read_buf + offset, write_buf);
            if (ret != 0) {
                rt_kprintf("Encryption failed: -0x%04x\n", -ret);
                goto cleanup;
            }
            write(fd_out, write_buf, BLOCK_SIZE);
            offset += BLOCK_SIZE;
            remaining -= BLOCK_SIZE;
        }

        /* 处理最后一个不完整的块 (PKCS7 填充) */
        if (remaining > 0 || bytes_read < READ_BUF_SIZE) {
            size_t padded_len = BLOCK_SIZE;
            uint8_t pad_val = BLOCK_SIZE - remaining;

            memset(write_buf, pad_val, padded_len);
            memcpy(write_buf, read_buf + offset, remaining);

            ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                         padded_len, iv, write_buf, write_buf);
            if (ret != 0) {
                rt_kprintf("Encryption failed: -0x%04x\n", -ret);
                goto cleanup;
            }
            write(fd_out, write_buf, padded_len);
        }

        total_read += bytes_read;
    }

    rt_kprintf("File encrypted: %s -> %s (%d bytes)\n",
               input_path, output_path, total_read);

cleanup:
    mbedtls_aes_free(&aes);
    if (read_buf) rt_free(read_buf);
    if (write_buf) rt_free(write_buf);
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0) close(fd_out);
    return ret;
}

/**
 * 使用 AES-256-CBC 解密文件
 * @param input_path    加密文件路径
 * @param output_path   解密文件路径
 * @param key           32字节解密密钥
 * @return 成功返回0，失败返回负错误码
 */
int decrypt_file(const char *input_path, const char *output_path,
                 const uint8_t *key)
{
    int ret = 0;
    mbedtls_aes_context aes;
    uint8_t iv[BLOCK_SIZE];
    uint8_t *read_buf = NULL;
    uint8_t *write_buf = NULL;
    int fd_in = -1, fd_out = -1;

    fd_in = open(input_path, O_RDONLY, 0);
    if (fd_in < 0) {
        rt_kprintf("Failed to open input file: %s\n", input_path);
        return -1;
    }

    fd_out = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        rt_kprintf("Failed to open output file: %s\n", output_path);
        close(fd_in);
        return -1;
    }

    read_buf = rt_malloc(READ_BUF_SIZE + BLOCK_SIZE);
    write_buf = rt_malloc(READ_BUF_SIZE + BLOCK_SIZE);
    if (read_buf == NULL || write_buf == NULL) {
        ret = -1;
        goto cleanup;
    }

    /* 从文件头部读取 IV */
    ssize_t bytes_read = read(fd_in, iv, BLOCK_SIZE);
    if (bytes_read != BLOCK_SIZE) {
        rt_kprintf("Failed to read IV\n");
        ret = -1;
        goto cleanup;
    }

    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_dec(&aes, key, 256);
    if (ret != 0) {
        rt_kprintf("setkey_dec failed: -0x%04x\n", -ret);
        goto cleanup;
    }

    size_t total_read = BLOCK_SIZE;  /* IV 已读取 */

    while ((bytes_read = read(fd_in, read_buf, READ_BUF_SIZE)) > 0) {
        size_t remaining = bytes_read;
        size_t offset = 0;

        while (remaining >= BLOCK_SIZE) {
            ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                         BLOCK_SIZE, iv,
                                         read_buf + offset, write_buf);
            if (ret != 0) {
                rt_kprintf("Decryption failed: -0x%04x\n", -ret);
                goto cleanup;
            }

            /* 检查是否是最后一个块 */
            ssize_t next_read = read(fd_in, read_buf + remaining, BLOCK_SIZE);
            if (next_read < BLOCK_SIZE) {
                /* 最后一个块 - 去除 PKCS7 填充 */
                uint8_t pad_val = write_buf[BLOCK_SIZE - 1];
                if (pad_val > 0 && pad_val <= BLOCK_SIZE) {
                    /* 验证填充 */
                    int valid = 1;
                    for (size_t i = 0; i < pad_val; i++) {
                        if (write_buf[BLOCK_SIZE - 1 - i] != pad_val) {
                            valid = 0;
                            break;
                        }
                    }
                    if (valid) {
                        write(fd_out, write_buf, BLOCK_SIZE - pad_val);
                    } else {
                        rt_kprintf("Invalid PKCS7 padding!\n");
                        ret = -1;
                        goto cleanup;
                    }
                } else {
                    rt_kprintf("Invalid PKCS7 padding!\n");
                    ret = -1;
                    goto cleanup;
                }
            } else {
                write(fd_out, write_buf, BLOCK_SIZE);
            }

            offset += BLOCK_SIZE;
            remaining -= BLOCK_SIZE;
        }

        total_read += bytes_read;
    }

    rt_kprintf("File decrypted: %s -> %s\n", input_path, output_path);

cleanup:
    mbedtls_aes_free(&aes);
    if (read_buf) rt_free(read_buf);
    if (write_buf) rt_free(write_buf);
    if (fd_in >= 0) close(fd_in);
    if (fd_out >= 0) close(fd_out);
    return ret;
}
```

## 4. 验证方法

### 4.1 基本验证测试

```c
#include <mbedtls/aes.h>
#include <rtthread.h>

void test_aes_encrypt_decrypt(void)
{
    rt_kprintf("=== AES-256-CBC 加密解密测试 ===\n");

    /* 1. 准备测试数据 */
    uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };

    uint8_t iv[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };

    uint8_t plaintext[] = "SiFli AES Test Data 1234567890!@#$%";
    size_t plaintext_len = strlen((char *)plaintext);

    rt_kprintf("明文 (%d bytes): %s\n", plaintext_len, plaintext);

    /* 2. 加密 */
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, sizeof(iv));

    size_t encrypted_len = ((plaintext_len / 16) + 1) * 16;
    uint8_t *encrypted = rt_malloc(encrypted_len);

    int ret = aes_encrypt(key, iv_copy, plaintext, plaintext_len, encrypted);
    if (ret != 0) {
        rt_kprintf("❌ 加密失败!\n");
        rt_free(encrypted);
        return;
    }

    rt_kprintf("密文 (%d bytes): ", encrypted_len);
    for (size_t i = 0; i < encrypted_len; i++) {
        rt_kprintf("%02x", encrypted[i]);
    }
    rt_kprintf("\n");

    /* 3. 验证密文与明文不同 */
    int same = (memcmp(plaintext, encrypted, plaintext_len) == 0);
    rt_kprintf("密文≠明文验证: %s\n",
               same ? "❌ 失败(密文相同)" : "✅ 通过");

    /* 4. 解密 */
    uint8_t *decrypted = rt_malloc(encrypted_len);
    memcpy(iv_copy, iv, sizeof(iv));

    ret = aes_decrypt(key, iv_copy, encrypted, encrypted_len, decrypted);
    if (ret != 0) {
        rt_kprintf("❌ 解密失败!\n");
        rt_free(encrypted);
        rt_free(decrypted);
        return;
    }

    rt_kprintf("解密后: %s\n", decrypted);

    /* 5. 验证解密结果与原始明文一致 */
    if (memcmp(plaintext, decrypted, plaintext_len) == 0) {
        rt_kprintf("✅ 解密结果与原始明文一致!\n");
    } else {
        rt_kprintf("❌ 解密结果不匹配!\n");
    }

    /* 6. 用错误密钥解密验证失败 */
    uint8_t wrong_key[32];
    memcpy(wrong_key, key, 32);
    wrong_key[0] ^= 0xFF;

    uint8_t *wrong_decrypted = rt_malloc(encrypted_len);
    memcpy(iv_copy, iv, sizeof(iv));
    ret = aes_decrypt(wrong_key, iv_copy, encrypted, encrypted_len, wrong_decrypted);

    if (memcmp(plaintext, wrong_decrypted, plaintext_len) != 0) {
        rt_kprintf("✅ 错误密钥解密结果不同（符合预期）\n");
    } else {
        rt_kprintf("❌ 错误密钥也能解密（严重问题!）\n");
    }

    rt_free(encrypted);
    rt_free(decrypted);
    rt_free(wrong_decrypted);

    rt_kprintf("=== 测试完成 ===\n");
}

/* 注册为 finsh 命令 */
MSH_CMD_EXPORT(test_aes_encrypt_decrypt, test AES encrypt and decrypt);
```

### 4.2 文件验证方法

| 方法 | 说明 |
|------|------|
| **已知答案测试 (KAT)** | 使用 NIST 标准测试向量验证 AES/SHA 实现正确性 |
| **二进制对比** | 使用 hexdump 对比加密文件与原始文件，确认字节完全不同 |
| **文件哈希校验** | 计算原始文件和解密后文件的 SHA-256，哈希值应一致 |
| **长度验证** | 加密后文件大小应是 16 字节的倍数 (CBC 模式) |
| **错误密钥测试** | 用错误密钥解密应得到无意义数据 |

### 4.3 哈希验证示例

```c
#include <mbedtls/sha256.h>
#include <dfs_posix.h>

/**
 * 计算文件的 SHA-256 哈希值
 * @param file_path 要计算哈希的文件
 * @param hash      32字节输出缓冲区
 * @return 成功返回0
 */
int calculate_file_hash(const char *file_path, uint8_t *hash)
{
    int fd = open(file_path, O_RDONLY, 0);
    if (fd < 0) {
        rt_kprintf("Failed to open file: %s\n", file_path);
        return -1;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  /* 0 = SHA-256, 不是 SHA-224 */

    uint8_t buf[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buf, sizeof(buf))) > 0) {
        mbedtls_sha256_update(&ctx, buf, bytes_read);
    }

    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    close(fd);

    return 0;
}

/**
 * 验证加密/解密后的文件完整性
 */
void test_file_integrity(void)
{
    uint8_t hash_original[32], hash_decrypted[32];

    rt_kprintf("=== 文件完整性测试 ===\n");

    /* 计算原始文件的哈希 */
    if (calculate_file_hash("/data/original.bin", hash_original) != 0) {
        rt_kprintf("Failed to hash original file\n");
        return;
    }

    /* 加密和解密 */
    uint8_t key[32] = { /* ... 你的密钥 ... */ };
    uint8_t iv[16] = { /* ... 你的 IV ... */ };

    encrypt_file("/data/original.bin", "/data/encrypted.bin", key, iv);
    decrypt_file("/data/encrypted.bin", "/data/decrypted.bin", key);

    /* 计算解密后文件的哈希 */
    if (calculate_file_hash("/data/decrypted.bin", hash_decrypted) != 0) {
        rt_kprintf("Failed to hash decrypted file\n");
        return;
    }

    /* 比较哈希值 */
    if (memcmp(hash_original, hash_decrypted, 32) == 0) {
        rt_kprintf("✅ 文件完整性验证通过 (SHA-256 匹配)\n");
    } else {
        rt_kprintf("❌ 文件完整性验证失败!\n");
    }

    rt_kprintf("Original hash: ");
    for (int i = 0; i < 32; i++) rt_kprintf("%02x", hash_original[i]);
    rt_kprintf("\nDecrypted hash: ");
    for (int i = 0; i < 32; i++) rt_kprintf("%02x", hash_decrypted[i]);
    rt_kprintf("\n");
}

MSH_CMD_EXPORT(test_file_integrity, verify file integrity after encrypt/decrypt);
```

## 5. 关键文件参考

| 文件 | 说明 |
|------|------|
| `external/mbedtls/include/mbedtls/config.h` | Boot 版 MbedTLS 配置 |
| `external/mbedtls_228/ports/inc/tls_config.h` | 完整版 MbedTLS 配置 |
| `external/mbedtls_228/ports/src/tls_client.c` | TLS 客户端实现 |
| `middleware/crypto/sifli_crypto.h` | SiFli 加密中间件 API (AES-CMAC) |
| `middleware/crypto/sifli_crypto_port.c` | 硬件加密端口层 |
| `middleware/dfu/dfu_sec.c` | DFU 安全模块 (使用 MbedTLS) |
| `middleware/boot/secboot.c` | 安全启动模块 (使用 MbedTLS) |

## 6. 完整示例项目

SDK 提供了完整的 MbedTLS 示例项目，位于：

```
example/crypto/mbedtls_aes/
├── README.md           # 中文文档
├── README_EN.md        # 英文文档
├── project/
│   ├── SConstruct      # 构建脚本
│   ├── SConscript      # 构建配置
│   ├── proj.conf       # 项目配置
│   └── Kconfig         # Kconfig 配置
└── src/
    ├── SConscript      # 源码构建配置
    └── main.c          # 示例代码
```

### 6.1 编译示例

```bash
# 进入示例项目目录
cd example/crypto/mbedtls_aes/project

# 编译 (替换为你的开发板名称)
scons --board=sf32lb52-lcd_n16r8_hcpu -j8
```

### 6.2 运行测试

连接串口终端，在 finsh shell 中执行：

```bash
# 显示测试菜单
mbedtls_test

# 运行 AES 内存加密解密测试
mbedtls_test 1

# 运行文件加密解密测试
mbedtls_test 2

# 运行 SHA-256 哈希测试
mbedtls_test 3

# 运行所有测试
mbedtls_test 4
```

### 6.3 测试输出示例

```
=== AES-256-CBC 内存加密解密测试 ===
明文 (前16字节): 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
密文 (前16字节): a3 f5 8b 2c 7e 9d 1a 4b 6c 8e 2f 5a 3d 7b 9c 1e
密文长度: 272 bytes (明文: 256 bytes)
✅ 密文与明文不同 (正确)
解密长度: 256 bytes
✅ 解密结果与原始明文一致!
✅ 错误密钥解密结果不同 (符合预期)
=== 内存测试完成 ===
```

## 7. 故障排除

### 6.1 常见问题

**问题: `mbedtls/aes.h: No such file or directory`**
- 解决方案: 确保 `proj.conf` 中有 `PKG_USING_MBEDTLS=y` 或 `PKG_SIFLI_MBEDTLS_BOOT=y`

**问题: 链接器报错 `_mbedtls_*` 函数未定义**
- 解决方案: 检查对应的 Kconfig 选项是否已启用

**问题: 解密产生乱码数据**
- 检查: 加密和解密必须使用相同的 IV
- 检查: 密钥必须与加密时使用的一致
- 检查: 密文长度必须是 16 字节的倍数

**问题: Invalid PKCS7 padding 错误**
- 检查: 文件在存储/传输过程中是否损坏
- 检查: 是否使用了正确的密钥

### 6.2 调试模式

启用 MbedTLS 调试日志：

```
PKG_USING_MBEDTLS_DEBUG=y
```

然后使用 MbedTLS 调试回调：

```c
#include <mbedtls/debug.h>

void my_debug(void *ctx, int level,
              const char *file, int line,
              const char *str)
{
    rt_kprintf("mbedtls[%d]: %s:%04d: %s", level, file, line, str);
}

/* 设置调试回调 */
mbedtls_ssl_conf_dbg(&conf, my_debug, NULL);
mbedtls_debug_set_threshold(4);  /* 最大调试级别 */
```
