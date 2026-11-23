/**
 * @file    scene_system.c
 * @brief   系统信息页：芯片/内存/存储 + WiFi 状态与手动开关 + 最近日志
 *
 * 三个用途：
 *   ① 看 S31 的真实配置（核心数、频率、flash/PSRAM、堆余量、任务数、运行时长）；
 *   ② **手动断开/重连 WiFi** —— 用来验证右上角信号图标（断开后应变全灰）；
 *   ③ 显示最近 10 条系统日志（拦截 esp_log 得到的环形缓冲，不含 syslog 噪音）。
 *
 * 刷新节奏：每 1 秒更新一次（scene 的 hold_ms = 100，即每 10 帧）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_player.h"
#include "backlight.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "bsp_led.h"
#include "wifi_config.h"
#include "wifi_prov.h"
#include "voice.h"
#include "wifi_sta.h"

/* 11-20 主题化：指向可切换色板（本页还承载"深色模式"开关） */
#define COL_BG     (ui_theme()->bg_top)
#define COL_TEXT   (ui_theme()->title)
#define COL_DIM    (ui_theme()->sub)
#define COL_BLUE   0x4C9AFF
#define COL_ORANGE 0xF2A03D
#define COL_GREEN  0x35D07F
#define COL_RED    0xE0554B
#define COL_PANEL  (ui_theme()->card)    /* 白卡片 */

static lv_obj_t *s_lbl_sys;      /* 左面板：芯片/内存/存储 */
static lv_obj_t *s_lbl_wifi;     /* 右面板：WiFi 详情 */
static lv_obj_t *s_lbl_log;      /* 底部：最近日志 */
static lv_obj_t *s_lbl_btn;      /* WiFi 按钮文字 */
static lv_obj_t *s_btn_wifi;
static lv_obj_t *s_lbl_bl;      /* 背光百分比 */
static lv_obj_t *s_bar_mic;
static lv_obj_t *s_lbl_asr;

/* ★ 语音识别结果桥：voice 回调在 voice_task 上下文，绝不能直接碰 LVGL
 *   （lv_async_call 也死锁过）——只写标志，由本场景 draw 里轮询消费。 */
static char          s_asr_buf[256];
static volatile bool s_asr_ready;
static bool          s_asr_subscribed;     /* 麦克风电平条 */
static lv_obj_t *s_lbl_mic;     /* 麦克风电平数值 */
static lv_obj_t *s_slider_hue;  /* RGB 灯色相滑块 */
static lv_obj_t *s_slider_val;  /* RGB 灯亮度滑块 */
static lv_obj_t *s_led_swatch;  /* RGB 灯色块预览 */
static uint16_t  s_led_hue = 0;   /* ★ 会话内保持：切页回来滑块恢复到灯的真实状态 */
static uint8_t   s_led_val = 128;

/* ---------------- 回调 ---------------- */

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_HOME);
}

static void wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_sta_connected())
    {
        wifi_sta_disconnect_user();
    }
    else
    {
        wifi_sta_reconnect_user();
    }
}

/* ---------------- 刷新 ---------------- */

static void refresh_all(void)
{
    char b[512];

    /* ---- 左：芯片 / 内存 / 存储 ---- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t cpu_mhz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t ps_total  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    int64_t  up_s = esp_timer_get_time() / 1000000;
    int      up_h = (int)(up_s / 3600);
    int      up_m = (int)((up_s % 3600) / 60);
    int      up_sec = (int)(up_s % 60);
    uint32_t tasks = (uint32_t)uxTaskGetNumberOfTasks();

    snprintf(b, sizeof(b),
             "CHIP      ESP32-S31  %d core(s) @ %u MHz\n"
             "FLASH     16 MB\n"
             "PSRAM     %u KB free / %u KB\n"
             "SRAM      %u KB free / %u KB  (min %u KB)\n"
             "UPTIME    %02d:%02d:%02d      TASKS %u\n"
             "IDF       %s",
             chip.cores, (unsigned)cpu_mhz,
             (unsigned)(ps_free / 1024), (unsigned)(ps_total / 1024),
             (unsigned)(ram_free / 1024), (unsigned)(ram_total / 1024),
             (unsigned)(ram_min / 1024),
             up_h, up_m, up_sec, (unsigned)tasks,
             esp_get_idf_version());
    lv_label_set_text(s_lbl_sys, b);

    /* ---- 右：WiFi ---- */
    char ip[20] = {0};
    wifi_sta_get_ip(ip, sizeof(ip));
    bool conn = wifi_sta_connected();
    int  rssi = wifi_sta_rssi();

    int lvl = (rssi == 0) ? 0 : (rssi >= -55 ? 3 : (rssi >= -70 ? 2 : (rssi >= -85 ? 1 : 0)));

    snprintf(b, sizeof(b),
             "STATUS   %s\n"
             "SSID     %s\n"
             "IP       %s\n"
             "RSSI     %d dBm  (%d/3)\n"
             "MODE     STA + auto-reconnect",
             conn ? "connected" : "DISCONNECTED",
             conn ? WIFI_SSID : "-",
             ip,
             rssi, lvl);
    lv_label_set_text(s_lbl_wifi, b);
    lv_obj_set_style_text_color(s_lbl_wifi,
                                lv_color_hex(conn ? COL_TEXT : COL_RED), 0);

    /* 按钮文字随状态变 */
    lv_label_set_text(s_lbl_btn, conn ? "DISCONNECT WiFi" : "CONNECT WiFi");
    lv_obj_set_style_border_color(s_btn_wifi,
                                  lv_color_hex(conn ? COL_RED : COL_GREEN), 0);

    /* ---- 底部：最近日志（8 行） ---- */
    static char lines[7][96];
    int n = ui_logbuf_copy(lines, 7);
    b[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n && used < sizeof(b) - 100; i++)
    {
        int w = snprintf(b + used, sizeof(b) - used, "%s\n", lines[i]);
        if (w <= 0)
        {
            break;
        }
        used += (size_t)w;
    }
    if (n == 0)
    {
        snprintf(b, sizeof(b), "(no log yet)");
    }
    lv_label_set_text(s_lbl_log, b);
}

/* ---------------- 建界面 ---------------- */

static lv_obj_t *panel(lv_obj_t *scr, int x, int y, int w, int h, uint32_t border)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 14, 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(p, lv_color_hex(ui_theme()->shadow), 0);
    lv_obj_set_style_shadow_offset_y(p, 3, 0);
    (void)border;
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/**
 * 配网按钮：开 SoftAP 并弹一张指引卡。
 *
 * ★ 为什么用弹出卡而不是切页面：配网时用户手上是手机，屏幕只需"告诉连哪个热点、
 *   开哪个网址"就够了，不需要在屏上输入任何东西（输 WiFi 密码必须用真键盘）。
 * ★ 卡片挂在当前 scr 上，切页时随 lv_obj_clean 一起消失，不会残留。
 */
/** 记住从哪进的时钟，触摸返回时回这个场景（全局变量，scene_clock.c 里用） */
int g_clock_return_scene = UI_SCENE_HOME;

static void clock_cb(lv_event_t *e)
{
    (void)e;
    g_clock_return_scene = ui_current_scene();
    ui_goto_scene(UI_SCENE_CLOCK);
}

static void prov_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    if (mask != NULL)
    {
        lv_obj_del(mask);
    }
}

static void on_voice_evt(const voice_evt_t *e)
{
    if (e->type == VOICE_EV_TEXT || e->type == VOICE_EV_ERROR)
    {
        snprintf(s_asr_buf, sizeof(s_asr_buf), "%s",
                 e->text ? e->text : "(empty)");
        s_asr_ready = true;
    }
}

static void voice_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_VOICE);   /* 独立语音页（本页只做入口） */
}

static void prov_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_obj_get_screen(lv_event_get_target(e));

    esp_err_t err = wifi_prov_start();
    if (err != ESP_OK)
    {
        return;
    }

    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_set_size(mask, 800, 480);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_set_size(card, 600, 300);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A2233), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t1 = lv_label_create(card);
    lv_obj_set_style_text_font(t1, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(0x5B7CFA), 0);
    lv_label_set_text(t1, "手机配网");
    lv_obj_set_pos(t1, 0, 0);

    char b[128];
    lv_obj_t *t2 = lv_label_create(card);
    lv_obj_set_style_text_font(t2, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(0xE6EDF7), 0);
    snprintf(b, sizeof(b), "1. 连热点  %s\n    密码  %s\n2. 打开  http://%s/",
             wifi_prov_ssid(), PROV_AP_PASS, PROV_AP_IP);
    lv_label_set_text(t2, b);
    lv_obj_set_pos(t2, 0, 36);

    lv_obj_t *t3 = lv_label_create(card);
    /* ★ 必须是中文字体：ui_font_14 是 Montserrat，不含汉字字形，
     *   中文会全部渲染成方块（tofu）—— 上一版这里就是一排小矩形。 */
    lv_obj_set_style_text_font(t3, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(t3, lv_color_hex(0x8A97AD), 0);
    lv_label_set_text(t3, "保存后设备会自动重启并连接新网络");
    lv_obj_set_pos(t3, 0, 124);

    lv_obj_t *btn = lv_obj_create(card);
    lv_obj_set_size(btn, 140, 44);
    /* ★ 坐标是相对 card 的**内容区**（已扣掉 pad 24）：内容区只有 552×252。
     *   x=436+140=576 > 552、y=226+44=270 > 252 ⇒ 右/下被裁，剩一条横矩形。
     *   改成 x=380（+140=520）、y=176（+44=220），都在内。 */
    lv_obj_set_pos(btn, 380, 176);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x5B7CFA), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, prov_close_cb, LV_EVENT_CLICKED, mask);

    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(bl, "关闭");
    lv_obj_center(bl);
}

static void bl_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    backlight_set(v);
    if (s_lbl_bl != NULL)
    {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", v);
        lv_label_set_text(s_lbl_bl, b);
    }
}

/* ★ 11-20 深色模式开关：切色板 → 刷状态栏 → 重建本页（frame=0 会 lv_obj_clean 重画） */
static void theme_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool dark = lv_obj_has_state(sw, LV_STATE_CHECKED);
    /* ★ 只换色板 + 请求重建；状态栏刷新放到重建那一帧（build_system 末尾），
     *   否则会出现"顶部新色 + 页面旧色"的一帧混色。 */
    ui_theme_set(dark);
    ui_goto_scene(UI_SCENE_SYSTEM);
}

/* ★ 板载 RGB 灯调色（HUE/VAL 滑块实时驱动 WS2812） */
/* ★★ 11-21 教训：滑条 VALUE_CHANGED 每帧几十次，若在事件回调里直接走 RMT
 *   发 WS2812 波形（led_strip_refresh），RMT 与 LCD 共用 DMA，
 *   高频抢占会崩溃（乱屏 + 重启）。现在事件里只记值，由场景周期限速发送。 */
static volatile bool s_led_dirty;

static void led_cb(lv_event_t *e)
{
    (void)e;
    s_led_hue = (uint16_t)lv_slider_get_value(s_slider_hue);
    s_led_val = (uint8_t)lv_slider_get_value(s_slider_val);
    s_led_dirty = true;                     /* 交给 scene 周期发送 */
    lv_obj_set_style_bg_color(s_led_swatch,
                              lv_color_hsv_to_rgb(s_led_hue, 255, s_led_val), 0);
}

static lv_obj_t *s_root;   /* 整页容器：切页动画用 */

static void build_system(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;

    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);   /* 单色浅底（渐变会放大边缘闪烁） */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* HOME 返回 */
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 90, 32);
    lv_obj_set_pos(btn, 12, 6);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ui_theme()->card), 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(ui_theme()->shadow), 0);
    lv_obj_set_style_shadow_offset_y(btn, 2, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(ui_theme()->card_line), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " HOME");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &ui_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(title, "SYSTEM INFO");
    lv_obj_set_pos(title, 120, 6);

    /* 左面板：芯片 / 内存 */
    lv_obj_t *p1 = panel(scr, 16, 52, 470, 190, 0x2A3550);
    s_lbl_sys = lv_label_create(p1);
    lv_obj_set_style_text_font(s_lbl_sys, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_sys, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_sys, 6, 0);
    lv_label_set_text(s_lbl_sys, "reading...");

    /* 右面板：WiFi 状态 + 开关 */
    lv_obj_t *p2 = panel(scr, 500, 52, 284, 190, 0x2A3550);
    s_lbl_wifi = lv_label_create(p2);
    lv_obj_set_style_text_font(s_lbl_wifi, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_wifi, 6, 0);
    lv_label_set_text(s_lbl_wifi, "reading...");

    s_btn_wifi = lv_button_create(p2);
    lv_obj_set_size(s_btn_wifi, 240, 40);
    lv_obj_align(s_btn_wifi, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_btn_wifi, 10, 0);
    lv_obj_set_style_bg_color(s_btn_wifi, lv_color_hex(ui_theme()->card), 0);
    lv_obj_set_style_shadow_width(s_btn_wifi, 10, 0);
    lv_obj_set_style_shadow_opa(s_btn_wifi, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(s_btn_wifi, lv_color_hex(ui_theme()->shadow), 0);
    lv_obj_set_style_border_width(s_btn_wifi, 2, 0);
    lv_obj_set_style_border_color(s_btn_wifi, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_shadow_width(s_btn_wifi, 0, 0);
    s_lbl_btn = lv_label_create(s_btn_wifi);
    lv_obj_set_style_text_font(s_lbl_btn, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_btn, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(s_lbl_btn, "DISCONNECT WiFi");
    lv_obj_center(s_lbl_btn);
    lv_obj_add_event_cb(s_btn_wifi, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* 底部：最近日志 */
    lv_obj_t *p3 = panel(scr, 16, 248, 768, 228, 0x2A3550);
    /* ---- 背光调节（日志卡顶部一行，滑块加大到 320px） ---- */
    {
        lv_obj_t *bl = lv_label_create(p3);
        lv_obj_set_style_text_font(bl, &ui_font_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(bl, "BACKLIGHT");
        lv_obj_set_pos(bl, 0, 6);

        lv_obj_t *sl = lv_slider_create(p3);
        lv_obj_set_size(sl, 320, 12);
        lv_obj_set_pos(sl, 96, 8);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, backlight_get(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sl, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x6C4CF1), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, 6, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x6C4CF1), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
        lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(sl, 8, LV_PART_KNOB);
        lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(sl, lv_color_hex(0x6C4CF1), LV_PART_KNOB);
        lv_obj_add_event_cb(sl, bl_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_lbl_bl = lv_label_create(p3);
        lv_obj_set_style_text_font(s_lbl_bl, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_bl, lv_color_hex(COL_TEXT), 0);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", backlight_get());
        lv_label_set_text(s_lbl_bl, b);
        lv_obj_set_pos(s_lbl_bl, 430, 4);

        /* ---- 深色模式开关（背光行右侧） ---- */
        lv_obj_t *dk = lv_label_create(p3);
        lv_obj_set_style_text_font(dk, &ui_font_14, 0);
        lv_obj_set_style_text_color(dk, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(dk, "DARK MODE");
        lv_obj_set_pos(dk, 520, 8);

        lv_obj_t *sw = lv_switch_create(p3);
        lv_obj_set_size(sw, 52, 26);
        lv_obj_set_pos(sw, 610, 2);
        if (ui_theme_is_dark())
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* ---- 配网按钮（DARK MODE 右侧空位） ---- */
        lv_obj_t *pv = lv_obj_create(p3);
        lv_obj_set_size(pv, 96, 26);
        /* ★ 放 MIC 行右侧（x 640）：DARK MODE 开关占到 662，那行右侧已被挤满，
         *   硬塞会超出卡片内边距（756）被裁掉。MIC 数值标签只到 ~470，右边空着。 */
        lv_obj_set_pos(pv, 640, 36);
        lv_obj_set_style_bg_color(pv, lv_color_hex(0x5B7CFA), 0);
        lv_obj_set_style_bg_opa(pv, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pv, 8, 0);
        lv_obj_set_style_border_width(pv, 0, 0);
        lv_obj_set_style_pad_all(pv, 0, 0);
        lv_obj_add_flag(pv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(pv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(pv, prov_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *pvl = lv_label_create(pv);
        lv_obj_set_style_text_font(pvl, &ui_font_14, 0);
        lv_obj_set_style_text_color(pvl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(pvl, "WIFI");
        lv_obj_center(pvl);

        /* ---- 时钟/屏保按钮（WIFI 右边，DARK MODE 行的剩余空位） ----
         * ★ 宽 88 不是 96：x=664 起，664+88=752 < 内边距 756，刚好放下；
         *   96 宽会超出 4px 被裁。 */
        lv_obj_t *ck = lv_obj_create(p3);
        lv_obj_set_size(ck, 88, 26);
        lv_obj_set_pos(ck, 664, 2);
        lv_obj_set_style_bg_color(ck, lv_color_hex(0x35D07F), 0);
        lv_obj_set_style_bg_opa(ck, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ck, 8, 0);
        lv_obj_set_style_border_width(ck, 0, 0);
        lv_obj_set_style_pad_all(ck, 0, 0);
        lv_obj_add_flag(ck, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ck, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(ck, clock_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ckl = lv_label_create(ck);
        lv_obj_set_style_text_font(ckl, &ui_font_14, 0);
        lv_obj_set_style_text_color(ckl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(ckl, "CLOCK");
        lv_obj_center(ckl);
    }

    /* ---- 语音按钮（MIC 行右侧空位）+ 识别结果行 ---- */
    {
        lv_obj_t *vb = lv_obj_create(p3);
        lv_obj_set_size(vb, 100, 26);
        lv_obj_set_pos(vb, 520, 36);
        lv_obj_set_style_bg_color(vb, lv_color_hex(0xFF9A3D), 0);
        lv_obj_set_style_bg_opa(vb, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(vb, 8, 0);
        lv_obj_set_style_border_width(vb, 0, 0);
        lv_obj_set_style_pad_all(vb, 0, 0);
        lv_obj_add_flag(vb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(vb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(vb, voice_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *vbl = lv_label_create(vb);
        lv_obj_set_style_text_font(vbl, &ui_font_14, 0);
        lv_obj_set_style_text_color(vbl, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(vbl, "VOICE");
        lv_obj_center(vbl);
    }

    /* ---- 麦克风电平（日志卡第二行）：说话时条会跳 ---- */
    {
        lv_obj_t *mi = lv_label_create(p3);
        lv_obj_set_style_text_font(mi, &ui_font_14, 0);
        lv_obj_set_style_text_color(mi, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(mi, "MIC");
        lv_obj_set_pos(mi, 0, 40);

        s_bar_mic = lv_bar_create(p3);
        lv_obj_set_size(s_bar_mic, 320, 12);
        lv_obj_set_pos(s_bar_mic, 96, 42);

        /* 语音识别结果（VOICE 按钮触发后显示） */
        s_lbl_asr = lv_label_create(p3);
        lv_obj_set_style_text_font(s_lbl_asr, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_asr, lv_color_hex(ui_theme()->title), 0);
        lv_label_set_text(s_lbl_asr, "");
        lv_obj_set_pos(s_lbl_asr, 0, 66);
        lv_obj_set_width(s_lbl_asr, 740);
        lv_label_set_long_mode(s_lbl_asr, LV_LABEL_LONG_WRAP);
        lv_bar_set_range(s_bar_mic, 0, 100);
        lv_bar_set_value(s_bar_mic, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bar_mic, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar_mic, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(0x35D07F), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bar_mic, 6, LV_PART_INDICATOR);

        s_lbl_mic = lv_label_create(p3);
        lv_obj_set_style_text_font(s_lbl_mic, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_mic, lv_color_hex(COL_TEXT), 0);
        lv_label_set_text(s_lbl_mic, "0");
        lv_obj_set_pos(s_lbl_mic, 430, 38);
    }

    /* ---- 板载 RGB 灯调色（日志卡第三行）：HUE 滑块 + 亮度滑块 + 色块预览 ---- */
    {
        lv_obj_t *li = lv_label_create(p3);
        lv_obj_set_style_text_font(li, &ui_font_14, 0);
        lv_obj_set_style_text_color(li, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(li, "LED");
        lv_obj_set_pos(li, 0, 76);

        s_slider_hue = lv_slider_create(p3);
        lv_obj_set_size(s_slider_hue, 200, 12);
        lv_obj_set_pos(s_slider_hue, 96, 78);
        lv_slider_set_range(s_slider_hue, 0, 359);
        lv_slider_set_value(s_slider_hue, s_led_hue, LV_ANIM_OFF);   /* ★ 恢复上次状态 */
        lv_obj_set_style_bg_color(s_slider_hue, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slider_hue, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_slider_hue, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_hue, lv_color_hex(0xE8556D), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_slider_hue, 6, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(s_slider_hue, 3, LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_hue, led_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *vb = lv_label_create(p3);
        lv_obj_set_style_text_font(vb, &ui_font_14, 0);
        lv_obj_set_style_text_color(vb, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(vb, "VAL");
        lv_obj_set_pos(vb, 312, 76);

        s_slider_val = lv_slider_create(p3);
        lv_obj_set_size(s_slider_val, 150, 12);
        lv_obj_set_pos(s_slider_val, 356, 78);
        lv_slider_set_range(s_slider_val, 0, 255);
        lv_slider_set_value(s_slider_val, s_led_val, LV_ANIM_OFF);   /* ★ 恢复上次状态 */
        lv_obj_set_style_bg_color(s_slider_val, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slider_val, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_slider_val, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider_val, lv_color_hex(0xE8556D), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_slider_val, 6, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(s_slider_val, 3, LV_PART_KNOB);
        lv_obj_add_event_cb(s_slider_val, led_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_led_swatch = lv_obj_create(p3);   /* 色块预览 */
        lv_obj_set_size(s_led_swatch, 44, 20);
        lv_obj_set_pos(s_led_swatch, 530, 74);
        lv_obj_set_style_radius(s_led_swatch, 6, 0);
        lv_obj_set_style_border_width(s_led_swatch, 0, 0);
        lv_obj_clear_flag(s_led_swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_led_swatch,
                                  lv_color_hsv_to_rgb(s_led_hue, 255, s_led_val), 0);
    }

    /* 日志放进可滚动容器：内容超出可上下滑动，不会被裁掉半行 */
    lv_obj_t *logwrap = lv_obj_create(p3);
    lv_obj_set_pos(logwrap, 0, 124);
    lv_obj_set_size(logwrap, 744, 88);
    lv_obj_set_style_bg_opa(logwrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logwrap, 0, 0);
    lv_obj_set_style_pad_all(logwrap, 0, 0);
    lv_obj_set_style_radius(logwrap, 0, 0);
    lv_obj_set_scrollbar_mode(logwrap, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *cap = lv_label_create(p3);
    lv_obj_set_style_text_font(cap, &ui_font_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(cap, "RECENT LOG  (latest at bottom)");
    lv_obj_set_pos(cap, 0, 106);

    s_lbl_log = lv_label_create(logwrap);
    lv_obj_set_style_text_font(s_lbl_log, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_log, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_line_space(s_lbl_log, 1, 0);
    lv_obj_set_pos(s_lbl_log, 0, 0);
    lv_obj_set_width(s_lbl_log, 740);
    lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lbl_log, "(no log yet)");

    refresh_all();
}

void scene_system(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_system();
        ui_statusbar_refresh();   /* 重建完再刷状态栏：整屏同帧换色，无混色过渡 */
    }
    else if (frame % 25 == 0)     /* 约 500ms 刷新一次（原 1s，加快一倍） */
    {
        refresh_all();
    }

    /* 滑条 → WS2812：限速 ~16Hz，且只在 LVGL 场景循环里发（不在事件回调里） */
    if (s_led_dirty && (frame % 3) == 0)
    {
        s_led_dirty = false;
        bsp_led_set_hsv(s_led_hue, 255, s_led_val);
    }

    /* 麦克风电平单独快刷（100ms）：说话时条要跟得上，慢了像卡住 */
    if (frame > 0 && (frame % 5) == 0 && s_bar_mic != NULL)
    {
        int lv = audio_player_mic_level();
        lv_bar_set_value(s_bar_mic, lv, LV_ANIM_OFF);
        if (s_lbl_mic != NULL)
        {
            char b[8];
            snprintf(b, sizeof(b), "%d", lv);
            lv_label_set_text(s_lbl_mic, b);
        }

        /* 语音识别结果：voice 回调只置标志，这里（LVGL 上下文）消费 */
        if (s_asr_ready && s_lbl_asr != NULL)
        {
            s_asr_ready = false;
            lv_label_set_text(s_lbl_asr, s_asr_buf);
        }
    }

    lvgl_port_task();
}
