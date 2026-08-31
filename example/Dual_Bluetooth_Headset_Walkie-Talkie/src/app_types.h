/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_TYPES_H
#define APP_TYPES_H

#define APP_VOLUME_LEVEL_MAX 15U

typedef enum
{
    APP_ROLE_RELAY = 0,
    APP_ROLE_SOURCE,
    APP_ROLE_COUNT,
} app_role_t;

typedef enum
{
    APP_MODE_IDLE = 0,
    APP_MODE_SCANNING,
    APP_MODE_CONNECTING,
    APP_MODE_MEDIA,
    APP_MODE_TALK_STARTING,
    APP_MODE_TALKING,
    APP_MODE_TALK_STOPPING,
    APP_MODE_SOURCE_PLAYING,
    APP_MODE_ERROR,
    APP_MODE_COUNT,
} app_mode_t;

typedef enum
{
    APP_VOLUME_TARGET_MUSIC = 0,
    APP_VOLUME_TARGET_TALK,
    APP_VOLUME_TARGET_COUNT,
} app_volume_target_t;

#endif
