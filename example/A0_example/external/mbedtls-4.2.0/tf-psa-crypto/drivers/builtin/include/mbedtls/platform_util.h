/**
 * \file platform_util.h
 *
 * \brief Platform utility functions for Mbed TLS
 */

#ifndef MBEDTLS_PLATFORM_UTIL_H
#define MBEDTLS_PLATFORM_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check return macros */
#if !defined(MBEDTLS_CHECK_RETURN)
#if defined(__GNUC__)
#define MBEDTLS_CHECK_RETURN __attribute__((__warn_unused_result__))
#elif defined(_MSC_VER) && _MSC_VER >= 1700
#include <sal.h>
#define MBEDTLS_CHECK_RETURN _Check_return_
#else
#define MBEDTLS_CHECK_RETURN
#endif
#endif

/* Return check levels */
#if defined(MBEDTLS_CHECK_RETURN_WARNING)
#define MBEDTLS_CHECK_RETURN_TYPICAL MBEDTLS_CHECK_RETURN
#else
#define MBEDTLS_CHECK_RETURN_TYPICAL
#endif

#define MBEDTLS_CHECK_RETURN_CRITICAL MBEDTLS_CHECK_RETURN
#define MBEDTLS_CHECK_RETURN_OPTIONAL

/* Ignore return value macro */
#if !defined(MBEDTLS_IGNORE_RETURN)
#define MBEDTLS_IGNORE_RETURN(result) ( (void) !( result ) )
#endif

/**
 * \brief Securely zeroize a buffer
 *
 * \param buf   Buffer to be zeroized
 * \param len   Length of the buffer in bytes
 */
void mbedtls_platform_zeroize(void *buf, size_t len);

/**
 * \brief Securely zeroize and free a buffer
 *
 * \param buf   Buffer to be zeroized and freed
 * \param len   Length of the buffer in bytes
 */
void mbedtls_zeroize_and_free(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_PLATFORM_UTIL_H */
