/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BT_H
#define APP_BT_H

#include <stdbool.h>
#include <stdint.h>

#include "app_types.h"

int app_bt_init(void);
bool app_bt_is_ready(void);
int app_bt_scan(void);
int app_bt_connect(const char *target);
int app_bt_disconnect(const char *target);
int app_bt_talk(bool start);
int app_bt_set_headset_volume(app_volume_target_t target, uint8_t headset,
                              uint8_t level);
void app_bt_print_volume(void);
void app_bt_print_status(void);

#endif
