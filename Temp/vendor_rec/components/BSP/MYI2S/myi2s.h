/**
 ******************************************************************************
 * @file        i2s.h
 * @version     V1.0
 * @brief       I2S驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef _MYI2S_H_
#define _MYI2S_H_

#include <stdio.h>
#include <string.h>
#include "hal/gpio_types.h"
#include "driver/i2s_std.h"


#define I2S_NUM                 (I2S_NUM_0)                /* I2S port */
#define I2S_BCK_IO              (GPIO_NUM_3)               /* ES8389_SCLK */
#define I2S_WS_IO               (GPIO_NUM_4)               /* ES8389_LRCK */
#define I2S_DO_IO               (GPIO_NUM_5)               /* ES8389_DSDIN */
#define I2S_DI_IO               (GPIO_NUM_6)               /* ES8389_SDOUT */
#define I2S_MCK_IO              (GPIO_NUM_NC)              /* 无MCLK,时钟由BCLK派生 */
#define I2S_SAMPLE_RATE         (16000)                    /* 采样率16kHz */
extern i2s_chan_handle_t tx_handle;
extern i2s_chan_handle_t rx_handle;
extern i2s_std_config_t my_std_cfg;                        /* I2S模式配置结构体(供外部更新采样率/位宽) */

/* 函数声明 */
esp_err_t myi2s_init(void);                                         /* I2S初始化 */
void i2s_trx_start(void);                                           /* 启动I2S */
void i2s_trx_stop(void);                                            /* 停止I2S */
void i2s_trx_start_ex(bool enable_tx, bool enable_rx);              /* 启动指定I2S通道 */
void i2s_trx_stop_ex(bool disable_tx, bool disable_rx);             /* 停止指定I2S通道 */
void i2s_deinit(void);                                              /* 卸载I2S */
size_t i2s_tx_write(uint8_t *buffer, uint32_t frame_size);          /* I2S传输数据 */
size_t i2s_rx_read(uint8_t *buffer, uint32_t frame_size);           /* I2S接收数据 */
void i2s_set_samplerate_bits_sample(int samplerate,int bits_sample);/* 设置采样率和位宽 */
void i2s_set_tx_clock_apll(void);                                   /* 将TX(主机)时钟源重配为APLL,获取精确采样率 */

#endif
