/**
 * @file    scene_splash.c
 * @brief   开机初始化画面
 *
 * 这是"设备"的启动画面，不再绑定某个具体功能（以后还会加更多界面）：
 *   Starting WiFi  →  Waiting for data  →  Ready
 * 数据到手后停留 0.8 秒再进主界面；超时 15 秒也放行（进主界面显示离线状态）。
 * 底部显示编译时间（__DATE__/__TIME__），用于确认"烧进去的确实是新构建"。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "pc_monitor.h"
#include "ui_fonts.h"
#include "ui_scenes.h"
#include "wifi_sta.h"

static const char *TAG = "splash";

#define COL_BG      0x000000
#define COL_TEXT    0xE6EAF0
#define COL_DIM     0x5A6273
#define COL_GREEN   0x35D07F
#define COL_BLUE    0x4C9AFF
#define COL_YELLOW  0xF2C14E

#define TIMEOUT_MS      15000
#define MIN_SHOW_MS     600
#define READY_HOLD_MS   800

void ui_splash_run(void)
{
    if (!lvgl_port_ready())
    {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A1020), 0);   /* 单色深蓝（渐变已撤回：逐行插值占用总线，放大边缘闪烁） */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &ui_font_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(title, "ESP32-S31");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_set_style_text_font(sub, &ui_font_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(sub, "desktop terminal    800x480 RGB    WiFi + Audio");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *status = lv_label_create(scr);
    lv_obj_set_style_text_font(status, &ui_font_20, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(COL_YELLOW), 0);
    lv_label_set_text(status, "Booting...");
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 70);

    /* 编译时间：一眼确认板子里跑的是不是最新构建（踩过"没烧进去"的坑） */
    lv_obj_t *ver = lv_label_create(scr);
    lv_obj_set_style_text_font(ver, &ui_font_14, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x3E4657), 0);
    lv_label_set_text(ver, "build " __DATE__ "  " __TIME__);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -10);

    int64_t t0        = esp_timer_get_time();
    int     last_stage = -1;

    while (1)
    {
        int64_t el = (esp_timer_get_time() - t0) / 1000;    /* ms */

        pc_stats_t st;
        pc_monitor_get(&st);

        int         stage;
        const char *txt;
        uint32_t    col;

        if (st.valid)
        {
            stage = 3;
            txt   = "Ready";
            col   = COL_GREEN;
        }
        else if (wifi_sta_connected())
        {
            stage = 2;
            txt   = "Waiting for data...";
            col   = COL_BLUE;
        }
        else
        {
            stage = 1;
            txt   = "Starting WiFi...";
            col   = COL_YELLOW;
        }

        if (stage != last_stage)
        {
            lv_label_set_text(status, txt);
            lv_obj_set_style_text_color(status, lv_color_hex(col), 0);
            last_stage = stage;
            ESP_LOGI(TAG, "stage %d: %s", stage, txt);
        }

        lvgl_port_task();

        if (st.valid && el > (MIN_SHOW_MS + READY_HOLD_MS))
        {
            break;
        }
        if (el > TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "timeout, go on with stale state");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
