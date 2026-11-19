/**
 ******************************************************************************
 * @file        myes8389.h
 * @version     V1.0
 * @brief       ES8389驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板 
 ******************************************************************************
 */

#ifndef __MYES8389_H_
#define __MYES8389_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "hal/gpio_types.h"
#include "math.h"
#include "string.h"
#include "es8389_codec.h"
#include "myi2s.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include <stdbool.h>
#include "myiic.h"


/* 引脚定义 */
#define PA_CTRL_GPIO_PIN          GPIO_NUM_7                 /* 功放使能 */


/* ---- 初始化 ---- */
uint8_t myes8389_init(void);

/* ---- API 声明 ---- */
int  myes8389_record_start(int sample_rate, int bits);       /* 开 ADC + PLL×2 + I2S */
void myes8389_record_stop(void);                             /* 关 ADC + I2S */
int  myes8389_play_start(int sample_rate, int bits);         /* 开 DAC + PLL×4 + I2S + PA */
void myes8389_play_stop(void);                               /* 关 DAC + PA + I2S */
void    myes8389_adc_enable(bool enable);
void    myes8389_dac_enable(bool enable);
void    myes8389_set_sample_info(int sample_rate, int bits_per_sample);
uint8_t myes8389_set_fs(uint32_t sample_rate, uint16_t bits_per_sample);
int     myes8389_reg_read(int reg);

/* ---- 音量 (0~100) ---- */
#define ES8389_DEFAULT_VOLUME   50                           /* 开机默认播放音量 */
void myes8389_set_volume(int vol);                           /* 运行时设置播放音量 (播放中立即生效) */
int  myes8389_get_volume(void);                              /* 读取当前播放音量 */

#endif
