/**
 * @file    scene_weather.c
 * @brief   天气主页面（2026-11-20 新增，成为开机首页）
 *
 * 布局（800x480，顶部状态栏由 layer_top 叠加）：
 *   左块：城市 + 大温度（48 号）+ 描述 + 更新时间
 *   右块：极简天气图标（太阳/云/雨三态，色圆 + 射线/雨滴）+ 今天高低温
 *   中部：湿度 / 风速 / 体感 三张小卡
 *   底部：dock 四钮（应用 / 监控 / 音乐 / 系统）——原首页降级为"APPS"页
 *
 * 数据：weather.h（wttr.in，30 分钟自动刷新；点右上刷新图标立即拉取）
 * 主题：全部色值走 ui_theme()，深浅色自动适配
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "weather.h"

#define C_BG      (ui_theme()->bg_top)
#define C_CARD    (ui_theme()->card)
#define C_TITLE   (ui_theme()->title)
#define C_SUB     (ui_theme()->sub)
#define C_FAINT   (ui_theme()->faint)
#define C_SHADOW  (ui_theme()->shadow)
#define C_ACCENT  (ui_theme()->accent)

/* 天气图标用色（品牌恒定） */
#define WX_SUN    0xF2A03D
#define WX_CLOUD  0x9AA6BC
#define WX_RAIN   0x4C9AFF

static lv_obj_t *s_city;
static lv_obj_t *s_temp;
static lv_obj_t *s_desc;
static lv_obj_t *s_upd;
static lv_obj_t *s_hl_today;
static lv_obj_t *s_hl_tmrw;
static lv_obj_t *s_icon;      /* 图标容器（整块重建换形态） */
static lv_obj_t *s_hum_val;
static lv_obj_t *s_wind_val;
static lv_obj_t *s_aqi_val;
static lv_obj_t *s_feel_val;
static lv_obj_t *s_state_lbl; /* 加载中/失败提示 */

/* ---------------- 导航回调 ---------------- */

static void to_apps_cb(lv_event_t *e)    { (void)e; ui_goto_scene(UI_SCENE_APPS); }
static void to_monitor_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_MONITOR); }
static void to_audio_cb(lv_event_t *e)   { (void)e; ui_goto_scene(UI_SCENE_AUDIO); }
static void to_system_cb(lv_event_t *e)  { (void)e; ui_goto_scene(UI_SCENE_SYSTEM); }

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    weather_refresh_now();
}

/* ---------------- 图标绘制（太阳 / 云 / 雨 三态） ---------------- */

static void icon_clear(void)
{
    lv_obj_clean(s_icon);
}

static lv_obj_t *make_dot(lv_obj_t *parent, int x, int y, int dia, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, dia, dia);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_radius(d, dia / 2, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

/* 太阳：黄圆 + 8 条短射线 */
static void draw_sun(void)
{
    lv_obj_t *sun = make_dot(s_icon, 34, 34, 72, WX_SUN);
    (void)sun;
    static const int rays[8][4] = {
        /* x, y, w, h —— 上/下/左/右 + 四斜向 */
        {67, 8, 6, 20}, {67, 112, 6, 20},
        {8, 67, 20, 6}, {112, 67, 20, 6},
        {26, 26, 14, 6}, {100, 26, 14, 6},
        {26, 108, 14, 6}, {100, 108, 14, 6},
    };
    for (int i = 0; i < 8; i++)
    {
        lv_obj_t *r = lv_obj_create(s_icon);
        lv_obj_set_size(r, rays[i][2], rays[i][3]);
        lv_obj_set_pos(r, rays[i][0], rays[i][1]);
        lv_obj_set_style_radius(r, 3, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(WX_SUN), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* 云：两个大圆 + 一条底边 */
static void draw_cloud(bool with_rain)
{
    make_dot(s_icon, 22, 42, 56, WX_CLOUD);
    make_dot(s_icon, 58, 30, 68, WX_CLOUD);
    make_dot(s_icon, 90, 52, 44, WX_CLOUD);
    lv_obj_t *base = lv_obj_create(s_icon);
    lv_obj_set_size(base, 104, 22);
    lv_obj_set_pos(base, 30, 76);    /* 底面 98 = 三个圆的共同底部，不得更低，否则像云下挂着横梁 */
    lv_obj_set_style_radius(base, 11, 0);
    lv_obj_set_style_bg_color(base, lv_color_hex(WX_CLOUD), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_set_style_pad_all(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);

    if (with_rain)
    {
        /* 雨滴：细长条，从云底(98)下方 6px 起，长短错落更像下雨 */
        static const int drops[3][3] = { { 46, 104, 20 }, { 72, 108, 26 }, { 98, 104, 20 } };
        for (int i = 0; i < 3; i++)
        {
            lv_obj_t *d = lv_obj_create(s_icon);
            lv_obj_set_size(d, 5, drops[i][2]);
            lv_obj_set_pos(d, drops[i][0], drops[i][1]);
            lv_obj_set_style_radius(d, 3, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(WX_RAIN), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_pad_all(d, 0, 0);
            lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

/* 按描述关键字选形态（中文描述来自 wttr.in lang=zh） */
static char s_last_desc[40] = { 0 };   /* 图标重画判据；frame==0 清空强制重画 */

static void icon_update(const char *desc)
{
    icon_clear();
    if (desc == NULL || desc[0] == 0)
    {
        draw_cloud(false);
        return;
    }
    /* ★ 和风（中文）+ wttr（英文）双源匹配：中文按 UTF-8 字节 strstr 即可 */
    if (strstr(desc, "\u6674") != NULL || strstr(desc, "Clear") != NULL ||
        strstr(desc, "Sunny") != NULL)
    {
        draw_sun();
    }
    else if (strstr(desc, "\u96e8") != NULL || strstr(desc, "\u96ea") != NULL ||
             strstr(desc, "\u96f7") != NULL || strstr(desc, "shower") != NULL ||
             strstr(desc, "Rain") != NULL || strstr(desc, "Snow") != NULL ||
             strstr(desc, "Thunder") != NULL || strstr(desc, "Sleet") != NULL)
    {
        draw_cloud(true);
    }
    else
    {
        draw_cloud(false);
    }
}

/* ---------------- 温度色标（蓝冷 / 青凉 / 绿舒适 / 橙热） ---------------- */

static uint32_t temp_color(int t)
{
    if (t <= 0)  { return 0x5AA9FF; }
    if (t <= 12) { return 0x54C7B0; }
    if (t <= 26) { return 0x4FCB62; }
    return 0xFF9A3D;
}

/* ---------------- 天气渐变背景（深/浅主题各一套） ----------------
 * 晴 = 暖橙、雨雪 = 蓝灰、多云/其它 = 淡蓝灰。渐变方向自上而下。 */
static void bg_apply(const char *desc)
{
    bool dark = ui_theme_is_dark();
    uint32_t top, bot;

    bool sunny = desc != NULL &&
                 (strstr(desc, "\u6674") != NULL || strstr(desc, "Clear") != NULL ||
                  strstr(desc, "Sunny") != NULL);
    bool wet = desc != NULL &&
               (strstr(desc, "\u96e8") != NULL || strstr(desc, "\u96ea") != NULL ||
                strstr(desc, "\u96f7") != NULL || strstr(desc, "shower") != NULL ||
                strstr(desc, "Rain") != NULL || strstr(desc, "Snow") != NULL ||
                strstr(desc, "Thunder") != NULL || strstr(desc, "Sleet") != NULL ||
                strstr(desc, "Sleet") != NULL || strstr(desc, "shower") != NULL);

    /* ★ 渐变区间刻意压窄（色阶差 15~30 级）：480px 高度上每级色阶对应 15+ 行，
     *   否则 8bit bpp 会出现肉眼可见的横向色带（banding）。 */
    if (sunny)
    {
        top = dark ? 0x33261A : 0xFFDEB0;
        bot = dark ? 0x241A0E : 0xFFC995;
    }
    else if (wet)
    {
        top = dark ? 0x141F2C : 0xC0D6EA;
        bot = dark ? 0x0C141C : 0x9DBBD8;
    }
    else
    {
        top = dark ? 0x1C2532 : 0xE6EEF8;
        bot = dark ? 0x121A24 : 0xC6D5E6;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(bot), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

/* ---------------- 指标小卡 ---------------- */

static lv_obj_t *make_metric(lv_obj_t *parent, int x, const char *title, lv_obj_t **val_out)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 176, 92);
    lv_obj_set_pos(c, x, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_shadow_width(c, 14, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(C_SHADOW), 0);
    lv_obj_set_style_shadow_offset_y(c, 3, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, &ui_font_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_SUB), 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 0, 0);

    lv_obj_t *v = lv_label_create(c);
    lv_obj_set_style_text_font(v, &ui_font_28, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(v, "--");
    lv_obj_set_pos(v, 0, 24);
    *val_out = v;
    return c;
}

/* ---------------- dock ---------------- */

static void make_dock_btn(lv_obj_t *parent, int x, int y, const char *sym, const char *txt,
                          lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 150, 44);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 10, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(C_SHADOW), 0);
    lv_obj_set_style_shadow_offset_y(b, 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    /* 符号必须用 LVGL 内置 montserrat（含 symbol 字形），Segoe 字体没有这些码位 */
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);

    lv_obj_t *si = lv_label_create(b);
    lv_obj_set_style_text_font(si, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(si, lv_color_hex(C_SUB), 0);
    lv_label_set_text(si, sym);

    lv_obj_t *tx = lv_label_create(b);
    lv_obj_set_style_text_font(tx, &ui_font_14, 0);
    lv_obj_set_style_text_color(tx, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(tx, txt);
}

/* ---------------- 建界面 ---------------- */

static void build_weather(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 左块：城市 + 大温度 + 描述 + 更新时间 ---- */
    s_city = lv_label_create(scr);
    /* ★ 城市名是中文：用 24px 专用中文小字库（含常见城市名字，体积 ~80KB） */
    lv_obj_set_style_text_font(s_city, &ui_font_city24, 0);
    lv_obj_set_style_text_color(s_city, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_city, "Locating...");
    lv_obj_set_pos(s_city, 40, 48);

    s_temp = lv_label_create(scr);
    lv_obj_set_style_text_font(s_temp, &ui_font_32, 0);
    lv_obj_set_style_text_color(s_temp, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(s_temp, "--");
    lv_obj_set_pos(s_temp, 40, 80);   /* 与城市名左对齐（原来 32 偏左） */

    s_desc = lv_label_create(scr);
    /* ★ 描述来自和风是中文：用 28px 专用中文小字库（含天气描述用字） */
    lv_obj_set_style_text_font(s_desc, &ui_font_wx28, 0);
    lv_obj_set_style_text_color(s_desc, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(s_desc, "");
    lv_obj_set_pos(s_desc, 40, 152);

    s_upd = lv_label_create(scr);
    lv_obj_set_style_text_font(s_upd, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_upd, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_text_opa(s_upd, LV_OPA_70, 0);
    lv_label_set_text(s_upd, "");
    lv_obj_set_pos(s_upd, 40, 194);

    s_state_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_state_lbl, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_state_lbl, "Loading weather...");
    lv_obj_set_pos(s_state_lbl, 40, 216);

    /* ---- 右块：图标 + 高低温 ---- */
    s_icon = lv_obj_create(scr);
    lv_obj_set_size(s_icon, 140, 140);
    lv_obj_set_pos(s_icon, 520, 32);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_icon, 0, 0);
    lv_obj_set_style_pad_all(s_icon, 0, 0);
    lv_obj_set_style_radius(s_icon, 0, 0);
    lv_obj_clear_flag(s_icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    draw_cloud(false);

    s_hl_today = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hl_today, &ui_font_20, 0);
    lv_obj_set_style_text_color(s_hl_today, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_hl_today, "");
    lv_obj_set_pos(s_hl_today, 540, 176);

    s_hl_tmrw = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hl_tmrw, &ui_font_20, 0);
    lv_obj_set_style_text_color(s_hl_tmrw, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_hl_tmrw, "");
    lv_obj_set_pos(s_hl_tmrw, 540, 204);

    /* 右上刷新按钮（图标） */
    {
        lv_obj_t *rb = lv_button_create(scr);
        lv_obj_set_size(rb, 44, 44);
        lv_obj_set_pos(rb, 736, 40);
        lv_obj_set_style_radius(rb, 22, 0);
        lv_obj_set_style_bg_color(rb, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(rb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rb, 0, 0);
        lv_obj_set_style_shadow_width(rb, 8, 0);
        lv_obj_set_style_shadow_opa(rb, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(rb, lv_color_hex(C_SHADOW), 0);
        lv_obj_add_event_cb(rb, refresh_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(rb);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_ACCENT), 0);
        lv_label_set_text(l, LV_SYMBOL_REFRESH);
        lv_obj_center(l);
    }

    /* ---- 中部三指标卡 ---- */
    {
        lv_obj_t *row = lv_obj_create(scr);
        lv_obj_set_size(row, 740, 92);
        lv_obj_set_pos(row, 40, 272);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        make_metric(row, 0,   "HUMIDITY", &s_hum_val);
        make_metric(row, 188, "WIND",     &s_wind_val);
        make_metric(row, 376, "FEELS",    &s_feel_val);
        make_metric(row, 564, "AIR AQI",  &s_aqi_val);   /* 空气质量（open-meteo） */
    }

    bg_apply(NULL);   /* 首帧默认渐变（多云），数据到达后随描述切换 */
    s_last_desc[0] = 0;   /* ★ 进页清残留：强制本帧重画图标/背景（防 static 残留） */

    /* ---- 底部 dock ---- */
    {
        int x0 = 40;
        int y0 = 404;
        make_dock_btn(scr, x0 + 0,   y0, LV_SYMBOL_LIST,     "APPS", to_apps_cb);
        make_dock_btn(scr, x0 + 160, y0, LV_SYMBOL_DRIVE,    "MONITOR", to_monitor_cb);
        make_dock_btn(scr, x0 + 320, y0, LV_SYMBOL_AUDIO,    "MUSIC", to_audio_cb);
        make_dock_btn(scr, x0 + 480, y0, LV_SYMBOL_SETTINGS, "SYSTEM", to_system_cb);
    }
}

/* ---------------- 刷新（每帧调用，内部限频） ---------------- */

static void refresh_ui(void)
{
    wx_data_t w;
    weather_get(&w);

    char b[48];

    if (w.state == WX_STATE_INIT || w.state == WX_STATE_LOADING)
    {
        lv_label_set_text(s_state_lbl, "Loading weather...");
    }
    else if (w.state == WX_STATE_FAIL)
    {
        lv_label_set_text_fmt(s_state_lbl, "Failed (http %d), retry in 5min", w.http_code);
    }
    else
    {
        lv_label_set_text(s_state_lbl, "");
    }

    if (w.temp_c != 0 || w.state == WX_STATE_OK)
    {
        snprintf(b, sizeof(b), "%d°", w.temp_c);
        lv_label_set_text(s_temp, b);
        lv_obj_set_style_text_color(s_temp, lv_color_hex(temp_color(w.temp_c)), 0);
    }
    if (w.city[0])
    {
        lv_label_set_text(s_city, w.city);
    }
    if (w.desc[0])
    {
        lv_label_set_text(s_desc, w.desc);
    }
    if (w.updated[0])
    {
        snprintf(b, sizeof(b), "Updated %s", w.updated);
        lv_label_set_text(s_upd, b);
    }
    if (w.today_max != -99)
    {
        snprintf(b, sizeof(b), "Today  \u2191%d\u00b0  \u2193%d\u00b0", w.today_max, w.today_min);
        lv_label_set_text(s_hl_today, b);
        snprintf(b, sizeof(b), "Tmrw   \u2191%d\u00b0  \u2193%d\u00b0", w.tmrw_max, w.tmrw_min);
        lv_label_set_text(s_hl_tmrw, b);
    }
    if (w.humidity >= 0)
    {
        snprintf(b, sizeof(b), "%d%%", w.humidity);
        lv_label_set_text(s_hum_val, b);
    }
    if (w.aqi > 0)
    {
        snprintf(b, sizeof(b), "%d", w.aqi);
        lv_label_set_text(s_aqi_val, b);
        /* 分级配色（US AQI）：好=绿 中等=黄 敏感=橙 不健康=红 很差=紫 */
        uint32_t col = 0x4CAF50;
        if (w.aqi > 300)
        {
            col = 0x7E3B3B;
        }
        else if (w.aqi > 200)
        {
            col = 0x8E44AD;
        }
        else if (w.aqi > 150)
        {
            col = 0xE0443E;
        }
        else if (w.aqi > 100)
        {
            col = 0xF57C00;
        }
        else if (w.aqi > 50)
        {
            col = 0xE6A700;
        }
        lv_obj_set_style_text_color(s_aqi_val, lv_color_hex(col), 0);
    }

    if (w.wind_kmph >= 0)
    {
        snprintf(b, sizeof(b), "%d km/h", w.wind_kmph);
        lv_label_set_text(s_wind_val, b);
    }
    if (w.feels_c != -99)
    {
        snprintf(b, sizeof(b), "%d°", w.feels_c);
        lv_label_set_text(s_feel_val, b);
    }

    /* 图标只在描述变化时重建（每 500ms 重建会闪）。
     * ★ s_last_desc 是文件级变量（见上），frame==0 清空 ⇒ 每次进页强制重画一次。
     *   曾因它用函数内 static 跨场景残留：数据在屏保期间刷新了、残留值卡住
     *   变化判断 ⇒ 重进天气页时文字已是 Sunny、图标还是旧的乌云（实测踩到）。 */
    if (strcmp(s_last_desc, w.desc) != 0)
    {
        snprintf(s_last_desc, sizeof(s_last_desc), "%s", w.desc);
        icon_update(w.desc);
        bg_apply(w.desc);
    }

}

void scene_weather(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_weather();
        refresh_ui();
    }
    else if ((frame % 25) == 0)    /* 约 500ms 刷一次 */
    {
        refresh_ui();
    }

    lvgl_port_task();
}
