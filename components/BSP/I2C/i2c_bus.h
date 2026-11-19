/**
 * @file    i2c_bus.h
 * @brief   I2C0 总线（触摸 / 摄像头 SCCB 共用）
 *
 * 原理图：TP_SDA = IO0，TP_SCL = IO1（与 DVP 的 SDA/SCL 同网）
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#define I2C_BUS_PORT        I2C_NUM_0
#define I2C_BUS_SDA_GPIO    GPIO_NUM_0
#define I2C_BUS_SCL_GPIO    GPIO_NUM_1
#define I2C_BUS_FREQ_HZ     400000

esp_err_t i2c_bus_init(void);
i2c_master_bus_handle_t i2c_bus_handle(void);
