/**
 * @file    i2c_bus.c
 * @brief   I2C0 总线初始化（全局唯一实例）
 */
#include "i2c_bus.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port          = I2C_BUS_PORT,
        .sda_io_num        = I2C_BUS_SDA_GPIO,
        .scl_io_num        = I2C_BUS_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "I2C0 ready  SDA=IO%d SCL=IO%d  %d Hz",
             I2C_BUS_SDA_GPIO, I2C_BUS_SCL_GPIO, I2C_BUS_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_handle(void)
{
    return s_bus;
}
