/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 ******************************************************************************
 * @file   app_comm.c
 * @author Sifli software development team
 ******************************************************************************
 */
/*
 * @attention
 * Copyright (c) 2019 - 2024,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "rtthread.h"
#include "rtdevice.h"
#include "log.h"
#include "rtdbg.h"
#include "lvgl.h"
#include "lv_ext_resource_manager.h"
#include "app_comm.h"
#if !defined(BSP_USING_PC_SIMULATOR)
    #include "drv_touch.h"
#endif
#include "app_module.h"
#include "app_lang.h"
#include "app_nvm_lang_compat.h"
#ifdef BSP_BLE_SIBLES
    #include "ble_nvm.h"
#endif
#if defined(OTA_EXT_V3) && defined(BSP_BLE_SIBLES)
    #include "ota_fwk.h"
#endif
#ifdef APP_TOOL_SUPPORT
    #include "app_tool_comm.h"
#endif
#if (defined(OTA_EXT_V3_NAND) || defined(OTA_EXT_V3_NOR_RAM)) && defined(USING_TF_CARD)
    #include "tf_init.h"
#endif
#if defined(BT_PAN_OTA)
    #include "ota_nvm.h"
#endif

#if defined(DRV_EPIC_NEW_API)
    #include "drv_epic.h"
#endif
#if defined (RT_USING_SENSOR) && (defined (BF0_HCPU) || defined (SENSOR_IN_HCPU) || defined (BSP_USING_PC_SIMULATOR))
    #include "sensor_service.h"
#endif

#ifdef QUICKJS_LVGL
    #include "lvgl_v8_qjs_main.h"
#endif

#ifdef MICROPYTHON_USING_LVGL
    #include "lvgl_v8_mpy_main.h"
#endif
#ifdef RT_USING_LWIP
    #include "lwip/netif.h"
#endif
#if defined(RT_USING_BT)
    #include "bt_comm.h"
    #include "bt_connect.h"
    #include "bt_config.h"
#endif

static const char          *reg_main_name;
static struct               rt_semaphore pwr_off_sema;
static screen_lock_ctrl_t   screen_lock = {15000, true};
static screen_light_ctrl_t  screen_light = {0};
#ifdef __RELEASE__
    static bool             is_release_mode = true;
#else
    static bool             is_release_mode = false;
#endif

#ifndef BSP_USING_PC_SIMULATOR
    extern void             close_display(void);
    extern void             open_display(void);
#endif

#if !defined(MENU_FRAMEWORK)
    #define app_menu_list_get_num()   0
#endif

#if 0
/*
 * SDK multilingual migration note:
 * The original solution-side app_comm common services are kept here intact as
 * a reference baseline, but they are not compiled in the SDK yet. The current
 * step only enables locale/language-pack related functionality.
 */

/**
  * @brief  Turn off LCD, LCD power off.
  */
void app_lcd_off(void)
{
#if !defined(BSP_USING_PC_SIMULATOR)
    close_display();
#endif
}

/**
  * @brief  Turn on LCD, LCD power on.
  */
void app_lcd_on(void)
{
#if !defined(BSP_USING_PC_SIMULATOR)
    open_display();
#endif
}

/**
 * @brief  Notify LCPU, the HCPU has been powered on.
           In dual core (non 52x), due to the need for low battery detection on the HCPU during power on,
           the battery measurement of the LCPU is opened here..
 */
void app_lcpu_pwr_on(void)
{
    //send power on msg to LCPU. LCPU will open sensor, start sensor related data collection and algorithm
    ipc_send_event_to_lcpu(LCPU_PWR_ON);

    //If it is not in the same core, the battery measurement on the LCPU needs to wait for the low battery detection of the HCPU
#if defined(BSP_USING_CHARGER)
    battery_service_init();
#endif
}

/**
 * @brief  Notify LCPU, the HCPU is about to shutdown.
 * @param  type Interface of type app_lcpu_t.
 */
void app_lcpu_pwr_off(uint16_t type)
{
    //send power off msg to lcpu
    ipc_send_event_to_lcpu(type);
    //waiting lcpu close done.
    rt_err_t ret = rt_sem_take(&pwr_off_sema, RT_WAITING_FOREVER);
    LOG_I("%s: ret %d", __func__, ret);
}

/**
 * @brief  After notify LCPU when the HCPU is about to shutdown, waiting for LCPU response.
 */
void app_lcpu_pwr_off_cb(void)
{
    //send power off msg to LCPU. end sensor related data collection and algorithm
    rt_sem_release(&pwr_off_sema);
}

/**
 * @brief  Obtain relevant information about the device
 * @param  device_info Pointer to device information
 */
void app_device_info_get(device_info_t *device_info)
{
    char year[5] = {0};
    char mon[4]  = {0};
    char day[3]  = {0};

    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

#ifdef BSP_BLE_SIBLES
    uint8_t *addr = nvm_ble_get(mac_addr);

    strncpy((char *)&device_info->device_name[0], WATCH_DEVICE_NAME, sizeof(device_info->device_name) - 1);
    device_info->device_name[sizeof(device_info->device_name) - 1] = 0;
    strncpy((char *)&device_info->device_cpu[0], CPU_MODEL, sizeof(device_info->device_cpu) - 1);
    device_info->device_cpu[sizeof(device_info->device_cpu) - 1] = 0;
    strncpy((char *)&device_info->sw_version[0], PRODUCT_SOFTWARE_VER, sizeof(device_info->sw_version) - 1);
    device_info->sw_version[sizeof(device_info->sw_version) - 1] = 0;
    strncpy((char *)&device_info->device_mac[0], (char *)addr, sizeof(device_info->device_mac) - 1);
    device_info->device_mac[sizeof(device_info->device_mac) - 1] = 0;
#endif

    char *date = __DATE__;

    strncpy(mon, date, sizeof(mon) - 1);
    mon[sizeof(mon) - 1] = 0;
    strncpy(day, date + 4, sizeof(day) - 1);
    day[sizeof(day) - 1] = 0;
    strncpy(year, date + 7, sizeof(year) - 1);
    year[sizeof(year) - 1] = 0;

    if (strncmp(mon, months[0], sizeof(mon)) == 0)
    {
        strcpy(mon, "01");
    }
    else if (strncmp(mon, months[1], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "02");
    }
    else if (strncmp(mon, months[2], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "03");
    }
    else if (strncmp(mon, months[3], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "04");
    }
    else if (strncmp(mon, months[4], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "05");
    }
    else if (strncmp(mon, months[5], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "06");
    }
    else if (strncmp(mon, months[6], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "07");
    }
    else if (strncmp(mon, months[7], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "08");
    }
    else if (strncmp(mon, months[8], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "09");
    }
    else if (strncmp(mon, months[9], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "10");
    }
    else if (strncmp(mon, months[10], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "11");
    }
    else if (strncmp(mon, months[11], sizeof(mon) - 1) == 0)
    {
        strcpy(mon, "12");
    }

    strcpy((char *)&device_info->build_date, year);
    strcat((char *)&device_info->build_date, "-");
    strcat((char *)&device_info->build_date, mon);
    strcat((char *)&device_info->build_date, "-");
    strcat((char *)&device_info->build_date, day);
    strcat((char *)&device_info->build_date, "_");
    strcat((char *)&device_info->build_date, __TIME__);

    //nvm_sys_get(device_info) = device_info;
}

int pwr_off_sema_init(void)
{
    LOG_I("%s", __func__);
    RT_ASSERT(RT_EOK == rt_sem_init(&pwr_off_sema, "power_off", 0, RT_IPC_FLAG_FIFO));
    return 0;
}
INIT_PRE_APP_EXPORT(pwr_off_sema_init);

/**
 * @brief  Determine if the lock screen time has timed out
 * @retval bool Timeout or not
 */
bool app_screen_lock_time_is_end(void)
{
    if (monkey_mode()) return false;

    if (screen_lock.enable &&
            screen_lock.idle_time_limit <= lv_disp_get_inactive_time(NULL))
    {
        return true;
    }
    return false;
}

/**
 * @brief  Set whether to enable the lock screen function
 */
void app_screen_lock_enable(bool enable)
{
    LOG_I("%s enable %d", __func__, enable);
    screen_lock.enable = enable;
    if (enable)
        lv_disp_trig_activity(NULL);
}

/**
 * @brief  Set lock screen time length and store it in NVM.
 * @param  ms Lock screen time length, unit ms
 * @retval uint32_t Previous lock screen time length before setting
 */
uint32_t app_screen_lock_time_set(uint32_t ms)
{
    uint32_t pre_limit = screen_lock.idle_time_limit;
    LOG_I("%s: lock time %d", __func__, ms);
    lv_disp_trig_activity(NULL);
    screen_lock.idle_time_limit = ms;
    nvm_sys_update(lock_time, ms, 0);
    return pre_limit;
}

/**
 * @brief  Temporarily set lock screen time length, not store it in NVM.
 * @param  ms Lock screen time length, unit ms
 * @retval uint32_t Previous lock screen time length before setting
 */
uint32_t app_screen_lock_time_temp_set(uint32_t ms)
{
    uint32_t pre_limit = screen_lock.idle_time_limit;
    LOG_I("%s: lock time %d", __func__, ms);
    lv_disp_trig_activity(NULL);
    screen_lock.idle_time_limit = ms;
    return pre_limit;
}

/**
 * @brief  Update system time.
 * @param  time New system time
 */
void app_sys_time_update(sys_time_t *time)
{
    //update rtc
    service_sys_time_set(time);
    ipc_send_msg_to_lcpu(LCPU_TIME_SET, time, sizeof(sys_time_t));
}

#if !defined(BSP_USING_PC_SIMULATOR)
/**
 * @brief  Reset the system clock, the system clock will be restored to its default state.
 */
void app_sys_time_reset(void)
{
    uint32_t mask = 0;
    uint32_t pmu_wakeup_src = pm_get_pwron_wakeup_src();

#ifndef SOC_SF32LB55X
    mask = PMUC_WSR_WDT1 | PMUC_WSR_WDT2 | PMUC_WSR_PWRKEY;
#endif
    mask |= PMUC_WSR_IWDT;

    if (pmu_wakeup_src & mask)
    {
        return;
    }

    if (PM_COLD_BOOT == SystemPowerOnModeGet())
    {
        sys_time_t default_time = {0};
        default_time.year = SIFLI_DEFAULT_YEAR;
        default_time.month = SIFLI_DEFAULT_MON;
        default_time.day = SIFLI_DEFAULT_DAY;
        default_time.hour = SIFLI_DEFAULT_HOUR;
        default_time.min = SIFLI_DEFAULT_MIN;
        default_time.second = SIFLI_DEFAULT_SECOND;
        default_time.zone = SIFLI_DEFAULT_TIMEZONE;
        app_sys_time_update(&default_time);
    }
}
#endif

/**
 * @brief  Update system time using time_t's time stamp.
 * @param  timestamp New system time
 */
void app_sys_timestamp_update(time_t timestamp)
{
    sys_time_t init_time;
    time_t raw_time = timestamp + 28800;    //加8小时
    struct tm *p_info = localtime(&raw_time);

    init_time.year = p_info->tm_year + 1900;
    init_time.month = p_info->tm_mon + 1;
    init_time.day = p_info->tm_mday;
    init_time.hour = p_info->tm_hour;
    init_time.min = p_info->tm_min;
    init_time.second = p_info->tm_sec;
    app_sys_time_update(&init_time);
}

#if defined (HINDI_LANG_SUPPORT) || defined (HINDI_LANG_PREV_CONVERT_SUPPORT)
static const char *_hindi_name = "hindi";

/**
 * @brief  Open hindi(Devanagari) with font name Devanagari _hindi_name.
           HINDI_LANG_PREV_CONVERT_SUPPORT: Static text uses pre conversion to improve display speed
 */
void app_hindi_font_active(void)
{
    hindi_shaper_init(_hindi_name, true);
}

/**
 * @brief  close hindi(Devanagari) font.
 */
void app_hindi_font_deactive(void)
{
    hindi_shaper_deinit();
}
#endif

/**
 * @brief  Clean TP buffer in driver.
 */
void app_tp_buffer_clean(void)
{
#if !defined(BSP_USING_PC_SIMULATOR)
    rt_device_t touch_device = rt_device_find("touch");
    if (touch_device)
    {
        struct touch_message touch_data;
        while (rt_device_read(touch_device, 0, &touch_data, 1));
    }
#endif
}

/**
 * @brief  Enable TP wakeup function or not.
           Due to tp pin maybe placed in LCPU, so need send to LCPU.
 * @param  enable Enable or not.
 */
void app_tp_wakeup_enable(uint8_t enable)
{
#if defined(TOUCH_WAKEUP_SUPPORT)
    LOG_I("app_tp_wakeup_enable!");
    uint16_t msg_id = enable ? LCPU_TP_WAKEUP_ENABLE_REQ : LCPU_TP_WAKEUP_DISABLE_REQ;
    ipc_send_msg_to_lcpu(msg_id, NULL, 0);
    drv_tp_set_wakeup_check_enable(enable); //H side
#endif
    nvm_sys_update(tp_wakeup_enable, enable, 0);
}

/**
 * @brief  Clean frame buffer.
 */
void app_frame_buffer_clean(void)
{
    lv_disp_draw_buf_t *disp_buf = lv_disp_get_draw_buf(lv_disp_get_default());
    rt_kprintf("%s: %d\n", __func__, disp_buf->size * LV_COLOR_DEPTH / 8);
    memset(disp_buf->buf_act, 0x00, disp_buf->size * LV_COLOR_DEPTH / 8);
}
#endif

/**
 * @brief  Initalize local language.
           1) Get language list(ex_lang) from flash's language packet if exist.
           2) Set default language.
 */
void app_locale_lang_init(void)
{
#if !defined (BSP_USING_PC_SIMULATOR) && defined (RT_USING_DFS)
    //Load multi-languages from LANG_INSTALLER_PATH of file_system.
    app_lang_load_pack_list(LANG_INSTALLER_PATH);
#endif

    //use chinese temporarily. it will updated when read nvm.
    //const char *locale = "简体中文";

#if !defined (BSP_USING_PC_SIMULATOR) && defined (RT_USING_DFS)
    //get locale language from flash.
    app_lang_install_pack(LANG_INSTALLER_PATH, nvm_sys_get(locale_lang));
#endif

    //set locale as current language.
    lv_res_t res = lv_ext_set_locale(NULL, nvm_sys_get(locale_lang));
#ifdef APP_TOOL_SUPPORT
    sfat_set_lang_type(nvm_sys_get(locale_lang));
#endif
}


/**
 * @brief  Updated and set local language.
 * @param  locale Locale language name.
 */
void app_locale_lang_update(const char *locale)
{
    LOG_I("%s: %s", __func__, locale);

    if (NULL == locale)
    {
        //if language can't find , set the first language to default.
        locale = lv_i18n_lang_pack[0]->locale;
    }
#if !defined (BSP_USING_PC_SIMULATOR) && defined (RT_USING_DFS)
    app_lang_install_pack(LANG_INSTALLER_PATH, locale);
#endif
    lv_res_t res = lv_ext_set_locale(NULL, locale);
    nvm_sys_copy_update(locale_lang, (void *)locale, MAX_LANG_NAME_LEN, 0);
#ifdef APP_SETTING_USED
    extern void setting_lang_update(void);
    setting_lang_update();
#endif
#ifdef APP_DLMODULE_APP_USED
    extern void dynamic_app_lang_update(void);
    dynamic_app_lang_update();
#endif
#ifdef APP_WF_USED
    wf_lang_update();
#ifdef APP_DLMODULE_WF_USED
    extern void dynamic_wf_lang_update(void);
    dynamic_wf_lang_update();
#endif
#endif
#ifdef MENU_FRAMEWORK
    app_menu_lang_update();
#endif

#ifdef APP_TOOL_SUPPORT
    sfat_set_lang_type(locale);
#endif
}

#if 0
/*
 * The remaining original solution-side app_comm services stay here as a
 * preserved reference. They will be enabled in later migration steps when the
 * corresponding SDK-side dependencies are ready.
 */

/**
 * @brief  Set LCD display brightness.
 * @param  level Brightness level.
 */
void app_lcd_backlight_set(uint8_t level)
{
#ifndef BSP_USING_PC_SIMULATOR
    uint16_t brightness;
    rt_device_t bl_device = NULL;
    bl_device = rt_device_find("lcd");
    RT_ASSERT(bl_device);

    int ret = rt_device_open(bl_device, RT_DEVICE_OFLAG_RDWR);
    if (ret == RT_EOK || ret == -RT_EBUSY)
    {
        switch (level)
        {
        case BACKLIGHT_ONE_LEVEL ... BACKLIGHT_TEN_LEVEL:
        {
            brightness = level * 10;
            break;
        }
        default:
        {
            brightness = 70;
            break;
        }
        }
        rt_device_control(bl_device, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);
    }
    nvm_sys_update(lcd_light, level, 0);

    if (bl_device != NULL && ret == RT_EOK)
        rt_device_close(bl_device);
#endif

}

/**
 * @brief  Get the name of currently running application name.
 * @retval string Currently running application name
 */
char *app_current_app_name_get(void)
{
    char *app = NULL;

    lv_timer_mutex_take();
#ifdef GUI_APP_FRAMEWORK
    gui_runing_app_t *active_app = app_schedule_get_active();
    if (active_app)
        app = (char *)&active_app->id[0];
#else
    //TBD...
#endif
    lv_timer_mutex_rel();

    return app;
}

/**
 * @brief  Enable watchdog or not.
 * @param  enable Enable watchdog or not.
 */
void app_watchdog_enable(uint8_t enable)
{
    LOG_I("%s: enable %d", __func__, enable);
    service_watchdog_enable_set(enable);
    nvm_sys_update(watchdog_enable, enable, 0);
    if (enable)
        ipc_send_event_to_lcpu(LCPU_WATCHDOG_ON);
    else
        ipc_send_event_to_lcpu(LCPU_WATCHDOG_OFF);
}

/**
 * @brief  Pet watchdog to avoid watchdog timeout.
 */
void app_watchdog_pet(void)
{
    service_watchdog_pet();
}

/**
 * @brief  Set OTA status and save it.
 * @param  state OTA status.
 * @retval rt_err_t Set status result
 */
rt_err_t app_ota_status_set(uint16_t state)
{
#if (defined (OTA_EXT_V3_NAND) || defined(OTA_EXT_V3_NOR_RAM))&& !defined(BSP_USING_PC_SIMULATOR)
    ota_set_res_status(state);
//    int ret = rt_flash_config_write(FACTORY_CFG_ID_OTA, (uint8_t *)&state, sizeof(state));
//    if (ret == 0)
//    {
//        LOG_E("%s: write failed! state %d", __func__, state);
//        RT_ASSERT(ret > 0);
//        return RT_ERROR;
//    }
#endif
    return RT_EOK;
}

/**
 * @brief  Get OTA status.
 * @retval uint16_t Current OTA state.
 */
uint16_t app_ota_status_get(void)
{
    uint16_t state = 0;
#if (defined (OTA_EXT_V3_NAND) || defined(OTA_EXT_V3_NOR_RAM))&& !defined(BSP_USING_PC_SIMULATOR)
    state = ota_get_res_status();
    state = state ? (state | OTA_BLE_MASK) : 0;
//    int ret = rt_flash_config_read(FACTORY_CFG_ID_OTA, (uint8_t *)&state, sizeof(state));
//    LOG_I("%s: state %d, ret %d", __func__, state, ret);
//    if (0 == ret)
//    {
//        state = 0;
//    }
#else
    //TBD...
#endif
#if (defined(OTA_EXT_V3_NAND) || defined(OTA_EXT_V3_NOR_RAM)) && defined(USING_TF_CARD)
    if (!state)
    {
        state = (TF_OTA_MOVING_ERROR == dfs_check_res_integrity()) ? RT_TRUE : RT_FALSE;
        state = state ? (state | OTA_TF_MASK) : 0;
    }
#endif
#if defined(BT_PAN_OTA)
    if (!state)
    {
        state = ota_ext_get_status();
        state = state ? (state | OTA_PAN_MASK) : 0;
    }
#endif
    return state;
}

/**
 * @brief  Get version mode, release mode or debug mode.
           For release mode, some exception can't be asserted
           For debug mode, watchdog maybe close.
 * @retval int 0: debug mode; 1: release mode.
 */
int app_version_is_release_mode(void)
{
    return is_release_mode;
}

/**
 * @brief  Set version mode.
 * @param  mode Version mode, 0: debug mode; 1: release mode.
 */
void app_version_release_mode_set(bool mode)
{
    is_release_mode = mode;
}

/**
 * @brief  Waiting GPU done.
           It will be called when power_off animation procedure.
 */
void app_gpu_wait_done(void)
{
    //wait gpu done, because power_off_anim open a new high priority thread to draw gif.
#if !defined(DRV_EPIC_NEW_API)
    lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    if (NULL == disp) return;
    /*Flush the rendered content to the display*/
    lv_draw_ctx_t *draw_ctx = disp->driver->draw_ctx;
    if (draw_ctx->wait_for_finish) draw_ctx->wait_for_finish(draw_ctx);
#else
    while (drv_epic_is_busy())
    {
        rt_thread_mdelay(1);
    }
#endif
}

/**
 * @brief  Convert time to format string. hour : minute: second with format %02d:%02d:%02d
 * @retval string time string with %02d:%02d:%02d (hour : minute: second).
 */
char *app_convert_time_formats_str(bool need_sec)
{
    static char time_str[32];
    uint8_t len = 0, hour = 0;
    time_data_t *current_time = service_current_time_get(false);

    //setting_sys_user_t *sys_user = (setting_sys_user_t *)app_db_get_setting_data(BLE_APP_SETTING_SYS_USER);
    time_str[0] = 0;

    if (0) //(HOUR_MODE_12H == sys_user->sys_hour_mode)//default hour 24H mode
    {
        hour = (current_time->hour % 12) == 0 ? 12 : (current_time->hour % 12);
    }
    else
    {
        hour = current_time->hour;
    }

    len = strlen(time_str);
    snprintf(time_str + len, sizeof(time_str) - 1, "%02d:%02d",
             hour, current_time->min);

    if (need_sec)
    {
        len = strlen(time_str);
        snprintf(time_str + len, sizeof(time_str) - 1, ":%02d", current_time->second);
    }
    if (0) //(HOUR_MODE_12H == sys_user->sys_hour_mode)//default hour 24H mode
    {
        len = strlen(time_str);
        snprintf(time_str + len, sizeof(time_str) - 1, " %s", current_time->hour >= 12 ? "PM" : "AM");
    }

    return time_str;
}

/**
 * @brief  Initialize font, deal ota abnoraml.
 */
void app_font_open(void)
{
#if defined (LV_USING_FREETYPE_ENGINE)
    /* load all ft lib and ft size */
    lvsf_font_load();
    /* open freetype */
    if (0 == app_ota_status_get())
    {
        lv_freetype_open_font(false);
    }
    else
    {
        lv_freetype_open_indicated_font(OTA_ERR_FONT);
    }
#endif
}

/**
 * @brief  check file is valid or not.
 */
bool app_file_check_valid(const char *path)
{
    struct dfs_fd fd;
    bool ret = true;

    if (NULL == path || 0 == strlen(path)) return false;
    LOG_I("%s: %s", __func__, path);

#ifdef RT_USING_DFS
    if (dfs_file_open(&fd, path, O_RDONLY) != 0)
    {
        LOG_I("Open file %s failed!", path);
        return false;
    }
    dfs_file_close(&fd);
#else
    ret = false;
#endif

    return ret;
}

/**
 * @brief  check path is valid or not.
 */
bool app_path_check_valid(const char *path)
{
    bool ret = true;
    LOG_I("%s: %s", __func__, path);

    if (0 == strlen(path)) return false;
#ifdef RT_USING_DFS
    struct stat fileStat;
    ret = (stat(path, &fileStat) == 0) && S_ISDIR(fileStat.st_mode);
#else
    ret = false;
#endif

    return ret;
}

size_t app_get_file_size(const char *file)
{
    size_t file_size = 0;
    if (!file) return 0;
    struct stat statbuf;
    if (0 == dfs_file_stat(file, &statbuf))
    {
        file_size = statbuf.st_size;
    }
    return file_size;
}
int app_get_file_open_mode(const char *mode)
{
    int flags = 0;
#ifdef RT_USING_DFS
    flags = O_RDONLY;
    if (!mode) return flags;
    if (0 == strcmp(mode, "rb"))
        flags = O_RDONLY | O_BINARY;
    else if (0 == strcmp(mode, "wb"))
        flags = O_WRONLY | O_BINARY | O_CREAT | O_TRUNC;
    else if (0 == strcmp(mode, "r"))
        flags = O_RDONLY;
    else if (0 == strcmp(mode, "r+"))
        flags = O_RDWR;
    else if (0 == strcmp(mode, "rb+"))
        flags = O_RDWR | O_BINARY;
    else if (0 == strcmp(mode, "rw+"))
        flags = O_RDWR | O_TRUNC;
    else if (0 == strcmp(mode, "w"))
        flags = O_WRONLY | O_TRUNC | O_CREAT;
    else if (0 == strcmp(mode, "w+"))
        flags = O_RDWR | O_TRUNC | O_CREAT;
    else if (0 == strcmp(mode, "a"))
        flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (0 == strcmp(mode, "a+"))
        flags = O_RDWR | O_CREAT | O_APPEND;
    else
        rt_kprintf("unkown open mode %s\n", mode);
#endif
    return flags;
}

/**
 * @brief  check path is sd card or not.
 */
bool app_path_check_is_sd(const char *path)
{
    bool ret = true;
    LOG_I("%s: %s", __func__, path);

#ifdef RT_USING_DFS
    size_t sd_path_len = strlen(SD_ROOT_PATH);
    size_t path_len = strlen(path);
    if (0 == path_len || sd_path_len > path_len) return false;

    if (0 != strncmp(path, SD_ROOT_PATH, sd_path_len))
        ret = false;
#endif

    return ret;
}

/**
 * @brief  Get the value of the O_DIRECTORY macro.
 */
int app_get_o_directory(void)
{
#ifdef RT_USING_DFS
    return O_DIRECTORY;
#else
    return 0;
#endif
}

int app_mkdir(const char *path)
{
    rt_err_t err = RT_EOK;
    char *temp_path = app_calloc(1, strlen(path) + 1);
    RT_ASSERT(temp_path);
    strcpy(temp_path, path);
    char *p, *temp = temp_path;
    temp++;
    while (temp[0] && (p = strchr(temp, '/')))
    {
        char c = p[0];
        char *pos = p;
        p[0] = 0;
        if (0 != access(temp_path, 0) && 0 != mkdir(temp_path, 0))
        {
            LOG_I("%s: mkdir %s failed ", path, temp_path);
            err = RT_ERROR;
            break;
        }
        temp = p + 1;
        pos[0] = c;
    }
    app_free(temp_path);
    return err;
}

/**
 * @brief  Delete qjs application(aod/app/wf).
 * @param  id id to delete.
 * @retval 0 : success, others: failed
 */
int app_qjs_del_app(const char *id)
{
#ifdef QUICKJS_LVGL
    const char *path;
    char *dir;
    if (!id || !id[0]) return -1;
    if (!strncmp(id, QJS_AOD_PREFIX, strlen(QJS_AOD_PREFIX)))
    {
        path = QJS_AOD_PATH;
    }
    else if (!strncmp(id, QJS_WF_PREFIX, strlen(QJS_WF_PREFIX)))
    {
        path = QJS_WF_PATH;
    }
    else if (!strncmp(id, QJS_APP_PREFIX, strlen(QJS_APP_PREFIX)))
    {
        path = QJS_APP_PATH;
    }
    else
    {
        return -2;
    }
    dir = app_malloc(strlen(path) + strlen(id) + 2);
    if (!dir)
    {
        return -3;
    }
    strcpy(dir, path);
    strcat(dir, id);
#if defined (RT_USING_DFS) && defined (RT_USING_MODULE)
    apm_del_path(dir);
#endif
    LOG_I("%s: del %s!", __func__, id);
    app_free(dir);
    return 0;
#else
    return -1;
#endif
}

/**
 * @brief  Delete qjs application(aod/app/wf).
 * @param  id id to delete.
 * @retval 0 : success, others: failed
 */
int app_mpy_del_app(const char *id)
{
#ifdef MICROPYTHON_USING_LVGL
    const char *path;
    char *dir;
    if (!id || !id[0]) return -1;
    if (!strncmp(id, MPY_AOD_PREFIX, strlen(MPY_AOD_PREFIX)))
    {
        path = MPY_AOD_PATH;
    }
    else if (!strncmp(id, MPY_WF_PREFIX, strlen(MPY_WF_PREFIX)))
    {
        path = MPY_WF_PATH;
    }
    else if (!strncmp(id, MPY_APP_PREFIX, strlen(MPY_APP_PREFIX)))
    {
        path = MPY_APP_PATH;
    }
    else
    {
        return -2;
    }
    dir = app_malloc(strlen(path) + strlen(id) + 2);
    if (!dir)
    {
        return -3;
    }
    strcpy(dir, path);
    strcat(dir, id);
#if defined (RT_USING_DFS) && defined (RT_USING_MODULE)
    apm_del_path(dir);
#endif
    LOG_I("%s: del %s!", __func__, id);
    app_free(dir);
    return 0;
#else
    return -1;
#endif
}

/**
 * @brief  Delete micropython application(aod/app/wf).
 * @param  id id to delete.
 * @retval 0 : success, others: failed
 */
bool app_is_built_in(const char *thumb)
{
    if (thumb && !strncmp(thumb, BUILT_IN_APP_PATH, strlen(BUILT_IN_APP_PATH)))  //gui tool thumb is null
        return true;
    return false;
}

/**
 * @brief  Retrieve the registered power_on application. This application can be set through app_set_reg_power_on_app or obtained from NVM.
 * @retval The name of power on application
 */
const char *app_get_reg_power_on_app(void)
{
    return nvm_sys_get(power_on_app);
}

/**
 * @brief  Set the power on application through this interface and immediately store it in the NVM.
 *         This application will serve as the first application to be entered after the device power on.
 */
void app_set_reg_power_on_app(const char *power_on_app)
{
    if (strlen(power_on_app) < GUI_APP_NAME_MAX_LEN)
        nvm_sys_copy_update(power_on_app, power_on_app, GUI_APP_NAME_MAX_LEN, 1);
}

/**
 * @brief  Retrieve the registered main application. This application can be set through app_set_reg_main or obtained from NVM.
 * @retval The name of main application
 */
const char *app_get_reg_main_app(void)
{
    return nvm_sys_get(main_app);
}

/**
 * @brief  Set the main application through this interface and immediately store it in the NVM.
 *         Pressing the home key will return to this application.
 */
void app_set_reg_main_app(const char *main_app)
{
    if (strlen(main_app) < GUI_APP_NAME_MAX_LEN)
        nvm_sys_copy_update(main_app, main_app, GUI_APP_NAME_MAX_LEN, 1);
}

/**
 * @brief  Retrieve the registered tlv application. This application can be set through app_set_reg_tlv or obtained from NVM.
 * @retval The name of tlv application
 */
const char *app_get_reg_tlv_app(void)
{
    return nvm_sys_get(tlv_app);
}

/**
 * @brief  Set the tlv application through this interface and immediately store it in the NVM.
 *         When Gui is in the main app, pressing the home key will enterto this application.
 */
void app_set_reg_tlv_app(const char *tlv_app)
{
    if (strlen(tlv_app) < GUI_APP_NAME_MAX_LEN)
        nvm_sys_copy_update(tlv_app, tlv_app, GUI_APP_NAME_MAX_LEN, 1);
}

static int get_app_num_from_builtin_app(char **first_app)
{
    int num = 0;
    int tlv_num = 0;
#ifdef GUI_APP_FRAMEWORK
#ifdef APP_TLV_USED
    tlv_num = tlv_count_get();
#endif
    const builtin_app_desc_t *builtin_app = gui_builtin_app_list_open();
    while (builtin_app)
    {
        /* Count the number of app that are not Main. */
        if (strcmp(builtin_app->id, "aod_main")  &&                 /*Exclude the AOD */
                strcmp(builtin_app->id, "OTA")  &&                      /*Exclude the OTA */
                strcmp(builtin_app->id, "Main") &&                      /*Exclude the Main */
                (strcmp(builtin_app->id, "Tileview") || 0 < tlv_num))   /*Exclude Tlv_Fwk enable but no tlv application */
        {
            if (first_app && !*first_app) *first_app = (char *) builtin_app->id;        /* first app */
            rt_kprintf("%s: builtin_app %s\n", __func__, builtin_app->id);
            num++;
        }

        builtin_app = (builtin_app_desc_t *) gui_builtin_app_list_get_next(builtin_app);
    }
#endif
    return num;
}

static int serach_app_from_builtin_app(const char *search_app)
{
    int search_result = 0;
    int tlv_num = 0;
#ifdef GUI_APP_FRAMEWORK
#ifdef APP_TLV_USED
    tlv_num = tlv_count_get();
#endif
    const builtin_app_desc_t *builtin_app = gui_builtin_app_list_open();
    while (builtin_app)
    {
        /* Search for whether find_app exists. */
        if (search_app &&
                0 == strcmp(builtin_app->id, search_app)  &&                           /* If find find_app */
                strcmp(builtin_app->id, "aod_main")  &&                                /* Exclude the AOD application. aod_main is not a application */
                (strcmp(builtin_app->id, "Main") || 0 < app_menu_list_get_num()) &&    /* If not Main or no Main menu exist */
                (strcmp(builtin_app->id, "Tileview") || 0 < tlv_num))                  /* Tlv_Fwk enable and had at least one tlv application */
        {
            search_result = 1;                                                          /* Found it */
            break;
        }

        builtin_app = (builtin_app_desc_t *) gui_builtin_app_list_get_next(builtin_app);
    }
#endif
    return search_result;
}

/**
 * @brief  Retrieve the first app that launches on startup.
 *         First, try to obtain the application stored in NVM.
 *         If it is unsuccessful and there is no built-in application,
 *         then try to obtain an external application as the first application to start up.
 */
void app_get_poweron_app(void)
{
    const char *reg_name = NULL;
    const char *run_name = NULL;
    char *first_app = NULL;
    int dyn_num = 0;
    int app_tool_num = 0;
    uint8_t check_tlv = 0;
    int app_num = get_app_num_from_builtin_app(&first_app);

    /* Get dynamic_app num */
#ifdef APP_DLMODULE_APP_USED
    dyn_num = dynamic_app_get_cnt();
#endif
#ifdef APP_TOOL_SUPPORT
    /* Get Gui_builder_app num */
    builtin_app_desc_t *too_app = NULL;
    while ((too_app = sfat_manager_get_app_next(too_app, 0))) app_tool_num++;;
#endif

    rt_kprintf("%s: app_num %d dyn_num %d app_tool_num %d\n", __func__, app_num, dyn_num, app_tool_num);
#ifndef _CONTAINER_
_CHECK_APP:
#endif
    do
    {
        /* First, confirm whether it should enter the guide. */
#if defined (GUI_APP_FRAMEWORK) && defined (POWER_ON_GUIDE)
        if (POWER_ON_BY_RESET == nvm_sys_get(power_on_reason))
        {
            run_name = "Guide";
            /* Guide is always in builtin_app */
            if (serach_app_from_builtin_app(run_name)) break;                                                   /* Found it */
        }
#endif

        /* Read reg main app from NVM */
        reg_name = run_name = 0 == check_tlv ? app_get_reg_power_on_app() : app_get_reg_tlv_app();

        rt_kprintf("%s: reg_name %s, check_tlv %d\n", __func__, reg_name, check_tlv);

        /* If there is a registered main application, Try to search for it from the existing applications. */
        if (strcmp(run_name, "NULL") && !(0 == check_tlv && 0 == strcmp(run_name, "Main") && app_num <= 1))
        {
            /* Try to search for it in the application list. If it can't be found, only the default application can be launched. */
            /* 1. Try to search it in builtin_app_list */
            if (serach_app_from_builtin_app(run_name)) break;                                                   /* Found it */
#ifdef APP_DLMODULE_APP_USED
            /* 2. Try to search it in dynamic_app_list */
            if (dyn_num && dynamic_app_list_get_node(run_name)) break;                                          /* Found it */
#endif
#ifdef APP_TOOL_SUPPORT
            /* 3. Try to search it in tool_app_list */
            if (app_tool_num && sfat_manager_get_node_by_id((char *) run_name, SFAT_MANAGER_TYPE_APP)) break;   /* Found it */
#endif
        }

        /* If there is no registered main application, obtain the main application from the existing applications. */
        if (0 == app_num)                                                                                       /* no builtin_app */
        {
#ifdef APP_DLMODULE_APP_USED
            if (dyn_num &&
                    (run_name = dynamic_app_list_get_node(NULL) ? dynamic_app_list_get_node(NULL)->desc.id : NULL))
                break;                                                                                          /* Found it */
#endif
#ifdef APP_TOOL_SUPPORT
            if (app_tool_num &&
                    (run_name = ((builtin_app_desc_t *) sfat_manager_get_app_next(NULL, 0))->id)) break;           /* Found it */
#endif
        }
#if defined (BSP_USING_PC_SIMULATOR)
        else if (1 == app_num && first_app)                                                                     /* only one builtin_app */
        {
            run_name = first_app;                                                                               /* select first application as run_app */
            break;
        }
#endif

#ifdef GUI_APP_FRAMEWORK
        run_name = check_tlv == 0 ? "Main" : "Tileview";
        /*Try to search it in builtin_app_list*/
        if (serach_app_from_builtin_app(run_name)) break;                                                        /* Found it */
        if (first_app) run_name = first_app;                                                                     /* select first application as run_app */
#endif

    }
    while (0);

    rt_kprintf("%s: reg_name %s run_name %s\n", __func__, reg_name, run_name);

    if (!run_name) run_name = check_tlv == 0 ? "Main" : "Tileview";

    if (0 == check_tlv)
    {
#if APP_TOOL_SIMU_DEBUG == 1    //
        builtin_app_desc_t *app = sfat_manager_get_app_next(NULL, 0);
        if (RT_EOK != gui_app_run(app->id))
#elif APP_TOOL_SIMU_DEBUG == 2
        sfat_manager_node_t *sfat_node = sfat_manager_get_active_node(SFAT_MANAGER_TYPE_WF);
        wf_set_last_id(sfat_node->binary.header.st->id);
        if (RT_EOK != gui_app_run("Tileview"))
#else
        if (RT_EOK !=  gui_app_run(run_name))
#endif
        {
            LOG_I("%s: can't find a invalid application to run!!!", __func__);
            /* The pop-up window reminds that there is no app available to run */
            run_name = "NULL";
        }
#ifndef _CONTAINER_
        check_tlv = 1;
        goto _CHECK_APP;
#endif
    }

#ifdef _CONTAINER_
    /**
        For container projects, the current definition is that they support only one application.
        Therefore, the settings for reg_power_on_app, reg_tlv_app, and reg_main_app need to be configured to the same application.
     */
    app_set_reg_power_on_app(run_name);
    app_set_reg_tlv_app(run_name);
    app_set_reg_main_app(run_name);
#elif 1
    /*always set "Main" application as main app*/
    app_set_reg_main_app("Main");

    /*set valid tlv application*/
    app_set_reg_tlv_app(run_name);
#elif 0
    /**
        20250806. The registration configurations of the three application types — reg_tlv_app, reg_main_app, and reg_power_on_app
        — need to have their setting positions adjusted to within the setting application.
        - Configurations can only be completed manually by operating the setting application.
     */

    /* Save reg_main and reg_tlv in the NVM */
    const char *tlv_name = app_get_reg_tlv_app();
    const char *main_name = app_get_reg_main_app();

    if (run_name != reg_name)
    {
        /* If the running app is different from the reg_app, set the running app as reg_power_on. */
        app_set_reg_power_on_app(run_name);
    }

    /* If there is only one application, then disable the home key.*/
    int total_app_num = app_num + dyn_num + app_tool_num;
    if (1 == total_app_num)
    {
        /* only one application, set it to main_app */
        main_name = tlv_name = run_name;
    }
    else if (1 < total_app_num)   /* more than one application, set "Main" to main_app*/
    {
        main_name = "Main";
#ifdef APP_TLV_USED
        if (NULL == tlv_name) tlv_name = "Tileview";
#endif
    }

    /* If tlv_name changed, save it to NVM */
    if (strcmp(tlv_name, app_get_reg_tlv_app()))
    {
        app_set_reg_tlv_app(tlv_name);
    }

    /* If main_name changed, save it to NVM */
    if (strcmp(main_name, app_get_reg_main_app()))
    {
        app_set_reg_main_app(main_name);
    }

    rt_kprintf("%s: tlv_name %s main_name %s\n", __func__, tlv_name, main_name);
#endif
}

/**
 * This function is only used for Monkey testing and is called by the test_travserse_app function in lv_monkey_port.c.
 */
char *trav_app_for_monkey_test(int type)
{
    char *run_app = NULL;

#ifdef GUI_APP_FRAMEWORK
    int app_num = get_app_num_from_builtin_app(NULL);
    int dyn_num = 0;
    int app_tool_num = 0;
    int total_num = app_num + dyn_num + app_tool_num;

#ifdef APP_TOOL_SUPPORT
    builtin_app_desc_t *app = NULL;
    while ((app = sfat_manager_get_app_next(app, 0))) app_tool_num++;;
#endif
#ifdef APP_DLMODULE_APP_USED
    dyn_num = dynamic_app_get_cnt();
#endif

    static builtin_app_desc_t *builtin_app = NULL, *dyn_app = NULL, *tool_app = NULL;

    if (!builtin_app && !dyn_app && !tool_app)
    {
        builtin_app = (builtin_app_desc_t *)gui_builtin_app_list_open();
        RT_ASSERT(builtin_app);
    }

    //return TLV
    if (1 == type)
    {
        run_app = (char *) app_get_reg_tlv_app();
    }
    //randome select APP
    else if (2 == type)
    {
        uint16_t rand_num = ((rand() + 0x10000) & 0xffff) % total_num;
        uint16_t num = 0;
        if (rand_num < app_num)
        {
            builtin_app = NULL;
            while (++num  < rand_num)
            {
                builtin_app = (builtin_app_desc_t *) gui_builtin_app_list_get_next(builtin_app);
            }
            if (builtin_app) run_app = builtin_app->id;
        }
#ifdef APP_DLMODULE_APP_USED
        else if (rand_num < app_num + dyn_num)
        {
            dyn_app = NULL;
            while (++num  < rand_num - app_num)
            {
                dyn_app = (builtin_app_desc_t *) dynamic_app_list_get_app_next((dyn_app_node_t *) dyn_app);
            }
            if (dyn_app) run_app = dyn_app->id;
        }
#endif
#ifdef APP_TOOL_SUPPORT
        else
        {
            tool_app = NULL;
            while (++num  < rand_num - app_num)
            {
                tool_app = (builtin_app_desc_t *) sfat_manager_get_app_next(tool_app, 0);
            }
            if (tool_app) run_app = tool_app->id;
        }
#endif
    }
    //sequentially select
    else
    {
        int next = 0;
        if (builtin_app)
        {
            if (builtin_app) run_app = builtin_app->id;
            builtin_app = (builtin_app_desc_t *) gui_builtin_app_list_get_next(builtin_app);
            next = 1;
        }
#ifdef APP_DLMODULE_APP_USED
        if (!builtin_app && (dyn_app || next))
        {
            dyn_app = (builtin_app_desc_t *) dynamic_app_list_get_app_next((dyn_app_node_t *) dyn_app);
            if (dyn_app) run_app = dyn_app->id;
            next = 2;
        }
#endif
#ifdef APP_TOOL_SUPPORT
        if (!builtin_app && !dyn_app && (tool_app || next))
        {
            tool_app = sfat_manager_get_app_next(tool_app, 0);
            if (tool_app) run_app = tool_app->id;
        }
#endif
        if (!builtin_app && !dyn_app && !tool_app)
        {
            builtin_app = (builtin_app_desc_t *)gui_builtin_app_list_open();
            if (builtin_app) run_app = builtin_app->id;
        }
    }

    RT_ASSERT(run_app && run_app[0]);

    if (run_app && type < 4 && strcmp(app_schedule_get_active()->id, run_app))
    {
        int ret = gui_app_run(run_app);
        LOG_I("%s: cur %s next %s ret %d", __func__, app_schedule_get_active()->id, run_app, ret);
    }
#endif

    return run_app;
}

/**
 * @brief  Close all external fonts and switch the fonts of all texts back to the initial state.
 */
void app_text_font_reload(void)
{
#ifdef LV_USING_FREETYPE_ENGINE
    rt_list_t *font_list = NULL;
    char *font_name = NULL;

    /* disable font_ex*/
    while ((font_name = lvsf_font_trav_ex(&font_list, 1)))
    {
        lvsf_font_set_enable(font_name, 0);
    }

    /* reload obj font_ex*/
    font_list = NULL;
    while ((font_name = lvsf_font_trav_ex(&font_list, 1)))
    {
        /* 1.Reload all the text fonts in the screen */
        gui_runing_app_t *app = NULL;
        rt_list_t *app_list = NULL;

        /* Find all applications */
        while ((app = gui_app_trav(&app_list)))
        {
            subpage_node_t *page = NULL;
            rt_list_t *page_list = NULL;

            /* Find all subpages for this application*/
            while ((page = gui_app_page_trav(app, &page_list)))
            {
                lv_ext_reload_font((lv_obj_t *)page->scr, font_name);
            }
        }

        /* 2.Reload all the text fonts in the sys layer */
        lv_ext_reload_font(lv_layer_sys(), font_name);

        /* 3.Reload all the text fonts in the top layer */
        lv_ext_reload_font(lv_layer_top(), font_name);
    }
#endif
}

static void app_text_set_font(lv_obj_t *top_obj, const char *new_font)
{
    if (!top_obj || !new_font) return; /* Shouldn't happen */

    uint32_t child_cnt = lv_obj_get_child_cnt(top_obj);
    for (uint32_t i = 0; i < child_cnt; i++)
    {
        lv_obj_t *child = top_obj->spec_attr->children[i];

        app_text_set_font(child, new_font);

        if (lv_obj_check_type(child, &lv_label_class))
        {
            lv_font_t *cur_font = (lv_font_t *)lv_obj_get_style_text_font(child, LV_PART_MAIN);

            /* If the font of the obj has not been set, or the font is the same as the font to be updated,
             *  no processing will be carried out.
             */
            if (!cur_font || 0 == strcmp(cur_font->font_name, new_font)) continue;

            lv_font_t *font = (lv_font_t *)lvsf_get_font_by_name((char *)new_font, lvsf_get_size_from_font(cur_font));
            if (font)
            {
                lv_ext_set_local_text_font(child, font, LV_PART_MAIN | LV_STATE_DEFAULT);
                //LOG_I("font_name %s  %s", font->font_name, lv_label_get_text(child));
            }
        }
    }
}

/**
 * @brief  Update the font of the specified application and page.
 *         If both the application name and the page name are null,
 *         then update all existing texts.
 */
void app_text_font_update(const char *app_id, const char *page_id, const char *new_font)
{
    gui_runing_app_t *app = NULL;
    rt_list_t *app_list = NULL;

    if (!new_font) return;

    /* 1.Set the font of all texts in the screen.*/
    /* Find all subpages for this application*/
    while ((app = gui_app_trav(&app_list)))
    {
        subpage_node_t *page = NULL;
        rt_list_t *page_list = NULL;

        /* If the application name is null, then set the font of all applications on the screen. */
        if (app_id && 0 != strcmp(app_id, app->id)) continue;

        /* Find all subpages for this application*/
        while ((page = gui_app_page_trav(app, &page_list)))
        {
            /* If the page name is null, then set the font of all sub-pages of this application. */
            if (page_id && 0 != strcmp(page_id, page->name)) continue;

            app_text_set_font((lv_obj_t *)page->scr, new_font);
        }
    }

    if (!app_id && !page_id)
    {
        /* 2.Set the font of all texts in the sys layer. */
        app_text_set_font(lv_layer_sys(), new_font);

        /* 3.Set the font of all texts in the top layer. */
        app_text_set_font(lv_layer_top(), new_font);
    }

}

/**
 * @brief  This function is used to remove all occurrences of the substring 'sub' from the string 'src'.
 */
void app_remove_substr(char *str, const char *sub)
{
    if (!str || !sub) return;
    char *p;
    size_t sub_len = strlen(sub);

    /* loop to find if the substring 'sub' exists in the string 'str' */
    while ((p = strstr(str, sub)) != NULL)
    {
        /* use the memmove function to move the characters after 'p' forward by 'sub_len' positions */
        /* overwrite the found substring 'sub' */
        /* the last parameter strlen(p + sub_len) + 1 indicates the number of characters to move, including the string terminator '\0' */
        memmove(p, p + sub_len, strlen(p + sub_len) + 1);
    }
}

int app_mkfs_partition(const char *device_name)
{
#ifdef RT_USING_DFS
    extern const struct dfs_mount_tbl mount_table[];
    int index = 0;
    while (1)
    {
        if (mount_table[index].path == NULL) break;
        {
            if (!strcmp(mount_table[index].device_name, device_name))
            {
                dfs_unmount(mount_table[index].path);
                dfs_mkfs(mount_table[index].filesystemtype, mount_table[index].device_name);
                return 0;
            }
        }
        index++;
    }
#endif
    return -1;
}

void app_del_ext_all(void)
{
#ifdef APP_DLMODULE_APP_USED
    extern void dynamic_app_del_ext_all();
    dynamic_app_del_ext_all();
#endif

#ifdef APP_DLMODULE_WF_USED
    extern void dynamic_wf_del_ext_all();
    dynamic_wf_del_ext_all();
#endif

#ifdef APP_DLMODULE_AOD_USED
    extern void dynamic_aod_del_ext_all();
    dynamic_aod_del_ext_all();
#endif

#ifdef APP_TOOL_SUPPORT
    extern void sfat_manager_remove_all(sfat_manager_type_t);
    sfat_manager_remove_all(SFAT_MANAGER_TYPE_APP);
    sfat_manager_remove_all(SFAT_MANAGER_TYPE_WF);
#endif

#ifdef QUICKJS_LVGL
    gui_qjs_watchface_del_ext_all();
    gui_qjs_app_del_ext_all();
    gui_qjs_aod_del_ext_all();
#endif

#ifdef MICROPYTHON_USING_LVGL
    gui_python_watchface_del_ext_all();
    gui_python_app_del_ext_all();
    gui_python_aod_del_ext_all();
#endif
}

uint8_t app_get_backlight_level_by_lux(uint16_t lux)
{
    uint8_t level;
    if (lux <= 20)
    {
        level = BACKLIGHT_ONE_LEVEL;
    }
    else if (lux <= 50)
    {
        level = BACKLIGHT_TWO_LEVEL;
    }
    else if (lux <= 100)
    {
        level = BACKLIGHT_THREE_LEVEL;
    }
    else if (lux <= 200)
    {
        level = BACKLIGHT_FOUR_LEVEL;
    }
    else if (lux <= 400)
    {
        level = BACKLIGHT_FIVE_LEVEL;
    }
    else if (lux <= 600)
    {
        level = BACKLIGHT_SIX_LEVEL;
    }
    else if (lux <= 1200)
    {
        level = BACKLIGHT_SEVEN_LEVEL;
    }
    else if (lux <= 1600)
    {
        level = BACKLIGHT_EIGHT_LEVEL;
    }
    else if (lux <= 2500)
    {
        level = BACKLIGHT_NINE_LEVEL;
    }
    else
    {
        level = BACKLIGHT_TEN_LEVEL;
    }
    return level;
}

void app_screen_light_adjust_set(uint32_t manual_tick)
{
    screen_light.manual_adj_tick = manual_tick;
}

bool app_screen_light_adjust_is_timeout(void)
{
    if (screen_light.enable && (!screen_light.manual_adj_tick ||
                                screen_light.manual_adj_tick + BACKLIGHT_MANUAL_TIMEOUT <= rt_tick_get_millisecond()))
    {
        return true;
    }
    return false;
}


void app_screen_light_auto_set(uint8_t enable)
{
#if defined (RT_USING_SENSOR) && (defined (BF0_HCPU) || defined (SENSOR_IN_HCPU) || defined (BSP_USING_PC_SIMULATOR))
    screen_light.enable = enable;
    screen_light.manual_adj_tick = 0;
    if (enable)
    {
        app_open_sensors(SENSOR_ASL);
    }
    else
    {
        app_close_sensors(SENSOR_ASL);
    }
    nvm_sys_update(asl_enable, enable, false);
#endif
}

/**
 * @brief  To determine whether solution is builtin_res
 * @retval 0 : success, others: failed
 */
bool solution_is_builtin_res(void)
{
#ifdef SOLUTION_RES_BUILT_IN
    return true;
#else
    return false;
#endif
}

const char *app_get_active_netdev_name(void)
{
#ifdef RT_USING_LWIP
    for (uint8_t i = 4; i >= 1; i--)
    {
        struct netif *netdev = netif_get_by_index(i);
        if (netdev && (netdev->flags & NETIF_FLAG_UP) && (netdev->flags & NETIF_FLAG_LINK_UP))
        {
            return netdev->name;
        }
    }
#endif
    return NULL;
}

bool app_get_netdev_internet_up(const char *name)
{
#ifdef RT_USING_LWIP
    if (!name || !name[0])
    {
        return false;
    }
#ifdef BT_FINSH_PAN
    else if (!strcmp(name, "b0"))
    {
        bt_mac_t mac = app_bt_get_last_slave_addr();
        return BT_STATE_CONNECTED == app_bt_get_profile_state_by_conn_idx(app_bt_get_connindex_by_addr(&mac), BT_PROFILE_PAN);
    }
#endif
#ifdef PKG_USING_WPA_SUPPLICANT
    else if (!strcmp(name, PKG_USING_WPA_NET_NAME))
    {
        return (device_status_get(WIFI_STATUS) & WIFI_CONNECTED) ? true : false;
    }
#endif
#endif
    return false;
}

#if !defined(APP_NETWORK_USED)
void app_network_wakeup(bool wakeup)
{
    return;
}
#endif

#ifdef RT_USING_FINSH
#include <finsh.h>

#if 0
static int lang_update_cb(comm_msg_t *msg)
{
    app_locale_lang_update("English_US");
    return 0;
}

static int cmd_lang_update(void)
{
    send_msg_to_gui_thread(NULL, 0, lang_update_cb, 0, NEED_WAKEUP_UI);
    return 0;
}
MSH_CMD_EXPORT_REL(cmd_lang_update, lang, lang update);
#else
static char g_lang_cmd_locale[MAX_LANG_NAME_LEN];

static void lang_cmd_dump_registered_locales(void)
{
    bool first = true;
    LV_EXT_LANG_PACK_LIST_ITER_DEF(iter);

    rt_kprintf("registered locales: ");
    LV_EXT_LANG_PACK_LIST_ITER(NULL, iter)
    {
        LV_EXT_LANG_PACK_ITER(iter, lang_pack_iter)
        {
            rt_kprintf("%s%s", first ? "" : ", ", LV_EXT_LANG_PACK_ITER_GET_NAME(lang_pack_iter));
            first = false;
        }
    }

    if (first)
    {
        rt_kprintf("(none)");
    }
    rt_kprintf("\n");
}

#if !defined(BSP_USING_PC_SIMULATOR) && defined(RT_USING_DFS)
static void lang_cmd_dump_installer_locales(void)
{
    uint32_t i = 0;
    const char *locale;
    bool first = true;

    app_lang_load_pack_list(LANG_INSTALLER_PATH);

    rt_kprintf("installer locales(%s): ", LANG_INSTALLER_PATH);
    while ((locale = app_lang_pack_iterator(&i)) != NULL)
    {
        rt_kprintf("%s%s", first ? "" : ", ", locale);
        first = false;
    }

    if (first)
    {
        rt_kprintf("(none)");
    }
    rt_kprintf("\n");
}
#endif

static void lang_cmd_print_usage(void)
{
    rt_kprintf("usage:\n");
    rt_kprintf("  lang current        - show current locale\n");
    rt_kprintf("  lang list           - list registered locales in lv_ext_resouce\n");
    rt_kprintf("  lang nvm            - show stored locale in nvm/share_prefs\n");
#if !defined(BSP_USING_PC_SIMULATOR) && defined(RT_USING_DFS)
    rt_kprintf("  lang fs             - scan %s and list installer locales\n", LANG_INSTALLER_PATH);
#endif
    rt_kprintf("  lang <locale>       - switch locale, e.g. lang en_us\n");
}

static int lang_update_cb(comm_msg_t *msg)
{
    const char *locale = g_lang_cmd_locale;

#if !defined(BSP_USING_PC_SIMULATOR) && defined(RT_USING_DFS)
    app_lang_load_pack_list(LANG_INSTALLER_PATH);
#endif

    if ('\0' == locale[0])
    {
        app_locale_lang_update(NULL);
    }
    else
    {
        app_locale_lang_update(locale);
    }

    rt_kprintf("lang switch done, current locale: %s\n", lv_ext_get_locale());
    return 0;
}

static int cmd_lang_update(int argc, char **argv)
{
    if (argc <= 1)
    {
        lang_cmd_print_usage();
        rt_kprintf("current locale: %s\n", lv_ext_get_locale());
        lang_cmd_dump_registered_locales();
        return 0;
    }

    if ((0 == strcmp(argv[1], "current")) || (0 == strcmp(argv[1], "cur")))
    {
        rt_kprintf("current locale: %s\n", lv_ext_get_locale());
        return 0;
    }

    if (0 == strcmp(argv[1], "list"))
    {
        lang_cmd_dump_registered_locales();
        return 0;
    }

    if (0 == strcmp(argv[1], "nvm"))
    {
        rt_kprintf("stored locale: %s\n", nvm_sys_get(locale_lang));
#ifdef BSP_SHARE_PREFS
        rt_kprintf("nvm backend: share_prefs (persistent)\n");
#else
        rt_kprintf("nvm backend: compat cache only (reset will lose it)\n");
#endif
        return 0;
    }

#if !defined(BSP_USING_PC_SIMULATOR) && defined(RT_USING_DFS)
    if ((0 == strcmp(argv[1], "fs")) || (0 == strcmp(argv[1], "scan")))
    {
        lang_cmd_dump_installer_locales();
        return 0;
    }
#endif

    rt_memset(g_lang_cmd_locale, 0, sizeof(g_lang_cmd_locale));
    rt_strncpy(g_lang_cmd_locale, argv[1], sizeof(g_lang_cmd_locale) - 1);
    send_msg_to_gui_thread(NULL, 0, lang_update_cb, 0, NEED_WAKEUP_UI);
    rt_kprintf("lang switch request queued: %s\n", g_lang_cmd_locale);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_lang_update, lang, lang [current|list|nvm|fs|<locale>]: multilingual debug command);
#endif

extern int display_fps_onoff(void);
static int fps_on_cb(comm_msg_t *msg)
{
    display_fps_onoff();
    return 0;
}

static int cmd_fps_on(void)
{
    send_msg_to_gui_thread(NULL, 0, fps_on_cb, 0, NEED_WAKEUP_UI);
    return 0;
}
MSH_CMD_EXPORT_REL(cmd_fps_on, fps, fps: open or close fps log);

static int cmd_watchdog_on(void)
{
    static uint8_t on = 0;
    on = (on + 1) & 0x01;
    app_watchdog_enable(on);
    return 0;
}
MSH_CMD_EXPORT_REL(cmd_watchdog_on, watchdog, watchdog: open or close watchdog log);

static int cmd_sys_reset(void)
{
    app_power_off_action(POWER_OFF_WITH_REBOOT);
    return 0;
}
MSH_CMD_EXPORT_REL(cmd_sys_reset, reset, reset);

static int cmd_log_on(void)
{
    extern void log_pause(rt_bool_t pause);
    static uint8_t onoff = 0;
    onoff = (onoff + 1) & 0x01;
    LOG_I("%s: log_off %d", __func__, onoff);
    log_pause(onoff);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_log_on, log_on, log_on);

static int cmd_screen_lock(void)
{
    static uint8_t onoff = 1;
    onoff = (onoff + 1) & 0x01;
    app_screen_lock_enable(onoff);
    return 0;
}
MSH_CMD_EXPORT_REL(cmd_screen_lock, screen_lock, screen_lock);

#if defined(BSP_USING_SDIO)
#include "bf0_hal_aon.h"
rt_uint32_t *buff_test = RT_NULL;

#define SDIO_TEST_LEN 1024 * 100
void cmd_fs_write_t(char *path, int num)
{
    struct dfs_fd fd_test_sd;
    uint32_t open_time = 0, end_time = 0;
    float test_time = 0.0;
    float speed_test = 0.0;
    //char *buff = app_malloc(SDIO_TEST_LEN);
    memset(buff_test, 0x55, SDIO_TEST_LEN);
    uint32_t write_num = num;
    uint32_t write_byt = write_num * SDIO_TEST_LEN * 8;
    if (dfs_file_open(&fd_test_sd, path, O_RDWR | O_CREAT | O_TRUNC) == 0)
    {
        open_time = HAL_GTIMER_READ();
        while (write_num--)
        {
            dfs_file_write(&fd_test_sd, buff_test, SDIO_TEST_LEN);
        }
        end_time = HAL_GTIMER_READ();
    }
    dfs_file_close(&fd_test_sd);
    test_time = ((end_time - open_time) / HAL_LPTIM_GetFreq()) * 1000 * 1000;
    speed_test = write_byt / test_time;
    rt_kprintf("%s path=%s num=%d MBts testtime=%.6lfuS,speed_test=%.6lfMb/s\n", __func__, path, num, test_time, speed_test);
    //app_free(buff);

}

void cmd_fs_write(int argc, char **argv)
{
    cmd_fs_write_t(argv[1], atoi(argv[2]));

}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_fs_write, __cmd_fs_write, test write speed);

void cmd_fs_read_t(char *path, int num)
{
    struct dfs_fd fd_read;
    uint32_t open_time = 0, end_time = 0;
    float test_time = 0.0;
    float speed_test = 0.0;
    //char *buff = app_malloc(SDIO_TEST_LEN);
    uint32_t read_num = num;
    uint32_t read_byt = read_num * SDIO_TEST_LEN * 8;
    rt_memset(buff_test, 0, SDIO_TEST_LEN);
    if (dfs_file_open(&fd_read, path, O_RDONLY) == 0)
    {
        open_time = HAL_GTIMER_READ();
        while (read_num)
        {
            dfs_file_read(&fd_read, buff_test, SDIO_TEST_LEN);
            read_num--;
        }
        end_time = HAL_GTIMER_READ();
    }
    dfs_file_close(&fd_read);
    test_time = ((end_time - open_time) / HAL_LPTIM_GetFreq()) * 1000 * 1000;
    speed_test = read_byt / test_time;
    rt_kprintf("%s  path=%s num=%d MBts testtime=%.6lfuS,speed_test=%.6lfMb/s\n", __func__, path, num, test_time, speed_test);
    //app_free(buff);
}

void cmd_fs_read(int argc, char **argv)
{
    cmd_fs_read_t(argv[1], atoi(argv[2]));
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_fs_read, __cmd_fs_read, test read speed);
void cmd_fs_read_code_t(char *path, int num)
{
    rt_size_t len = 0;
    //char *buff = app_malloc(SDIO_TEST_LEN);
    uint32_t read_num = num;
    uint32_t read_byt = read_num * SDIO_TEST_LEN * 8;
    rt_memset(buff_test, 0, SDIO_TEST_LEN);
    rt_device_t code_dev = rt_device_find("sd0");
    if (!code_dev) return;
    rt_err_t ret = rt_device_open(code_dev, RT_DEVICE_FLAG_RDWR);
    uint32_t open_time = HAL_GTIMER_READ();
    while (read_num)
    {
        rt_device_read(code_dev, 0, buff_test, (SDIO_TEST_LEN) >> 9);
        read_num--;
    }
    uint32_t end_time = HAL_GTIMER_READ();
    rt_device_close(code_dev);
    float test_time = ((end_time - open_time) / HAL_LPTIM_GetFreq()) * 1000 * 1000;
    float speed_test = read_byt / test_time;
    rt_kprintf("%s path=%s num=%d MBts testtime=%.6lfuS,speed_test=%.6lfMb/s\n", __func__, path, num, test_time, speed_test);
    //app_free(buff);
}

void cmd_fs_read_code(int argc, char **argv)
{
    cmd_fs_read_code_t(argv[1], atoi(argv[2]));
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_fs_read_code, __cmd_fs_read_code, test read speed code);
void cmd_fs_write_code_t(char *path, int num)
{
    rt_size_t len = 0;
    //char *buff = app_malloc(SDIO_TEST_LEN);
    uint32_t read_num = num;
    uint32_t read_byt = read_num * SDIO_TEST_LEN * 8;
    rt_memset(buff_test, 0x55, SDIO_TEST_LEN);
    rt_device_t code_dev = rt_device_find("sd0");
    if (!code_dev) return;
    rt_err_t ret = rt_device_open(code_dev, RT_DEVICE_FLAG_RDWR);
    uint32_t open_time = HAL_GTIMER_READ();
    while (read_num)
    {
        rt_device_write(code_dev, 0, buff_test, (SDIO_TEST_LEN) >> 9);
        read_num--;
    }
    uint32_t end_time = HAL_GTIMER_READ();
    rt_device_close(code_dev);
    float test_time = ((end_time - open_time) / HAL_LPTIM_GetFreq()) * 1000 * 1000;
    float speed_test = read_byt / test_time;
    rt_kprintf("%s path=%s num=%d MBts testtime=%.6lfuS,speed_test=%.6lfMb/s\n", __func__, path, num, test_time, speed_test);
}

void cmd_fs_write_code(int argc, char **argv)
{
    cmd_fs_write_code_t(argv[1], atoi(argv[2]));

}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_fs_write_code, __cmd_fs_write_code, test write speed code);

int cmd_emmc_test_buff(int argc, char **argv)
{
    buff_test = app_malloc(SDIO_TEST_LEN);
    rt_kprintf("%s buff_test=%p\n", __func__, buff_test);
    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_emmc_test_buff, __cmd_emmc_test_buff, cmd emmc tes buff);
int cmd_emmc_test_free_buff(int argc, char **argv)
{
    if (buff_test)app_free(buff_test);
    buff_test = RT_NULL;
    rt_kprintf("%s buff_test=%p\n", __func__, buff_test);
    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_emmc_test_free_buff, __cmd_emmc_test_free_buff, cmd emmc tes free);

rt_err_t            copy(const char *src, const char *dst);
void cmd_emmc_stress(int argc, char **argv)
{
    if (!buff_test) buff_test = app_malloc(SDIO_TEST_LEN);
    for (int i = 1; i < atoi(argv[1]); i++)
    {
        cmd_fs_write_t("/external_sd/1.txt", i);
        cmd_fs_write_t("/external_sd/2.txt", i);

        cmd_fs_read_t("/external_sd/1.txt", i);
        cmd_fs_read_t("/external_sd/2.txt", i);

        copy("/external_sd/1.txt", "/1.txt");
        copy("/external_sd/2.txt", "/2.txt");

        cmd_fs_read_t("/1.txt", i);
        cmd_fs_read_t("/2.txt", i);

        dfs_file_unlink("/1.txt");
        dfs_file_unlink("/2.txt");
    }
    if (buff_test)app_free(buff_test);
    buff_test = RT_NULL;
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_emmc_stress, __cmd_emmc_stress, cmd emmc tes stress);

void cmd_emmc_test_all_write(void)
{
    for (int i = 1; i < 10 ; i += 2)
    {
        cmd_fs_write_t("/1.txt", i);
        rt_thread_mdelay(500);
        cmd_fs_write_t("/misc/1.txt", i);
        rt_thread_mdelay(500);
        //cmd_fs_write_code_t(NULL, i);
        //rt_thread_mdelay(500);
    }
}
void cmd_emmc_test_all_read(void)
{
    for (int i = 1; i < 10 ; i += 2)
    {
        cmd_fs_read_t("/1.txt", i);
        rt_thread_mdelay(500);
        cmd_fs_read_t("/misc/1.txt", i);
        rt_thread_mdelay(500);
        //cmd_fs_read_code_t(NULL, i);
        //rt_thread_mdelay(500);
    }
}
FINSH_FUNCTION_EXPORT_ALIAS(cmd_emmc_test_all_write, __cmd_emmc_test_all_write, cmd emmc tes all write);
FINSH_FUNCTION_EXPORT_ALIAS(cmd_emmc_test_all_read, __cmd_emmc_test_all_read, cmd emmc tes all read);


#endif


#ifndef BSP_USING_PC_SIMULATOR
#include <register.h>
#include <stdlib.h>
#include "bf0_hal_hlp.h"
static int cmd_test_memcpy(int argc, char **argv)
{
    if (argc < 2) return 0;

    uint8_t *src = (uint8_t *) 0x12a00000;
    uint32_t len = atoi(argv[1]) >> 2 << 2;
    uint8_t *dst = rt_malloc(len);
    RT_ASSERT(dst);
    SCB_InvalidateDCache_by_Addr((void *) src, len);
    rt_base_t level = rt_hw_interrupt_disable();
    HAL_DBG_DWT_Init();
    HAL_DBG_DWT_Reset();
    memcpy(dst, src, len);
    uint32_t cycles = HAL_DBG_DWT_GetCycles();
    rt_hw_interrupt_enable(level);
    LOG_I("%s: src %p dst %p len %d (cycles) %d", __func__, src, dst, len, cycles);
    rt_free(dst);
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_test_memcpy, test_memcpy, test_memcpy);
#endif

static int cmd_power_off(int argc, char **argv)
{
    uint8_t type = POWER_OFF;
    if (argc > 1)
    {
        uint8_t v = atoi(argv[1]);
        type = v <= POWER_OFF_BY_NAND_OVERWRITE ?  v : POWER_OFF;
    }

    app_power_off_action(type);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_power_off, power_off, power_off);
#endif

#endif
