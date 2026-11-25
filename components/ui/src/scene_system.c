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
#include "audio_rec.h"
#include "backlight.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"
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

/* ★ 语音识别结果桥：voice 回调在 voice_task 上下文，绝不能直接碰 LVGL
 *   （lv_async_call 也死锁过）——只写标志，由本场景 draw 里轮询消费。 */
static lv_obj_t *s_lbl_mic;     /* 麦克风电平数值 */

/* ---------------- 回调 ---------------- */

/**
 * ★ 文本没变就不 set（2026-11-25）。
 *
 * LVGL 的 `lv_label_set_text` **不比较内容**，同一串也标脏。
 * 本页每 500ms 全量刷 18 个 label，再叠加每 100ms 的麦克风电平 ——
 * **实测撞到 `max=16 ovf=2`**：脏区达到 `MAX_DIRTY` 上限并溢出，
 * 溢出后 flush_cb 退化成"整屏 768KB sync"，这正是最该避免的情况。
 * 加了比对后 500ms 那批通常只剩 1~3 块真变的，整体远低于上限。
 */
static void set_text_if_changed(lv_obj_t *lbl, const char *s)
{
    if (lbl == NULL || s == NULL)
    {
        return;
    }
    const char *old = lv_label_get_text(lbl);
    if (old == NULL || strcmp(old, s) != 0)
    {
        lv_label_set_text(lbl, s);
    }
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);
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
    set_text_if_changed(s_lbl_sys, b);

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
    set_text_if_changed(s_lbl_wifi, b);
    lv_obj_set_style_text_color(s_lbl_wifi,
                                lv_color_hex(conn ? COL_TEXT : COL_RED), 0);

    /* 按钮文字随状态变 */
    set_text_if_changed(s_lbl_btn, conn ? "DISCONNECT WiFi" : "CONNECT WiFi");
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
    set_text_if_changed(s_lbl_log, b);
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

static void bl_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    backlight_set(v);
    if (s_lbl_bl != NULL)
    {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", v);
        set_text_if_changed(s_lbl_bl, b);
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
    set_text_if_changed(bl, LV_SYMBOL_LEFT " HOME");
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &ui_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    set_text_if_changed(title, "SYSTEM INFO");
    lv_obj_set_pos(title, 120, 6);

    /* 左面板：芯片 / 内存 */
    lv_obj_t *p1 = panel(scr, 16, 52, 470, 190, 0x2A3550);
    s_lbl_sys = lv_label_create(p1);
    lv_obj_set_style_text_font(s_lbl_sys, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_sys, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_sys, 6, 0);
    set_text_if_changed(s_lbl_sys, "reading...");

    /* 右面板：WiFi 状态 + 开关 */
    lv_obj_t *p2 = panel(scr, 500, 52, 284, 190, 0x2A3550);
    s_lbl_wifi = lv_label_create(p2);
    lv_obj_set_style_text_font(s_lbl_wifi, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_line_space(s_lbl_wifi, 6, 0);
    set_text_if_changed(s_lbl_wifi, "reading...");

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
    set_text_if_changed(s_lbl_btn, "DISCONNECT WiFi");
    lv_obj_center(s_lbl_btn);
    lv_obj_add_event_cb(s_btn_wifi, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* 底部：最近日志 */
    lv_obj_t *p3 = panel(scr, 16, 248, 768, 228, 0x2A3550);
    /* ---- 背光调节（日志卡顶部一行，滑块加大到 320px） ---- */
    {
        lv_obj_t *bl = lv_label_create(p3);
        lv_obj_set_style_text_font(bl, &ui_font_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(COL_DIM), 0);
        set_text_if_changed(bl, "BACKLIGHT");
        lv_obj_set_pos(bl, 0, 6);

        lv_obj_t *sl = lv_slider_create(p3);
        lv_obj_set_size(sl, 440, 12);   /* 删掉右侧按钮后加宽（留出 DARK MODE 位置） */
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
        set_text_if_changed(s_lbl_bl, b);
        lv_obj_set_pos(s_lbl_bl, 548, 4);

        /* ---- 深色模式开关（背光行右侧） ---- */
        lv_obj_t *dk = lv_label_create(p3);
        lv_obj_set_style_text_font(dk, &ui_font_14, 0);
        lv_obj_set_style_text_color(dk, lv_color_hex(COL_DIM), 0);
        set_text_if_changed(dk, "DARK MODE");
        lv_obj_set_pos(dk, 596, 8);

        lv_obj_t *sw = lv_switch_create(p3);
        lv_obj_set_size(sw, 52, 26);
        lv_obj_set_pos(sw, 688, 2);
        if (ui_theme_is_dark())
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);


    }

    /* ---- 麦克风电平（日志卡第二行）：说话时条会跳 ---- */
    {
        lv_obj_t *mi = lv_label_create(p3);
        lv_obj_set_style_text_font(mi, &ui_font_14, 0);
        lv_obj_set_style_text_color(mi, lv_color_hex(COL_DIM), 0);
        set_text_if_changed(mi, "MIC");
        lv_obj_set_pos(mi, 0, 40);

        s_bar_mic = lv_bar_create(p3);
        lv_obj_set_size(s_bar_mic, 440, 12);
        lv_obj_set_pos(s_bar_mic, 96, 42);

        /* 语音识别结果（VOICE 按钮触发后显示） */
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
        set_text_if_changed(s_lbl_mic, "0");
        lv_obj_set_pos(s_lbl_mic, 548, 38);
    }

    /* 日志放进可滚动容器：内容超出可上下滑动，不会被裁掉半行 */
    lv_obj_t *logwrap = lv_obj_create(p3);
    lv_obj_set_pos(logwrap, 0, 96);
    lv_obj_set_size(logwrap, 744, 112);
    lv_obj_set_style_bg_opa(logwrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logwrap, 0, 0);
    lv_obj_set_style_pad_all(logwrap, 0, 0);
    lv_obj_set_style_radius(logwrap, 0, 0);
    lv_obj_set_scrollbar_mode(logwrap, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *cap = lv_label_create(p3);
    lv_obj_set_style_text_font(cap, &ui_font_14, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(COL_DIM), 0);
    set_text_if_changed(cap, "RECENT LOG  (latest at bottom)");
    lv_obj_set_pos(cap, 0, 78);

    s_lbl_log = lv_label_create(logwrap);
    lv_obj_set_style_text_font(s_lbl_log, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_log, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_line_space(s_lbl_log, 1, 0);
    lv_obj_set_pos(s_lbl_log, 0, 0);
    lv_obj_set_width(s_lbl_log, 740);
    lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_WRAP);
    set_text_if_changed(s_lbl_log, "(no log yet)");

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

    /* 麦克风电平单独快刷（100ms）：说话时条要跟得上，慢了像卡住 */
    if (frame > 0 && (frame % 5) == 0 && s_bar_mic != NULL)
    {
        int lv = audio_rec_level();
        lv_bar_set_value(s_bar_mic, lv, LV_ANIM_OFF);
        if (s_lbl_mic != NULL)
        {
            char b[8];
            snprintf(b, sizeof(b), "%d", lv);
            set_text_if_changed(s_lbl_mic, b);
        }

    }

    lvgl_port_task();
}
