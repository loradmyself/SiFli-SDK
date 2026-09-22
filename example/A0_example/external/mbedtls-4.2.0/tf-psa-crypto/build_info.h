/**
 * \file build_info.h
 *
 * \brief Build info for tf-psa-crypto (simplified for manual SCons build)
 */

#ifndef TF_PSA_CRYPTO_BUILD_INFO_H
#define TF_PSA_CRYPTO_BUILD_INFO_H

/* Include user configuration */
#include "mbedtls/config.h"

#include <stdint.h>

/* ===== psa_status_t type ===== */
typedef int32_t psa_status_t;

/* ===== PSA Crypto API error codes ===== */
#define PSA_SUCCESS                      ((psa_status_t) 0)
#define PSA_ERROR_GENERIC_ERROR          ((psa_status_t) -132)
#define PSA_ERROR_INVALID_ARGUMENT       ((psa_status_t) -138)
#define PSA_ERROR_INVALID_SIGNATURE      ((psa_status_t) -148)
#define PSA_ERROR_BUFFER_TOO_SMALL       ((psa_status_t) -142)
#define PSA_ERROR_INSUFFICIENT_MEMORY    ((psa_status_t) -141)
#define PSA_ERROR_INSUFFICIENT_DATA      ((psa_status_t) -143)
#define PSA_ERROR_BAD_STATE              ((psa_status_t) -137)
#define PSA_ERROR_NOT_SUPPORTED          ((psa_status_t) -134)

/* ===== Mbed TLS error codes ===== */
#define MBEDTLS_ERR_ERROR_CORRUPTION_DETECTED  (-0x006E)
#define MBEDTLS_ERR_PLATFORM_FEATURE_UNSUPPORTED  (-0x5E80)

#endif /* TF_PSA_CRYPTO_BUILD_INFO_H */
