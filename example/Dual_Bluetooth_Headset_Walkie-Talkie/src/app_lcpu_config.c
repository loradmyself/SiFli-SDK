/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// #include <stdint.h>
// #include <string.h>

// #include "bf0_hal.h"

// #if !defined(SF32LB52X) 
// #error "This application supports the SF32LB52X Nano N4 board only."
// #endif

// static const uint16_t g_app_em_offset[HAL_LCPU_CONFIG_EM_BUF_MAX_NUM] =
// {
//     0x0178, 0x0178, 0x0740, 0x07A0, 0x0848, 0x08B8, 0x0B38, 0x0CE8,
//     0x0E80, 0x1474, 0x14DC, 0x1AF4, 0x22F4, 0x22F4, 0x22F4, 0x22F4,
//     0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x25F4, 0x2614, 0x268C, 0x26DC,
//     0x27FC, 0x2810, 0x2824, 0x2914, 0x2924, 0x29E4, 0x2A00, 0x3A10,
//     0x4E24, 0x5F04,
// };

// void lcpu_rom_config_default(void);

// void lcpu_rom_config(void)
// {
//     hal_lcpu_bluetooth_em_config_t em_offset = {0};
//     hal_lcpu_bluetooth_act_configt_t activity = {0};
//     hal_lcpu_bluetooth_rom_config_t config = {0};
//     HAL_StatusTypeDef status;

//     lcpu_rom_config_default();

//     memcpy(em_offset.em_buf, g_app_em_offset, sizeof(g_app_em_offset));
//     em_offset.is_valid = 1U;
//     status = HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_EM_BUF, &em_offset,
//                                  sizeof(em_offset));
//     HAL_ASSERT(status == HAL_OK);

//     activity.bt_max_acl = 3U;
//     activity.bt_max_sco = 2U;
//     activity.ble_max_iso = 0U;
//     activity.bit_valid = (1U << 4) | (1U << 1) | (1U << 0);
//     status = HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_ACT_CFG, &activity,
//                                  sizeof(activity));
//     HAL_ASSERT(status == HAL_OK);

//     config.lld_prog_delay = 3U;
//     config.sco_cfg = 2U;
//     config.bit_valid = (1U << 13) | (1U << 2);
//     status = HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_CONFIG, &config,
//                                  sizeof(config));
//     HAL_ASSERT(status == HAL_OK);
// }
