/**
 * \file platform.h
 *
 * \brief Platform abstraction functions for Mbed TLS
 */

#ifndef MBEDTLS_PLATFORM_H
#define MBEDTLS_PLATFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Allocate memory block
 *
 * \param size  Number of bytes to allocate
 *
 * \return      Pointer to the allocated memory block, or NULL if allocation
 *              failed
 */
void *mbedtls_calloc(size_t n, size_t size);

/**
 * \brief Free memory block
 *
 * \param ptr   Pointer to memory block to be freed
 */
void mbedtls_free(void *ptr);

/**
 * \brief Initialize the platform
 *
 * \return      0 on success, or a negative error code
 */
int mbedtls_platform_init(void);

/**
 * \brief Deinitialize the platform
 */
void mbedtls_platform_free(void);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_PLATFORM_H */
