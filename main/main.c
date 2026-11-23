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
#include "app_cfg.h"
#include "backlight.h"
#include "bsp_led.h"
#include "sd_card.h"
#include "lcd.h"
#include "mdns.h"
#include "lvgl_port.h"
#include "nvs_flash.h"
#include "pc_monitor.h"
#include "touch.h"
#include "touch_service.h"
#include "ui_draw.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "weather.h"
#include "wifi_prov.h"
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

/**
 * 配网看门狗：开机 30 秒还没连上 WiFi 就开热点，让用户在手机浏览器里填表。
 *
 * ★ 只在"开机连不上"时触发，运行中断网不会自动开热点 —— 避免路由器重启、
 *   信号抖动时设备莫名其妙地广播一个 AP。要手动改配置走 System 页按钮。
 */
static void prov_watchdog_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < 30; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (wifi_sta_connected())
        {
            ESP_LOGI(TAG, "wifi up after %ds, no provisioning needed", i + 1);
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGW(TAG, "wifi not connected in 30s -> starting provisioning AP");
    if (wifi_prov_start() == ESP_OK)
    {
        ESP_LOGW(TAG, "connect phone to \"%s\" (pass %s), open http://%s/",
                 wifi_prov_ssid(), PROV_AP_PASS, PROV_AP_IP);
    }
    vTaskDelete(NULL);
}

/* ★ mDNS：让设备在局域网里有固定名字 http://s31.local/ —— 不用再记会变的 IP。
 *   等 WiFi 连上（拿到 IP）后初始化；失败不影响其他功能。 */
static void mdns_boot_task(void *arg)
{
    (void)arg;
    while (!wifi_sta_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_err_t e = mdns_init();
    if (e == ESP_OK)
    {
        mdns_hostname_set("s31");
        mdns_instance_name_set("ESP32-S31 SmartPanel");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS ready: http://s31.local/");
        /* ★ 配网页常驻：不用再点 WIFI 按钮（之前只在配网流程里启动） */
        if (wifi_prov_httpd_start() == ESP_OK)
        {
            ESP_LOGI(TAG, "config page at http://<sta-ip>/");
        }
    }
    else
    {
        ESP_LOGW(TAG, "mdns init failed: %s", esp_err_to_name(e));
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

    /* ★★★ 11-22 运行时配置：必须在 NVS 之后、WiFi/背光/网络任务之前。
     *   首次上电会把 wifi_config.h 里的编译期宏写成默认值，所以行为与改造前一致；
     *   以后配网页只改 NVS，业务代码不用动。 */
    app_cfg_init();

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
    /* 把 NVS 里的亮度作为上电默认值（配网页可改）；没有配置时 init 已置 75 */
    backlight_set_boot(app_cfg_get()->backlight);

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
        xTaskCreate(mdns_boot_task, "mdns", 3072, NULL, 3, NULL);
        pc_monitor_start();
        weather_start();          /* 天气：WiFi 就绪后首拉，之后 30 分钟一次 */
        hf_start();               /* HF 热榜：每小时一次 */
        ai_rank_start();          /* 国内 AI 产品榜：24 小时一次（站点季度更新） */
        bili_start();             /* B 站热搜：30 分钟一次 */
        ESP_LOGI(TAG, "monitor target: %s", pc_monitor_url());
        xTaskCreate(prov_watchdog_task, "provwd", 3072, NULL, 3, NULL);
    }
    else
    {
        ESP_LOGE(TAG, "wifi init failed");
    }
    ui_theme_init();       /* 读 NVS 恢复上次主题（浅色/深色） */
    ui_manager_run();
}
