#include "app_adc.h"
#include "bf0_hal_adc.h"
#include "bf0_hal_pinmux.h"
#include <stdlib.h>

#define LOG_TAG "app.adc"
#include <ulog.h>

static ADC_HandleTypeDef hadc;

static uint32_t adc_read_channel(uint32_t ch)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    sConfig.Channel = ch;
    sConfig.pchnl_sel = ch;
    sConfig.slot_en = 1;
    sConfig.nchnl_sel = 0;
    sConfig.acc_num = 0;
    HAL_ADC_ConfigChannel(&hadc, &sConfig);
    HAL_ADC_EnableSlot(&hadc, ch, 1);

    ADC_SET_UNMUTE(&hadc);
    HAL_Delay_us(200);
    __HAL_ADC_START_CONV(&hadc);
    HAL_ADC_PollForConversion(&hadc, 100);
    uint32_t raw = HAL_ADC_GetValue(&hadc, ch);
    ADC_SET_MUTE(&hadc);

    return raw;
}

int app_adc_init(void)
{
    HAL_ADC_DeInit(&hadc);

    hadc.Instance = hwp_gpadc;
    hadc.Init.adc_se = 1;
    hadc.Init.atten3 = 0;
    hadc.Init.adc_force_on = 0;
    hadc.Init.dma_en = 0;
    hadc.Init.op_mode = 0;
    hadc.Init.en_slot = 1;
    hadc.Init.data_samp_delay = 3;
    hadc.Init.conv_width = 100;
    hadc.Init.sample_width = 95;

    if (HAL_ADC_Init(&hadc) != HAL_OK)
    {
        LOG_E("ADC init failed");
        return -RT_ERROR;
    }

    LOG_I("ADC init OK");
    return RT_EOK;
}

#ifdef FINSH_USING_MSH
static void adc_init_cmd(int argc, char *argv[])
{
    app_adc_init();
}
MSH_CMD_EXPORT(adc_init_cmd, Initialize ADC);

static void adc_test(int argc, char *argv[])
{
    if (argc > 1)
    {
        int ch = atoi(argv[1]);
        if (ch < 0 || ch > 10)
        {
            rt_kprintf("channel must be 0~10\n");
            return;
        }
        uint32_t val = adc_read_channel(ch);
        rt_kprintf("CH%d raw = %d\n", ch, val);
    }
    else
    {
        rt_kprintf("Scanning CH0~CH10:\n");
        for (int ch = 0; ch <= 10; ch++)
        {
            uint32_t val = adc_read_channel(ch);
            rt_kprintf("  CH%d = %d\n", ch, val);
        }
    }
}
MSH_CMD_EXPORT(adc_test, ADC scan. Usage: adc_test [channel]);
#endif
