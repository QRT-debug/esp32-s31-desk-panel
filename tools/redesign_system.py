#!/usr/bin/env python3
"""把 scene_system.c 的设置页重做成手机风分组列表（一次性脚本，跑完可删）。"""
import re, sys

P = 'components/ui/src/scene_system.c'
src = open(P, encoding='utf-8').read()

# ---------- 1) 静态指针 ----------
old1 = '''static lv_obj_t *s_lbl_sys;      /* 左面板：芯片/内存/存储 */
static lv_obj_t *s_lbl_wifi;     /* 右面板：WiFi 详情 */
static lv_obj_t *s_lbl_log;      /* 底部：最近日志 */
static lv_obj_t *s_lbl_btn;      /* WiFi 按钮文字 */
static lv_obj_t *s_btn_wifi;
static lv_obj_t *s_lbl_bl;      /* 背光百分比 */
static lv_obj_t *s_bar_mic;'''
new1 = '''/* ---- 手机风设置页：每个列表行一个值 label（2026-11-26 重做） ---- */
static lv_obj_t *s_v_wifi;       /* 网络：连接状态 */
static lv_obj_t *s_v_ip;         /* 网络：IP */
static lv_obj_t *s_v_cfg;        /* 网络：配网行右侧提示 */
static lv_obj_t *s_v_chip;       /* 设备：芯片 */
static lv_obj_t *s_v_mhz;        /* 设备：主频 */
static lv_obj_t *s_v_ram;        /* 设备：SRAM */
static lv_obj_t *s_v_psram;      /* 设备：PSRAM */
static lv_obj_t *s_v_flash;      /* 设备：闪存 */
static lv_obj_t *s_v_up;         /* 设备：运行时间 */
static lv_obj_t *s_v_core;       /* 设备：双核占用 */
static lv_obj_t *s_lbl_log;      /* 日志 */
static lv_obj_t *s_lbl_bl;       /* 亮度百分比 */
static lv_obj_t *s_bar_mic;      /* 麦克风电平 */
static lv_obj_t *s_lbl_mic;      /* 麦克风数值 */'''
assert old1 in src, 'anchor1'
src = src.replace(old1, new1, 1)

# ---------- 2) refresh_all 的显示部分 ----------
i = src.index('    snprintf(b, sizeof(b),\n             "CHIP      ESP32-S31')
j = src.index('lv_color_hex(conn ? COL_RED : COL_GREEN), 0);', i)
j = src.index('\n', j) + 1
new2 = '''    /* ---- 设备组（每行一个值，都是短文本） ---- */
    snprintf(b, sizeof(b), "ESP32-S31");
    set_text_if_changed(s_v_chip, b);

    snprintf(b, sizeof(b), "%u MHz \\u00b7 %d \\u6838", (unsigned)cpu_mhz, chip.cores);
    set_text_if_changed(s_v_mhz, b);

    snprintf(b, sizeof(b), "%u/%u KB", (unsigned)(ram_free / 1024), (unsigned)(ram_total / 1024));
    set_text_if_changed(s_v_ram, b);

    snprintf(b, sizeof(b), "%u.%u/%u.%u MB",
             (unsigned)(ps_free / 1048576), (unsigned)((ps_free % 1048576) * 10 / 1048576),
             (unsigned)(ps_total / 1048576), (unsigned)((ps_total % 1048576) * 10 / 1048576));
    set_text_if_changed(s_v_psram, b);

    snprintf(b, sizeof(b), "%02d:%02d:%02d  %u \\u4efb\\u52a1", up_h, up_m, up_sec, (unsigned)tasks);
    set_text_if_changed(s_v_up, b);

    snprintf(b, sizeof(b), "c0 %d%%   c1 %d%%", c0, c1);
    set_text_if_changed(s_v_core, b);

    /* ---- 网络组 ---- */
    set_text_if_changed(s_v_wifi, conn ? "\\u5df2\\u8fde\\u63a5" : "\\u672a\\u8fde\\u63a5");
    lv_obj_set_style_text_color(s_v_wifi, lv_color_hex(conn ? COL_GREEN : COL_RED), 0);

    set_text_if_changed(s_v_ip, ip[0] ? ip : "\\u2014");
    set_text_if_changed(s_v_cfg, conn ? "\\u66f4\\u6362\\u7f51\\u7edc \\u203a" : "\\u53bb\\u914d\\u7f51 \\u203a");
'''
src = src[:i] + new2 + src[j:]

# ---------- 3) build_system 整体替换 ----------
k = src.index('static void build_system(void)')
k = src.rindex('/*', 0, k)          # 连前面的注释一起换
end_anchor = '    refresh_all();\n}'
e = src.index(end_anchor, k) + len(end_anchor)

new3 = r'''/* ---------------- 建界面：手机设置页风格（2026-11-26 重做） ----------------
 * 结构：顶部返回 + 标题；下面左右两列分组卡片，每组「小标题 + 圆角卡片 + 若干列表行」。
 * 行样式统一为「彩色圆点 + 中文标题 + 右侧灰色值」，分隔线左缩进（iOS 风格）。
 * ★ 中文一律 ui_font_cn18（其他字体是 ASCII 字模，中文会变方块）。 */

static void group_caption(lv_obj_t *scr, int x, int y, const char *t)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
    lv_obj_set_pos(l, x + 4, y);
    lv_label_set_text(l, t);
}

static lv_obj_t *group_card(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void row_sep(lv_obj_t *card, int y, int x0, int w)
{
    lv_obj_t *s = lv_obj_create(card);
    lv_obj_set_pos(s, x0, y);
    lv_obj_set_size(s, w, 1);
    lv_obj_set_style_bg_color(s, lv_color_hex(ui_theme()->card_line), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_radius(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/** 行左侧的彩色小圆点 */
static void row_dot(lv_obj_t *card, int y, int row_h, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(card);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_pos(d, 16, y + (row_h - 8) / 2);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/** 行标题 */
static void row_title(lv_obj_t *card, int y, int row_h, const char *t)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_font(l, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(l, 36, y + (row_h - 24) / 2);
    lv_label_set_text(l, t);
}

/** 标准行：圆点 + 标题 + 右侧值；返回值 label */
static lv_obj_t *setting_row(lv_obj_t *card, int y, int row_h, uint32_t dot,
                             const char *title, const char *value)
{
    row_dot(card, y, row_h, dot);
    row_title(card, y, row_h, title);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_font(v, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(COL_DIM), 0);
    lv_obj_set_width(v, 200);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(v, 160, y + (row_h - 24) / 2);
    lv_label_set_text(v, value);
    return v;
}

/** 整行可点：透明命中层盖在该行上（不挡视觉，只接点击） */
static void row_clickable(lv_obj_t *card, int y, int row_h, int w, lv_event_cb_t cb)
{
    lv_obj_t *hit = lv_obj_create(card);
    lv_obj_set_pos(hit, 0, y);
    lv_obj_set_size(hit, w, row_h);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_set_style_pad_all(hit, 0, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);
}

/** 打开配网热点（SoftAP + 网页表单，用手机连） */
static void prov_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = wifi_prov_start();
    ESP_LOGI("system", "prov start: %s", esp_err_to_name(err));
}

static void build_system(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;

    /* ---- 顶栏：圆形返回 + 标题 ---- */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 12, 8);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(ui_theme()->card), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *ba = lv_label_create(back);
    lv_obj_set_style_text_font(ba, &ui_font_20, 0);
    lv_obj_set_style_text_color(ba, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ba, LV_SYMBOL_LEFT);
    lv_obj_center(ba);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(title, "设置");
    lv_obj_set_pos(title, 58, 14);

    /* ================= 左列 ================= */
    group_caption(scr, 16, 52, "网络");
    lv_obj_t *c_net = group_card(scr, 16, 76, 376, 132);
    s_v_wifi = setting_row(c_net, 0, 44, COL_BLUE, "WiFi", "未连接");
    row_sep(c_net, 44, 36, 324);
    s_v_ip = setting_row(c_net, 44, 44, COL_BLUE, "IP 地址", "—");
    row_sep(c_net, 88, 36, 324);
    s_v_cfg = setting_row(c_net, 88, 44, COL_BLUE, "重新配网", "打开热点 ›");
    row_clickable(c_net, 0, 44, 376, wifi_toggle_cb);   /* 第一行：断开 / 重连 */
    row_clickable(c_net, 88, 44, 376, prov_cb);         /* 第三行：开热点配网 */

    group_caption(scr, 16, 218, "显示");
    lv_obj_t *c_disp = group_card(scr, 16, 242, 376, 100);

    /* 亮度行 */
    row_dot(c_disp, 0, 50, COL_ORANGE);
    row_title(c_disp, 0, 50, "亮度");
    {
        lv_obj_t *sl = lv_slider_create(c_disp);
        lv_obj_set_size(sl, 140, 10);
        lv_obj_set_pos(sl, 110, 20);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, backlight_get(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sl, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
        lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_add_event_cb(sl, bl_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_lbl_bl = lv_label_create(c_disp);
        lv_obj_set_style_text_font(s_lbl_bl, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_bl, lv_color_hex(COL_DIM), 0);
        lv_obj_set_width(s_lbl_bl, 90);
        lv_obj_set_style_text_align(s_lbl_bl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_lbl_bl, 270, 14);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", backlight_get());
        set_text_if_changed(s_lbl_bl, b);
    }
    row_sep(c_disp, 50, 36, 324);

    /* 深色模式行 */
    row_dot(c_disp, 50, 50, COL_ORANGE);
    row_title(c_disp, 50, 50, "深色模式");
    {
        lv_obj_t *sw = lv_switch_create(c_disp);
        lv_obj_set_size(sw, 50, 26);
        lv_obj_set_pos(sw, 306, 12 + 50);
        if (ui_theme_is_dark())
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ================= 右列 ================= */
    group_caption(scr, 408, 52, "设备");
    lv_obj_t *c_dev = group_card(scr, 408, 76, 376, 266);
    s_v_chip  = setting_row(c_dev, 0,   38, COL_GREEN, "芯片",  "ESP32-S31");
    s_v_mhz   = setting_row(c_dev, 38,  38, COL_GREEN, "主频",  "—");
    s_v_ram   = setting_row(c_dev, 76,  38, COL_GREEN, "内存",  "—");
    s_v_psram = setting_row(c_dev, 114, 38, COL_GREEN, "PSRAM", "—");
    s_v_flash = setting_row(c_dev, 152, 38, COL_GREEN, "闪存",  "16 MB");
    s_v_up    = setting_row(c_dev, 190, 38, COL_GREEN, "运行",  "—");
    s_v_core  = setting_row(c_dev, 228, 38, COL_GREEN, "双核",  "—");

    group_caption(scr, 408, 352, "日志");
    lv_obj_t *c_log = group_card(scr, 408, 376, 376, 90);

    /* 麦克风电平（日志卡顶部一行） */
    row_title(c_log, 0, 30, "麦克风");
    {
        s_bar_mic = lv_bar_create(c_log);
        lv_obj_set_size(s_bar_mic, 190, 10);
        lv_obj_set_pos(s_bar_mic, 110, 10);
        lv_bar_set_range(s_bar_mic, 0, 100);
        lv_bar_set_value(s_bar_mic, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bar_mic, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar_mic, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bar_mic, 5, LV_PART_INDICATOR);

        s_lbl_mic = lv_label_create(c_log);
        lv_obj_set_style_text_font(s_lbl_mic, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_mic, lv_color_hex(COL_DIM), 0);
        lv_obj_set_pos(s_lbl_mic, 310, 4);
        set_text_if_changed(s_lbl_mic, "0");
    }

    s_lbl_log = lv_label_create(c_log);
    lv_obj_set_style_text_font(s_lbl_log, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_log, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_line_space(s_lbl_log, 0, 0);
    lv_obj_set_pos(s_lbl_log, 16, 34);
    lv_obj_set_width(s_lbl_log, 344);
    lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_WRAP);
    set_text_if_changed(s_lbl_log, "(no log yet)");

    refresh_all();
}'''

src = src[:k] + new3 + src[e:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('done; lines =', src.count('\n'))
