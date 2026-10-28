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
bool app_bt_is_link_registered(const unsigned char *mac);
/* True while a link is being actively torn down (suspend-then-AVDTP-
 * disconnect). The share reconciler must not re-start a stream on such a
 * link, or the START races the AVDTP disconnect and re-pauses the audio. */
bool app_bt_is_disconnecting(const unsigned char *mac);
/* Bounce (disconnect + auto-reconnect) a link whose A2DP media path is
 * wedged: streaming at the app but no audio delivered. Rate limited. */
bool app_bt_bounce_media_link(const unsigned char *mac);
bool app_bt_has_phone_media(void);
int app_bt_scan(void);
int app_bt_connect(const char *target);
int app_bt_disconnect(const char *target);
int app_bt_phone_disconnect(void);
int app_bt_unbond(const char *target);
int app_bt_talk(bool start);
int app_bt_set_headset_volume(app_volume_target_t target, uint8_t headset,
                              uint8_t level);
void app_bt_print_volume(void);
void app_bt_print_status(void);

#endif
