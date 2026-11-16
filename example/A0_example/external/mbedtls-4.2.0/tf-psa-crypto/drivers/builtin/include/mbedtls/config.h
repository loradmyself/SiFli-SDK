/**
 * \file config.h
 *
 * \brief Mbed TLS configuration for ChaCha20/AES testing
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ===== Feature flags ===== */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_CIPHER_MODE_CTR

/* ===== Compiler macros ===== */
#if defined(__GNUC__)
#define MBEDTLS_COMPILER_IS_GCC
#endif

/* ===== Return check macros ===== */
#if defined(__GNUC__)
#define MBEDTLS_CHECK_RETURN __attribute__((__warn_unused_result__))
#else
#define MBEDTLS_CHECK_RETURN
#endif
#define MBEDTLS_CHECK_RETURN_TYPICAL
#define MBEDTLS_CHECK_RETURN_CRITICAL MBEDTLS_CHECK_RETURN
#define MBEDTLS_CHECK_RETURN_OPTIONAL
#define MBEDTLS_IGNORE_RETURN(result) ( (void) !( result ) )

/* ===== Platform options ===== */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Disable unneeded features */
#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_CHECK_PARAMS

/* ===== Version ===== */
#define MBEDTLS_VERSION_MAJOR  4
#define MBEDTLS_VERSION_MINOR  2
#define MBEDTLS_VERSION_PATCH  0

#endif /* MBEDTLS_CONFIG_H */
