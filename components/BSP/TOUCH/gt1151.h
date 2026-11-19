/**
 * @file    gt1151.h
 * @brief   GT1151Q 电容触摸驱动（I2C）
 *
 * 引脚（原理图）：SDA=IO0  SCL=IO1  INT=IO55  RST=IO39
 * I2C 地址由复位期间 INT 电平决定：INT 低 = 0x14，INT 高 = 0x5D
 */
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define GT_INT_GPIO      GPIO_NUM_55
#define GT_RST_GPIO      GPIO_NUM_39
#define GT_I2C_ADDR      0x14
#define GT_MAX_POINTS    5

esp_err_t  gt1151_init(void);
esp_err_t  gt1151_read(uint8_t *count, uint16_t *xs, uint16_t *ys, uint8_t max_pts);
const char *gt1151_id(void);

/* 诊断：最近一次读到的原始状态字 0x814E、触点数、累计 I2C 错误次数 */
uint8_t  gt1151_raw_status(void);
uint8_t  gt1151_touch_count(void);
uint32_t gt1151_err_count(void);
