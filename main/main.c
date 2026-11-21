/**
 ******************************************************************************
 * @file        main.c
 * @brief       ESP32-S31 桌面监视屏：局域网读取电脑 CPU/GPU/内存/磁盘
 *
 * 工程结构：
 *   components/BSP/LCD     RGB 面板 + 绘图
 *   components/BSP/I2C     I2C0（触摸）
 *   components/BSP/TOUCH   GT1151Q 驱动 + 触摸服务任务
 *   components/netapp      WiFi STA + 电脑状态轮询（HTTP JSON）
 *   components/ui          绘制原语 / 场景调度 / LVGL 移植 / 监控界面
 *   main/main.c            只做初始化与启动
 *
 * 配套电脑端：tools/pc_monitor_server.py（先跑它，再给板子上电）
 ******************************************************************************
 */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "audio_player.h"
#include "backlight.h"
#include "bsp_led.h"
#include "sd_card.h"
#include "lcd.h"
#include "lvgl_port.h"
#include "nvs_flash.h"
#include "pc_monitor.h"
#include "touch.h"
#include "touch_service.h"
#include "ui_draw.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "weather.h"
#include "hf_trend.h"
#include "ai_rank.h"
#include "bili_hot.h"
#include "wifi_sta.h"

static const char *TAG = "main";

/* ★ 11-20 诊断：一次性 dump 全部任务的核归属（回答"多核真的用上了吗"）。
 *   9 秒后打（WiFi 已连、splash 已过、四页已跑，代表稳态负载）。
 *   结论确认后整段可删（TRACE_FACILITY 也一并关回）。 */
static void task_core_dump_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(9000));
    TaskStatus_t st[48];
    UBaseType_t n = uxTaskGetSystemState(st, 48, NULL);
    ESP_LOGI(TAG, "=== task/core dump: %u tasks ===", (unsigned)n);
    for (UBaseType_t k = 0; k < n; k++)
    {
        /* ★ xTaskGetCoreID：任务钉住的核（0/1），tskNO_AFFINITY = 两核皆可自由调度 */
        BaseType_t core = xTaskGetCoreID(st[k].xHandle);
        const char *aff = (core == 0) ? "pin0" : (core == 1) ? "pin1" : "any";
        ESP_LOGI(TAG, "%-16s aff=%-4s prio=%2u state=%d stack_free=%6u",
                 st[k].pcTaskName, aff,
                 (unsigned)st[k].uxCurrentPriority,
                 (int)st[k].eCurrentState,
                 (unsigned)st[k].usStackHighWaterMark);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== PC MONITOR  (ESP32-S31 + 800x480 RGB) ===");

    xTaskCreate(task_core_dump_task, "taskdump", 4096, NULL, 3, NULL);

    /* NVS 必须最先初始化：WiFi 驱动要在里面存校准数据 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lcd_init();
    ESP_LOGI(TAG, "LCD ready  PCLK %d MHz", LCD_PCLK_MHZ);

    if (touch_service_start() == ESP_OK)
    {
        ESP_LOGI(TAG, "touch ready  id = %s", touch_id());
    }
    else
    {
        ESP_LOGW(TAG, "touch init failed, continue without touch");
    }

    /* LVGL 要在 lcd_init() 之后：它直接接管 panel 的帧缓冲 */
    if (lvgl_port_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "lvgl init failed");
    }

    /* ★ 背光只配 PWM 不点亮（duty=0）——首帧 flush 完成后 lvgl_port 才点亮，
     *   消灭复位→首帧之间的白屏期。必须在任何 LVGL 渲染之前配置好。 */
    backlight_init();

    /* ES8389 音频：初始化后停在 REC 态（麦克风常开、PA 关）。
     * ★ 复用触摸那条 I2C0；PA(IO7) 由模式机统一管理 */
    if (audio_player_init() == ESP_OK)
    {
        ESP_LOGI(TAG, "audio: ES8389 ready (mode=REC, mic hot, PA off)");
        /* 音乐 FAT 分区（vfs @0x200000）：只读挂载到 /music，失败不阻塞 */
        bsp_led_init();     /* IO37 WS2812 彩灯（系统页可调色） */

        audio_player_mount();

        /* TF 卡（SDMMC 4-bit）：挂到 /sdcard；失败不阻塞（回退 /music 分区） */
        sd_card_mount();

        /* 麦克风开机自检：连读 5 次（看电平是否从上电瞬态衰减到稳态） */
        vTaskDelay(pdMS_TO_TICKS(1500));
        for (int i = 0; i < 5; i++)
        {
            ESP_LOGI(TAG, "mic check #%d = %d", i, audio_player_mic_level());
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        audio_player_mic_debug_log();
    }
    else
    {
        ESP_LOGW(TAG, "audio init failed, continue without audio");
    }

    /* WiFi 异步连接，不阻塞；连上后 pc_monitor 自动开始拉数据 */
    if (wifi_sta_init() == ESP_OK)
    {
        pc_monitor_start();
        weather_start();          /* 天气：WiFi 就绪后首拉，之后 30 分钟一次 */
        hf_start();               /* HF 热榜：每小时一次 */
        ai_rank_start();          /* 国内 AI 产品榜：24 小时一次（站点季度更新） */
        bili_start();             /* B 站热搜：30 分钟一次 */
        ESP_LOGI(TAG, "monitor target: %s", pc_monitor_url());
    }
    else
    {
        ESP_LOGE(TAG, "wifi init failed");
    }
    ui_theme_init();       /* 读 NVS 恢复上次主题（浅色/深色） */
    ui_manager_run();
}
