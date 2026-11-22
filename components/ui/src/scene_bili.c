/**
 * @file    scene_bili.c
 * @brief   B 站热搜榜（复古终端风，10 条一屏）
 *
 * 布局（800x480）：
 *   y06  ← 返回                        （状态栏在右上 layer_top）
 *   y36  BILI / HOT SEARCH      UPD 21:15  OK  [刷新]
 *   y84  TRENDING NOW / TOP 10
 *   y108 分隔线
 *   y116 起 10 行 × 34：序号(#01, 14号) + 热搜词(20号中文) + 热度(14号, 右)
 *   y458 页脚
 *
 * ★ 缺字处理：热搜词常有生僻字，字库（GB2312 一级 3755 字）可能没有
 *   ⇒ 逐字检查，缺的字替换成 '?'（保留条目和长度，不出现豆腐块）。
 * ★ 架构：行槽预建、刷新只改文字（同 HF/AI 榜）。
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
#include "bili_hot.h"

#define BILI_ROWS  10
#define BILI_ROW_H 34

#define C_BILI 0xFB7299   /* B 站粉 */

typedef struct
{
    uint32_t bg, fg, dim, line, card;
} bl_pal_t;

static bl_pal_t pal(void)
{
    if (ui_theme_is_dark())
    {
        bl_pal_t p = { 0x120E10, 0xE8D5DB, 0x8A7580, 0x2E2227, 0x1A1417 };
        return p;
    }
    bl_pal_t p = { 0xFDF2F5, 0x241A1E, 0x6E5A62, 0xE3C8D0, 0xFFFFFF };
    return p;
}

typedef struct
{
    lv_obj_t *rk;
    lv_obj_t *word;
    lv_obj_t *heat;
} bl_row_t;

static bl_row_t  s_row[BILI_ROWS];
static lv_obj_t *s_upd;
static lv_obj_t *s_state;
static int       s_last_count = -1;
static int       s_last_state = -1;
static unsigned  s_last_gen;

static void refresh_cb(lv_event_t *e);

static lv_obj_t *mk_label(lv_obj_t *parent, int x, int y, const lv_font_t *f,
                          uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void mk_hline(lv_obj_t *parent, int x, int y, int w, uint32_t color)
{
    lv_obj_t *ln = lv_obj_create(parent);
    lv_obj_set_size(ln, w, 1);
    lv_obj_set_pos(ln, x, y);
    lv_obj_set_style_bg_color(ln, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_set_style_radius(ln, 0, 0);
    lv_obj_set_style_pad_all(ln, 0, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* UTF-8 解码（返回消耗字节数） */
static int u8_next(const uint8_t *p, uint32_t *cp)
{
    if (p[0] < 0x80)
    {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0)
    {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0)
    {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    *cp = p[0];
    return 1;
}

/* 把字库缺的字替换为 '?'，并按 UTF-8 边界截断到 max_chars 个字符 */
static void sanitize(const char *src, char *dst, size_t dsz, int max_chars)
{
    const uint8_t *p = (const uint8_t *)src;
    size_t o = 0;
    int    ch = 0;

    while (*p && o + 5 < dsz && ch < max_chars)
    {
        uint32_t cp;
        int adv = u8_next(p, &cp);
        lv_font_glyph_dsc_t dsc;

        if (cp < 0x80 || lv_font_get_glyph_dsc(&ui_font_cn18, &dsc, cp, 0))
        {
            if (o + (size_t)adv < dsz)
            {
                memcpy(dst + o, p, (size_t)adv);
                o += (size_t)adv;
            }
        }
        else
        {
            dst[o++] = '?';     /* 缺字降级：保留位置，不出现豆腐块 */
        }
        p += adv;
        ch++;
    }
    dst[o] = 0;
}

/* 热度缩写：39354137 → 39.4M */
static void fmt_heat(long v, char *out, size_t osz)
{
    if (v >= 1000000L)
    {
        snprintf(out, osz, "%.1fM", v / 1000000.0);
    }
    else if (v >= 1000L)
    {
        snprintf(out, osz, "%.1fK", v / 1000.0);
    }
    else
    {
        snprintf(out, osz, "%ld", v);
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    bili_refresh_now();
}

static void build_bili(void)
{
    bl_pal_t p = pal();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* ★★★ 11-22：同 scene_hf —— clean 后清空静态指针，防野指针 */
    memset(s_row, 0, sizeof(s_row));
    s_upd   = NULL;
    s_state = NULL;

    lv_obj_set_style_bg_color(scr, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上返回 */
    {
        lv_obj_t *bb = lv_button_create(scr);
        lv_obj_set_size(bb, 46, 26);
        lv_obj_set_pos(bb, 32, 6);
        lv_obj_set_style_radius(bb, 4, 0);
        lv_obj_set_style_bg_color(bb, lv_color_hex(p.card), 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bb, 1, 0);
        lv_obj_set_style_border_color(bb, lv_color_hex(p.line), 0);
        lv_obj_add_event_cb(bb, back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(bb);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(p.fg), 0);
        lv_label_set_text(bl, LV_SYMBOL_LEFT);
        lv_obj_center(bl);
    }

    /* 头部 */
    mk_label(scr, 32, 36, &ui_font_32, C_BILI, "BILI / HOT SEARCH");
    s_upd   = mk_label(scr, 610, 44, &ui_font_14, p.dim, "UPD --");
    s_state = mk_label(scr, 720, 44, &ui_font_14, C_BILI, "OK");

    {
        lv_obj_t *rb = lv_button_create(scr);
        lv_obj_set_size(rb, 44, 30);
        lv_obj_set_pos(rb, 748, 40);
        lv_obj_set_style_radius(rb, 4, 0);
        lv_obj_set_style_bg_color(rb, lv_color_hex(p.card), 0);
        lv_obj_set_style_bg_opa(rb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rb, 1, 0);
        lv_obj_set_style_border_color(rb, lv_color_hex(p.line), 0);
        lv_obj_add_event_cb(rb, refresh_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *rl = lv_label_create(rb);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(p.fg), 0);
        lv_label_set_text(rl, LV_SYMBOL_REFRESH);
        lv_obj_center(rl);
    }

    mk_label(scr, 32, 84, &ui_font_14, p.dim, "TRENDING NOW / TOP 10");
    mk_hline(scr, 32, 108, 736, p.fg);

    /* 10 行（预建） */
    for (int i = 0; i < BILI_ROWS; i++)
    {
        int y = 116 + i * BILI_ROW_H;
        s_row[i].rk   = mk_label(scr, 32, y + 8, &ui_font_14, C_BILI, "--");
        s_row[i].word = mk_label(scr, 76, y + 2, &ui_font_cn18, p.fg, "");   /* ★ 必须用中文字库：ui_font_20 没有汉字 */
        s_row[i].heat = mk_label(scr, 660, y + 8, &ui_font_14, p.dim, "");
        if (i < BILI_ROWS - 1)
        {
            mk_hline(scr, 32, y + BILI_ROW_H - 2, 736, p.line);
        }
    }

    mk_label(scr, 32, 458, &ui_font_14, p.dim, "BILIBILI.COM / HEAT SCORE");
    s_last_count = -1;
    s_last_state = -1;
    s_last_gen   = 0;
}

static void fill_bili(void)
{
    if (s_upd == NULL || s_state == NULL || s_row[0].rk == NULL)
    {
        return;                  /* 场景未构建完成/正在重建 */
    }

    bili_data_t d;
    bili_get(&d);

    char b[128];

    if (d.updated[0])
    {
        snprintf(b, sizeof(b), "UPD %s", d.updated);
        lv_label_set_text(s_upd, b);
    }
    lv_label_set_text(s_state, (d.state == BILI_OK) ? "OK" : "..");

    for (int i = 0; i < BILI_ROWS; i++)
    {
        if (i < d.count)
        {
            snprintf(b, sizeof(b), "#%02d", d.item[i].rank);
            lv_label_set_text(s_row[i].rk, b);

            sanitize(d.item[i].word, b, sizeof(b), 26);   /* 26 字 @20 号 ≈ 520px，右侧热度不撞 */
            lv_label_set_text(s_row[i].word, b);

            fmt_heat(d.item[i].heat, b, sizeof(b));
            lv_label_set_text(s_row[i].heat, b);
        }
        else
        {
            lv_label_set_text(s_row[i].rk, "");
            lv_label_set_text(s_row[i].word, "");
            lv_label_set_text(s_row[i].heat, "");
        }
    }

    s_last_count = d.count;
    s_last_state = (int)d.state;
    s_last_gen   = d.gen;
}

void scene_bili(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_bili();
        fill_bili();
    }
    else if ((frame % 25) == 0)
    {
        bili_data_t d;
        bili_get(&d);
        if (d.count != s_last_count || (int)d.state != s_last_state || d.gen != s_last_gen)
        {
            fill_bili();
        }
    }

    lvgl_port_task();
}
