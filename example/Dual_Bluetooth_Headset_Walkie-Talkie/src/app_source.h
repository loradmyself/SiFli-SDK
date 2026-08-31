/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SOURCE_H
#define APP_SOURCE_H

#include <stdbool.h>

int app_source_init(void);
bool app_source_is_ready(void);
int app_source_play(void);
int app_source_stop(void);
void app_source_print_status(void);

#endif
