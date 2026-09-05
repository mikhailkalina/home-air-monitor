// SPDX-License-Identifier: Apache-2.0
//
// The single error type crossing every port boundary.
//
// Platform-specific codes (esp_err_t, errno, SDL_GetError) are mapped to
// hal_status_t inside the adapter and never reach core/.

#ifndef HAC_PORTS_HAL_STATUS_H
#define HAC_PORTS_HAL_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

// HAL_OK is guaranteed to be 0, so `if (status != HAL_OK)` and a plain
// truthiness test agree. Every other code is non-zero.
typedef enum {
    HAL_OK = 0,
    HAL_ERR_TIMEOUT,
    HAL_ERR_IO,
    HAL_ERR_NOT_FOUND,
    HAL_ERR_INVALID_ARG,
    HAL_ERR_NO_MEM,
    HAL_ERR_UNSUPPORTED,
    HAL_ERR_BUSY,
    HAL_ERR_NOT_READY,
} hal_status_t;

#ifdef __cplusplus
}
#endif

#endif  // HAC_PORTS_HAL_STATUS_H
