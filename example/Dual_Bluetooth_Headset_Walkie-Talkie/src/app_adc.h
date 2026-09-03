#ifndef __APP_ADC_H__
#define __APP_ADC_H__

#include <rtthread.h>

/*
 * SF32LB57X GPADC — Channel to Pin Mapping
 *
 * RT-Thread device name: "bat1"
 *
 * | Channel | Pin       | Description                  |
 * |---------|-----------|------------------------------|
 * | 1       | PAD_PA01  | External analog input        |
 * | 2       | PAD_PA02  | External analog input        |
 * | 3       | PAD_PA03  | External analog input        |
 * | 4       | PAD_PA04  | External analog input        |
 * | 5       | PAD_PA05  | External analog input        |
 * | 6       | PAD_PA06  | External analog input        |
 * | 7       | PAD_PA07  | External analog input        |
 * | 8       | PAD_PA08  | External analog input        |
 * | 9       | PAD_PA09  | External analog input        |
 * | 10      | PAD_PA10  | External analog input        |
 * | 11      | (internal)| VBAT (1/2 divider)           |
 *
 * Note: GPADC pins are dedicated analog pins, NOT in pinmux function list.
 *       Use HAL_PIN_Set_Analog() to configure, no pinmux function needed.
 */

#define ADC_DEV_NAME        "bat1"      /* GPADC1 device name */
#define ADC_CHANNEL_MAX     11          /* Max external channel number */
#define ADC_VBAT_CHANNEL    11          /* Dedicated VBAT channel */

/* Initialize ADC module */
int app_adc_init(void);

/* Read PA00 ADC raw value (0~4095) */
rt_uint32_t app_adc_read(void);

#endif /* __APP_ADC_H__ */
