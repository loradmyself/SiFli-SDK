/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include "app_types.h"

void app_state_init(app_role_t role);
app_role_t app_state_get_role(void);
const char *app_state_role_name(app_role_t role);
app_mode_t app_state_get_mode(void);
void app_state_set_mode(app_mode_t mode);
const char *app_state_mode_name(app_mode_t mode);

#endif
