/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-06-02     Assistant    Adapted for GTW623 (CST-based)
 */

#include <rtthread.h>
#include "board.h"
#include "gtw623.h"
#include "drv_touch.h"
#include "string.h"

#ifdef TSC_USING_GTW623

#define DRV_DEBUG
#define DBG_LEVEL DBG_LOG
#define LOG_TAG "drv.gtw623"
#include <drv_log.h>

/* CST register map (16-bit) */
#define REG_CHIP_ID       0xD204
#define REG_DEBUG_INFO    0xD101
#define REG_NORMAL        0xD109
#define REG_TOUCH_STATUS  0xD000
#define TOUCH_POINT_SIZE  5U
#define TOUCH_POINT_DATA_OFFSET(index) \
    ((index) == 0U ? 0U : (2U + (index) * TOUCH_POINT_SIZE))
#define GTW623_MAX_CONTACTS 5U
#define TOUCH_REPORT_SIZE \
    (GTW623_MAX_CONTACTS * TOUCH_POINT_SIZE + 5U)
#define TOUCH_REPORT_END  0xABU
#define TOUCH_EVENT_ACTIVE 0x06U

/* drv_touch_publish_contacts()/struct touch_contact belong to a multi-touch
 * API not available in this SDK's drv_touch.h. Keep a local struct for report
 * parsing; only the single-point p_msg is published through touch_ops. */
struct gtw623_contact
{
    uint8_t id;
    uint16_t x;
    uint16_t y;
};

#define TOUCH_SLAVE_ADDR  0x5A

#define GPIO_TP_RESET 32
#define GPIO_TP_1V8_EN 0
#define GPIO_TP_INT  31

extern void HAL_Delay_us(__IO uint32_t us);
#define ztw_hw_us_delay(us) HAL_Delay_us(us)
#define gpio_set_level(pin,level) rt_pin_write(pin,level)
#define gpio_get_level(pin) rt_pin_read(pin)

static struct touch_drivers gtw_driver = {0};
static struct rt_i2c_bus_device *ft_bus = NULL;
static rt_bool_t touch_active;
static rt_bool_t touch_primary_valid;
static uint8_t touch_primary_id;
static uint16_t touch_last_x;
static uint16_t touch_last_y;

static void ts_isr_enable(rt_bool_t enable)
{
    if (enable)
        rt_pin_irq_enable(GPIO_TP_INT, PIN_IRQ_ENABLE);
    else
        rt_pin_irq_enable(GPIO_TP_INT, PIN_IRQ_DISABLE);
}

void ts_touch_isr(void *args)
{
    ts_isr_enable(RT_FALSE);
    rt_sem_release(gtw_driver.isr_sem);
}

static bool cst_cmd(uint16_t reg)
{
    uint8_t buf[2] = { reg >> 8, reg & 0xFF };
    struct rt_i2c_msg msgs[] = {
        {
            .addr   = TOUCH_SLAVE_ADDR,
            .flags  = RT_I2C_WR,
            .len    = 2,
            .buf    = buf,
        }
    };
    return rt_i2c_transfer(ft_bus, msgs, 1) == 1;
}

static bool cst_read(uint16_t reg, uint8_t *data, uint16_t len)
{
    uint8_t buf[2] = { reg >> 8, reg & 0xFF };
    struct rt_i2c_msg msgs[] = {
        {
            .addr   = TOUCH_SLAVE_ADDR,
            .flags  = RT_I2C_WR,
            .len    = 2,
            .buf    = buf,
        },
        {
            .addr   = TOUCH_SLAVE_ADDR,
            .flags  = RT_I2C_RD,
            .len    = len,
            .buf    = data,
        }
    };
    return rt_i2c_transfer(ft_bus, msgs, 2) == 2;
}

static bool cst_write(uint16_t reg, const uint8_t *data, uint16_t len)
{
    uint8_t buf[3];
    struct rt_i2c_msg msg;

    if (data == RT_NULL || len != 1U) {
        return false;
    }
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)reg;
    buf[2] = data[0];
    msg.addr = TOUCH_SLAVE_ADDR;
    msg.flags = RT_I2C_WR;
    msg.len = sizeof(buf);
    msg.buf = buf;
    return rt_i2c_transfer(ft_bus, &msg, 1) == 1;
}

static void hw_reset(void)
{
    gpio_set_level(GPIO_TP_RESET, 0);
    rt_thread_mdelay(2);
    gpio_set_level(GPIO_TP_RESET, 1);
    rt_thread_mdelay(200);
}

static bool detect(void)
{
    uint32_t id = 0;

    if (!cst_cmd(REG_DEBUG_INFO))
        return false;
    rt_thread_mdelay(1);
    if (!cst_read(REG_CHIP_ID, (uint8_t *)&id, 4))
        return false;

    if (id == 0 || id == 0xFFFFFFFF)
        return false;

    LOG_I("CST: id=0x%08X\n", (unsigned)id);
    return true;
}

static rt_err_t init(void)
{
    LOG_D("gtw623 init");

    touch_active = RT_FALSE;
    touch_primary_valid = RT_FALSE;
    touch_primary_id = 0U;
    touch_last_x = 0U;
    touch_last_y = 0U;
    //drv_touch_publish_contacts(RT_NULL, 0U);   // multi-touch API not in this SDK

    gpio_set_level(GPIO_TP_1V8_EN, 1);
    rt_pin_mode(GPIO_TP_RESET, PIN_MODE_OUTPUT);
    rt_thread_mdelay(10);

    hw_reset();

    rt_pin_mode(GPIO_TP_INT, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(GPIO_TP_INT, PIN_IRQ_MODE_FALLING, ts_touch_isr, NULL);
    rt_pin_irq_enable(GPIO_TP_INT, PIN_IRQ_ENABLE);

    if (!detect()) {
        LOG_E("CST: detect failed\n");
        return RT_ERROR;
    }

    if (!cst_cmd(REG_NORMAL)) {
        LOG_E("CST: normal mode failed\n");
        return RT_ERROR;
    }

    LOG_I("gtw623 init OK");
    return RT_EOK;
}

static rt_bool_t probe(void)
{
    ft_bus = (struct rt_i2c_bus_device *)rt_device_find(TOUCH_DEVICE_NAME);
    if (RT_Device_Class_I2CBUS != ft_bus->parent.type)
    {
        ft_bus = NULL;
    }
    if (ft_bus)
    {
        rt_device_open((rt_device_t)ft_bus, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_TX | RT_DEVICE_FLAG_INT_RX);
    }
    else
    {
        LOG_E("bus not find\n");
        return RT_FALSE;
    }

    return RT_TRUE;
}

static rt_err_t deinit(void)
{
    LOG_D("gtw623 deinit");
    rt_pin_irq_enable(GPIO_TP_INT, PIN_IRQ_DISABLE);
    touch_active = RT_FALSE;
    touch_primary_valid = RT_FALSE;
    touch_primary_id = 0U;
    touch_last_x = 0U;
    touch_last_y = 0U;
    //drv_touch_publish_contacts(RT_NULL, 0U);   // multi-touch API not in this SDK
    return RT_EOK;
}

static rt_err_t read_point(touch_msg_t p_msg)
{
    uint8_t buf[TOUCH_REPORT_SIZE];
    struct gtw623_contact contacts[GTW623_MAX_CONTACTS];
    uint8_t ack = TOUCH_REPORT_END;
    uint8_t reported_count;
    uint8_t active_count = 0U;
    uint16_t contact_ids = 0U;
    uint8_t i;
    rt_err_t result = RT_EEMPTY;

    if (!cst_read(REG_TOUCH_STATUS, buf, sizeof(buf))) {
        p_msg->event = TOUCH_EVENT_NONE;
        ts_isr_enable(RT_TRUE);
        return RT_ERROR;
    }

    /* CST92xx holds the report until D000 is acknowledged. Release every
     * successfully read report, including one later rejected as malformed. */
    if (!cst_write(REG_TOUCH_STATUS, &ack, 1U)) {
        p_msg->event = TOUCH_EVENT_NONE;
        result = RT_ERROR;
        goto done;
    }
    reported_count = (uint8_t)(buf[5] & 0x7FU);
    if (buf[0] == TOUCH_REPORT_END || buf[0] == 0U ||
        buf[6] != TOUCH_REPORT_END) {
        reported_count = 0U;
    }
    if (reported_count > GTW623_MAX_CONTACTS) {
        p_msg->event = TOUCH_EVENT_NONE;
        goto done;
    }
    for (i = 0U; i < reported_count; i++) {
        uint8_t offset = TOUCH_POINT_DATA_OFFSET(i);
        uint8_t id = (uint8_t)(buf[offset] >> 4);

        /* Track IDs are 4-bit lifecycle identifiers, not contact-array
         * indexes. A five-contact frame may legally use IDs above four. */
        if ((buf[offset] & 0x0FU) != TOUCH_EVENT_ACTIVE) {
            continue;
        }
        if ((contact_ids & (uint16_t)(1U << id)) != 0U) {
            p_msg->event = TOUCH_EVENT_NONE;
            goto done;
        }
        contact_ids |= (uint16_t)(1U << id);
        contacts[active_count].id = id;
        contacts[active_count].x =
            ((uint16_t)buf[offset + 1U] << 4) |
            ((uint16_t)buf[offset + 3U] >> 4);
        contacts[active_count].y =
            ((uint16_t)buf[offset + 2U] << 4) |
            ((uint16_t)buf[offset + 3U] & 0x0FU);
        active_count++;
    }
    //drv_touch_publish_contacts(contacts, active_count);   // multi-touch API not in this SDK

    if (active_count != 0U) {
        int8_t primary_index = -1;

        if (touch_primary_valid) {
            for (i = 0U; i < active_count; i++) {
                if (contacts[i].id == touch_primary_id) {
                    primary_index = (int8_t)i;
                    break;
                }
            }
        }
        if (primary_index < 0 && touch_active) {
            /* Do not jump the LVGL pointer to another finger when report
             * ordering changes or the primary finger is lifted. Raw users
             * still receive the complete multi-contact snapshot above. */
            p_msg->x = touch_last_x;
            p_msg->y = touch_last_y;
            p_msg->event = TOUCH_EVENT_UP;
            touch_active = RT_FALSE;
            touch_primary_valid = RT_FALSE;
            goto done;
        }
        if (primary_index < 0) {
            primary_index = 0;
            touch_primary_id = contacts[0].id;
            touch_primary_valid = RT_TRUE;
        }
        p_msg->x = contacts[primary_index].x;
        p_msg->y = contacts[primary_index].y;
        touch_last_x = p_msg->x;
        touch_last_y = p_msg->y;
        /* Motion frames must also report DOWN: tp_read_thread_entry only
         * forwards DOWN/UP (MOVE is dropped), same as gt911/cst820 drivers. */
        p_msg->event = TOUCH_EVENT_DOWN;
        touch_active = RT_TRUE;
    } else {
        /* The release report carries no coordinates. Preserve the last
         * sampled point so a short flick is not reduced to zero distance. */
        p_msg->x = touch_last_x;
        p_msg->y = touch_last_y;
        p_msg->event = touch_active ? TOUCH_EVENT_UP : TOUCH_EVENT_NONE;
        touch_active = RT_FALSE;
        touch_primary_valid = RT_FALSE;
    }

done:
    ts_isr_enable(RT_TRUE);
    return result;
}

static struct touch_ops ops =
{
    read_point,
    init,
    deinit
};

static int gtw_driver_init(void)
{
    gtw_driver.probe = probe;
    gtw_driver.ops = &ops;
    gtw_driver.user_data = RT_NULL;
    gtw_driver.isr_sem = rt_sem_create("gtw623", 0, RT_IPC_FLAG_FIFO);

    rt_touch_drivers_register(&gtw_driver);

    return 0;
}
INIT_COMPONENT_EXPORT(gtw_driver_init);

#endif
