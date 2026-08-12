/**
 * \file platform_util.c
 *
 * \brief Platform utility function implementations
 */

#include "mbedtls/platform_util.h"
#include <string.h>
#include <stdlib.h>

/**
 * \brief Securely zeroize a buffer
 */
void mbedtls_platform_zeroize(void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) {
        *p++ = 0;
    }
}

/**
 * \brief Securely zeroize and free a buffer
 */
void mbedtls_zeroize_and_free(void *buf, size_t len)
{
    if (buf == NULL) {
        return;
    }
    mbedtls_platform_zeroize(buf, len);
    free(buf);
}
