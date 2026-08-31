/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

#include "bf0_hal.h"
#include "drv_io.h"

#include "app_board.h"
#include "app_bt.h"
#include "app_i2s.h"
#include "app_source.h"
#include "app_state.h"

void HAL_MspInit(void)
{
    BSP_IO_Init();
}

int main(void)
{
    int result;
    app_role_t role;

    role = app_board_init();
    app_state_init(role);
    rt_kprintf("[intercom] PA39=%s, role=%s\n",
               (role == APP_ROLE_SOURCE) ? "high" : "low",
               app_state_role_name(role));

    if (role == APP_ROLE_SOURCE)
    {
        result = app_source_init();
        if (result != RT_EOK)
        {
            rt_kprintf("[intercom] source initialization failed: %d\n",
                       result);
            app_state_set_mode(APP_MODE_ERROR);
        }
        else
        {
            rt_kprintf("[intercom] source ready; use 'intercom play'\n");
        }
    }
    else
    {
        rt_kprintf("[intercom] dual-headset relay starting\n");
        result = app_i2s_init();
        if (result != RT_EOK)
        {
            rt_kprintf("[intercom] I2S initialization failed: %d\n", result);
            app_state_set_mode(APP_MODE_ERROR);
        }
        else
        {
            result = app_bt_init();
            if (result != RT_EOK)
            {
                rt_kprintf("[intercom] Bluetooth initialization failed: %d\n",
                           result);
                app_state_set_mode(APP_MODE_ERROR);
            }
            else
            {
                rt_kprintf("[intercom] relay ready; use 'intercom scan'\n");
            }
        }
    }

    rt_kprintf("[intercom] command shell ready; run 'intercom status'\n");
    while (1)
    {
        rt_thread_mdelay(10000);
    }
}
