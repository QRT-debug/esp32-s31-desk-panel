/**
 * @file    gt1151.c
 * @brief   GT1151Q / GT1158 触摸驱动实现（I2C 轮询）
 *
 * 与原厂例程的差异（都是踩过的坑）：
 *  1) 原厂 i2c_master_transmit 的长度用了 sizeof(pointer)，是错的，这里按 2+len 发。
 *  2) ★ 状态寄存器 0x814E 必须"只要 bit7 置位就清零"，**不管有没有触点**。
 *     只在 n>0 时清会让芯片卡死在"n=0 但 buffer 未释放"的状态，
 *     之后再也读不到坐标 —— 表现为"点屏幕完全没反应"。
 *  3) INT 脚保持输出低，不改成输入。因为它决定 I2C 地址（低=0x14），
 *     改成输入浮空后电平不确定，芯片一旦重新锁存地址就会变成 0x5D，
 *     之后所有访问 NAK，同样是"点了没反应"。轮询方式本来就不需要 INT。
 */
#include "gt1151.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lcd.h"

static const char *TAG = "gt1151";
static i2c_master_dev_handle_t s_dev = NULL;
static char s_id[8] = {0};

/* 诊断用：最近一次读到的原始状态字、触点数、累计 I2C 错误次数 */
static volatile uint8_t  s_raw    = 0;
static volatile uint8_t  s_n      = 0;
static volatile uint32_t s_errtot = 0;

#define REG_CMD         0x8040
#define REG_CFG         0x8047
#define REG_CFG_REAL    0x8050   /* GT1158 的配置表实际从这里开始 */
#define REG_PID         0x8140
#define REG_STATUS      0x814E
#define REG_TP1         0x8150
#define REG_STEP        8

/* 每个触点占 8 字节：X_L X_H Y_L Y_H SIZE_L SIZE_H 保留 保留 */

static esp_err_t wr_reg16(uint16_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tx[2 + 8];
    if (len > sizeof(tx) - 2)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    tx[0] = (uint8_t)(reg >> 8);
    tx[1] = (uint8_t)(reg & 0xFF);
    memcpy(tx + 2, buf, len);
    return i2c_master_transmit(s_dev, tx, 2 + len, 100);
}

static esp_err_t rd_reg16(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr, 2, buf, len, 100);
}

/* 上电后读出配置表里的关键参数，确认芯片与 800x480 这块屏是配对的。
 *
 * ★ 踩过的坑：GT1158 的配置表实际从 0x8050 开始（不是 GT911 的 0x8047）。
 *   按 GT911 的偏移去解 0x8047，会得到 X_max=5471 / tnum=124 这种荒谬值。
 *   实测 0x8050 处是 82 20 03 E0 01 05 ⇒ X=0x0320=800, Y=0x01E0=480, 5 点，
 *   与厂家给的 BT_GT1151Q_V130_800x480_Config.cfg 头 6 字节完全一致。 */
static void dump_config(void)
{
    uint8_t c[8] = {0};
    if (rd_reg16(REG_CFG_REAL, c, sizeof(c)) != ESP_OK)
    {
        ESP_LOGW(TAG, "read config table failed");
        return;
    }
    uint16_t x_max = (uint16_t)(c[1] | (c[2] << 8));
    uint16_t y_max = (uint16_t)(c[3] | (c[4] << 8));
    uint8_t  tnum  = c[5];
    ESP_LOGI(TAG, "cfg @0x8050: ver=0x%02X  X_max=%u  Y_max=%u  touch_num=%u",
             c[0], x_max, y_max, tnum);

    if (x_max != LCD_H_RES || y_max != LCD_V_RES)
    {
        ESP_LOGW(TAG, "cfg mismatch: panel is %dx%d", LCD_H_RES, LCD_V_RES);
    }
}

esp_err_t gt1151_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz    = I2C_BUS_FREQ_HZ,
        .device_address  = GT_I2C_ADDR,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle(), &dev_cfg, &s_dev),
                        TAG, "add device failed");

    /* RST 输出；INT 也输出并保持低 —— 它决定 I2C 地址（低=0x14），
     * 全程保持低电平，地址就不会漂。 */
    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << GT_RST_GPIO) | (1ULL << GT_INT_GPIO),
    };
    gpio_config(&io);

    gpio_set_level(GT_INT_GPIO, 0);
    gpio_set_level(GT_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(12));
    gpio_set_level(GT_RST_GPIO, 1);        /* 上升沿锁存 I2C 地址：INT=低 ⇒ 0x14 */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 地址锁存完就把 INT 交还给芯片。
     * ★ 不能再保持输出低：芯片检测到触摸时会把它拉高（推挽输出），
     *   MCU 同时输出低就是两个驱动器对着顶，芯片可能因此不工作。
     *   先 gpio_reset_pin 释放"输出"登记的占用，否则会报引脚冲突警告。 */
    gpio_reset_pin(GT_INT_GPIO);
    gpio_config_t io_in = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = 1ULL << GT_INT_GPIO,
    };
    gpio_config(&io_in);
    vTaskDelay(pdMS_TO_TICKS(60));

    uint8_t id[4] = {0};
    esp_err_t err = rd_reg16(REG_PID, id, 4);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "read PID failed: %s", esp_err_to_name(err));
        return err;
    }
    memcpy(s_id, id, 4);
    s_id[4] = 0;
    ESP_LOGI(TAG, "CTP id = %s (addr 0x%02X)", s_id, GT_I2C_ADDR);

    dump_config();

    /* 不发软复位（0x02）：它会重置芯片状态机，收益为零、风险不小。
     * 只写 0x00 确保处于 normal 工作模式。 */
    uint8_t v = 0x00;
    wr_reg16(REG_CMD, &v, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 清一次状态寄存器，避免残留的 buffer-ready 卡住第一次上报 */
    uint8_t zero = 0;
    wr_reg16(REG_STATUS, &zero, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t st0 = 0xFF;
    if (rd_reg16(REG_STATUS, &st0, 1) == ESP_OK)
    {
        ESP_LOGI(TAG, "status(0x814E) = 0x%02X  bit7=%d n=%u",
                 st0, (st0 >> 7) & 1, (unsigned)(st0 & 0x0F));
    }

    return ESP_OK;
}

esp_err_t gt1151_read(uint8_t *count, uint16_t *xs, uint16_t *ys, uint8_t max_pts)
{
    if (count == NULL || xs == NULL || ys == NULL || max_pts == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    static uint32_t s_err = 0;
    static uint8_t  s_last_st = 0xFF;

    uint8_t st = 0;
    esp_err_t err = rd_reg16(REG_STATUS, &st, 1);
    if (err != ESP_OK)
    {
        s_errtot++;
        /* I2C 出错时限速打印，否则 20 ms 一次会把日志刷爆 */
        if (s_err < 3 || (s_err % 200) == 0)
        {
            ESP_LOGE(TAG, "read status failed (%lu): %s", (unsigned long)s_err,
                     esp_err_to_name(err));
        }
        s_err++;
        return err;
    }
    s_err = 0;
    s_raw = st;

    /* ★ 只要 bit7 置位就清，不管有没有触点 —— 否则芯片不会更新下一次 */
    if (st & 0x80)
    {
        uint8_t zero = 0;
        wr_reg16(REG_STATUS, &zero, 1);
    }

    /* 状态变化时打一条，方便现场判断"芯片有没有在扫" */
    static TickType_t s_last_log = 0;
    if (st != s_last_st)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_log) >= pdMS_TO_TICKS(200))   /* 限速 200 ms，避免刷屏 */
        {
            ESP_LOGI(TAG, "status 0x%02X -> n=%u", st, (unsigned)(st & 0x0F));
            s_last_log = now;
        }
        s_last_st = st;
    }

    uint8_t n = st & 0x0F;
    s_n = n;
    if (n == 0 || n > max_pts)
    {
        return ESP_OK;
    }

    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t buf[4] = {0};
        if (rd_reg16((uint16_t)(REG_TP1 + i * REG_STEP), buf, 4) != ESP_OK)
        {
            break;
        }
        uint16_t x = (uint16_t)(((uint16_t)buf[1] << 8) | buf[0]);
        uint16_t y = (uint16_t)(((uint16_t)buf[3] << 8) | buf[2]);

        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;

        xs[i] = x;
        ys[i] = y;
    }
    *count = n;
    return ESP_OK;
}

const char *gt1151_id(void)
{
    return s_id;
}

uint8_t gt1151_raw_status(void)
{
    return s_raw;
}

uint8_t gt1151_touch_count(void)
{
    return s_n;
}

uint32_t gt1151_err_count(void)
{
    return s_errtot;
}
