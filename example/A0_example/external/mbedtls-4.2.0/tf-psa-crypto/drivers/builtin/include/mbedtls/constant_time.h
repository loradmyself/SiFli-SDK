/**
 * \file constant_time.h
 *
 * \brief Constant-time operations for Mbed TLS
 */

#ifndef MBEDTLS_CONSTANT_TIME_H
#define MBEDTLS_CONSTANT_TIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Check if two byte strings are equal in constant time
 *
 * \param a     Pointer to first byte string
 * \param b     Pointer to second byte string
 * \param n     Number of bytes to compare
 *
 * \return      0 if the strings are equal, non-zero otherwise
 */
int mbedtls_ct_memcmp(const void *a, const void *b, size_t n);

/**
 * \brief Check if a byte string is zero in constant time
 *
 * \param buf   Pointer to byte string
 * \param len   Number of bytes to check
 *
 * \return      0 if the string is zero, non-zero otherwise
 */
int mbedtls_ct_is_zero(const void *buf, size_t len);

/**
 * \brief Copy byte string in constant time
 *
 * \param dest  Destination buffer
 * \param src   Source buffer
 * \param n     Number of bytes to copy
 */
void mbedtls_ct_memcpy(void *dest, const void *src, size_t n);

/**
 * \brief Set byte buffer to zero in constant time
 *
 * \param buf   Buffer to zeroize
 * \param len   Number of bytes to zeroize
 */
void mbedtls_ct_memzero(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_CONSTANT_TIME_H */
