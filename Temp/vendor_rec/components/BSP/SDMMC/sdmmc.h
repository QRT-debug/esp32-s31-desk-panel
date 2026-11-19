/**
 ******************************************************************************
 * @file        sdmmc.h
 * @version     V1.0
 * @brief       TF驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __SDMMC_H
#define __SDMMC_H

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

/* SDMMC外设相关硬件引脚 */
#define SDMMC_PIN_CMD       GPIO_NUM_25
#define SDMMC_PIN_CLK       GPIO_NUM_24
#define SDMMC_PIN_D0        GPIO_NUM_20
#define SDMMC_PIN_D1        GPIO_NUM_21
#define SDMMC_PIN_D2        GPIO_NUM_22
#define SDMMC_PIN_D3        GPIO_NUM_23

/* 挂载点定义 */
#define MOUNT_POINT         "/sdcard"

/* 外部变量声明 */
extern sdmmc_card_t *card;
extern uint8_t sdmmc_mount_flag;

/* 函数声明 */
esp_err_t sdmmc_init(void);               /* SD卡初始化并挂载 */
esp_err_t sdmmc_unmount(void);            /* 取消挂载SD卡 */

#endif
