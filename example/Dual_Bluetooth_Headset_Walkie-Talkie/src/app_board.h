/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BOARD_H
#define APP_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

#define APP_HEADSET_COUNT 2U

app_role_t app_board_init(void);
void app_board_set_headset_connected(uint8_t index, bool connected);

#endif
