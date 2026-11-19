/**
 ******************************************************************************
 * @file        main.c
 * @version     V1.0
 * @brief       录音机实验
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "led.h"
#include "lcd.h"
#include "touch.h"
#include "myes8389.h"
#include "myi2s.h"
#include "sdmmc.h"
#include "fonts.h"
#include "exfuns.h"
#include "recorder.h"
#include "ui.h"

#define UI_TASK_STACK   8192            /* UI 任务栈大小 */

/**
 * @brief       UI 任务: 初始化录音后端后进入触摸 UI 主循环
 * @param       arg  未使用
 * @retval      无
 */
static void ui_task(void *arg)
{
    recorder_init();                    /* 录音后端(采集任务)就绪 */
    ui_run();                           /* 进入 UI 主循环, 不会返回 */
}

/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;

    /* --- NVS --- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* --- 板级初始化 --- */
    led_init();
    lcd_init();

    /* --- SD卡初始化 --- */
    while (sdmmc_init()) {
        lcd_show_string(30, 110, 200, 16, 16, "SD Card Error!", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* --- 加载字库: Flash storage 分区无字库时自动从SD卡更新 --- */
    while (fonts_init()) {
        lcd_clear(WHITE);
        lcd_show_string(30, 30, 200, 16, 16, "ESP32-S31", RED);
        lcd_show_string(30, 50, 200, 16, 16, "Font Updating...", RED);

        uint8_t ok = fonts_update_font(30, 70, 16, (uint8_t *)"0:", RED);
        while (ok) {
            lcd_show_string(30, 70, 200, 16, 16, "Font Update Failed!", RED);
            vTaskDelay(pdMS_TO_TICKS(200));
            lcd_fill(20, 70, 240, 86, WHITE);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        lcd_show_string(30, 70, 200, 16, 16, "Font Update Success!   ", RED);
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clear(WHITE);
    }

    /* --- I2S 初始化 --- */
    if (myi2s_init() != ESP_OK) {
        lcd_show_string(30, 110, 200, 16, 16, "I2S Error", RED);
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* --- 触摸屏初始化 --- */
    tp_dev.init();

    /* --- ES8389 初始化 --- */
    while (myes8389_init()) {
        lcd_show_string(30, 110, 200, 16, 16, "ES8389 Error", RED);
        vTaskDelay(pdMS_TO_TICKS(200));
        lcd_fill(30, 110, 239, 126, WHITE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* --- PA 使能: 录音时保持关闭, 播放时 myes8389_play_start 自动打开 --- */
    gpio_set_level(PA_CTRL_GPIO_PIN, 0);

    /* --- FatFs 初始化 --- */
    exfuns_init();

    /* --- 启动 UI 任务 (运行在核心1, 与核心0的录音采集任务分开) ---
     * 开发板信息与主菜单由 ui_run() 统一绘制
     */
    vTaskDelay(pdMS_TO_TICKS(200));
    xTaskCreatePinnedToCore(ui_task, "ui_task", UI_TASK_STACK, NULL, 5, NULL, 1);
}
