/**
 * \file error_common.h
 *
 * \brief Error handling for Mbed TLS
 */

#ifndef MBEDTLS_ERROR_COMMON_H
#define MBEDTLS_ERROR_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Translate an Mbed TLS error code to a string
 *
 * \param error     Error code to translate
 *
 * \return          Pointer to a string describing the error
 */
const char *mbedtls_high_level_strerr(int error);

/**
 * \brief Translate an Mbed TLS error code to a low-level string
 *
 * \param error     Error code to translate
 *
 * \return          Pointer to a string describing the error
 */
const char *mbedtls_low_level_strerr(int error);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_ERROR_COMMON_H */
