/**
 * @file    scene_ai_rank.c
 * @brief   国内 AI 产品榜（AICPB）— 可滚动详情列表
 *
 * 数据：aicpb.com 的 JSON-LD（排位 + 中文产品名；访问量数值只有 Top1 有公开 API）
 * 口径（页面原文）：Website 榜按 Website Visits，App 榜按 App MAU，数据 Jun 2026（季度更新）
 *
 * 布局（800x480，垂直滚动，20 条 + 2 个分节头）：
 *   y6    ← 返回（左上）
 *   y36   AI PRODUCTS / CN     DATA Jun 2026 / QUARTERLY   OK   [刷新]
 *   y106  分隔线
 *   y112..456  滚动区：每条 84 高
 *        #01  DeepSeek            ← 英文名 20 号
 *             纳米AI搜索（中文名） ← cn16 中文小字
 *             WEB / VISITS        ← 口径标签 14 号
 *   底部固定：AICPB.COM / SWIPE TO SCROLL + UPD 时刻
 *
 * ★ 架构铁律（同 scene_hf）：所有行在 build 时一次建好，刷新只改文字，绝不删对象。
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
#include "ai_rank.h"

#define AR_N         10          /* 每榜 10 条 */
#define AR_ROW_H     104
#define AR_HEAD_H    44

typedef struct
{
    uint32_t bg, fg, dim, line, box_fill, accent;
} ar_pal_t;

static ar_pal_t pal(void)
{
    if (ui_theme_is_dark())
    {
        ar_pal_t p = { 0x0E1412, 0xB9D4C0, 0x6E8577, 0x24342A, 0x151D18, 0x4FD08A };
        return p;
    }
    ar_pal_t p = { 0xEDF2EE, 0x14211C, 0x5A6B63, 0x9AB0A4, 0xFFFFFF, 0x1F9D6B };
    return p;
}

typedef struct
{
    lv_obj_t *rk;
    lv_obj_t *en;
    lv_obj_t *cn;
    lv_obj_t *tag;
} ar_row_t;

static ar_row_t s_web[AR_N];
static ar_row_t s_app[AR_N];
static lv_obj_t *s_scroll;
static lv_obj_t *s_month;
static lv_obj_t *s_state;
static lv_obj_t *s_upl;
static int       s_last_w = -1;
static int       s_last_a = -1;
static int       s_last_state = -1;

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

/* 按 UTF-8 边界截断（汉字不能被切半个，否则显示乱码） */
static void utf8_trunc(const char *src, char *dst, size_t dsz)
{
    size_t n = strlen(src);
    if (n < dsz)
    {
        memcpy(dst, src, n + 1);
        return;
    }
    size_t cut = dsz - 1;
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
    {
        cut--;
    }
    memcpy(dst, src, cut);
    dst[cut] = 0;
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    ai_rank_refresh_now();
}

static void build_row(lv_obj_t *parent, ar_row_t *r, int y, const ar_pal_t *p)
{
    /* 序号框（浅填充方框 + 28 号数字，参照 HF 页的标号风格） */
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 60, 60);
    lv_obj_set_pos(box, 24, y + 18);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(p->box_fill), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    r->rk = mk_label(box, 0, 0, &ui_font_28, p->accent, "--");
    lv_obj_align(r->rk, LV_ALIGN_CENTER, 0, 0);

    r->en = mk_label(parent, 104, y + 10, &ui_font_24, p->fg, "");        /* 英文名 24 */
    r->cn = mk_label(parent, 104, y + 44, &ui_font_cn18, p->dim, "");    /* 中文名 18 */
    r->tag = mk_label(parent, 104, y + 76, &ui_font_20, p->dim, "");     /* 口径 20（靠颜色弱化） */
}

static void build_head(lv_obj_t *parent, int y, const char *title, const ar_pal_t *p)
{
    lv_obj_t *h = lv_obj_create(parent);
    lv_obj_set_size(h, 780, AR_HEAD_H);
    lv_obj_set_pos(h, 10, y);
    lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    mk_label(h, 10, 10, &ui_font_24, p->accent, title);
    mk_hline(h, 10, 40, 740, p->line);
}

static void build_ar(void)
{
    ar_pal_t p = pal();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上返回（与状态栏同一行） */
    {
        lv_obj_t *bb = lv_button_create(scr);
        lv_obj_set_size(bb, 46, 26);
        lv_obj_set_pos(bb, 32, 6);
        lv_obj_set_style_radius(bb, 4, 0);
        lv_obj_set_style_bg_color(bb, lv_color_hex(p.box_fill), 0);
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
    mk_label(scr, 32, 36, &ui_font_32, p.fg, "AI PRODUCTS / CN");
    s_month = mk_label(scr, 452, 44, &ui_font_14, p.dim, "DATA ---- / QUARTERLY");
    s_state = mk_label(scr, 694, 44, &ui_font_14, p.accent, "OK");

    {
        lv_obj_t *rb = lv_button_create(scr);
        lv_obj_set_size(rb, 44, 30);
        lv_obj_set_pos(rb, 736, 40);
        lv_obj_set_style_radius(rb, 4, 0);
        lv_obj_set_style_bg_color(rb, lv_color_hex(p.box_fill), 0);
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

    mk_hline(scr, 32, 106, 736, p.fg);

    /* 滚动容器（垂直，原生惯性滚动 + 右侧滚动条） */
    s_scroll = lv_obj_create(scr);
    lv_obj_set_size(s_scroll, 800, 336);
    lv_obj_set_pos(s_scroll, 0, 112);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_set_style_pad_right(s_scroll, 12, 0);   /* 右侧留出滚动条的位置（画在 padding 区） */
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(s_scroll, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(p.dim), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_scroll, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_scroll, 2, LV_PART_SCROLLBAR);

    build_head(s_scroll, 0, "WEBSITE / BY VISITS", &p);
    for (int i = 0; i < AR_N; i++)
    {
        int y = AR_HEAD_H + i * AR_ROW_H;
        build_row(s_scroll, &s_web[i], y, &p);
        mk_hline(s_scroll, 20, y + AR_ROW_H - 1, 740, p.line);
    }

    int y2 = AR_HEAD_H + AR_N * AR_ROW_H;
    build_head(s_scroll, y2, "APP / BY MAU", &p);
    for (int i = 0; i < AR_N; i++)
    {
        int y = y2 + AR_HEAD_H + i * AR_ROW_H;
        build_row(s_scroll, &s_app[i], y, &p);
        mk_hline(s_scroll, 20, y + AR_ROW_H - 1, 740, p.line);
    }

    /* 页脚（固定，不随滚动） */
    mk_label(scr, 32, 456, &ui_font_14, p.dim, "AICPB.COM / SWIPE TO SCROLL");
    s_upl = mk_label(scr, 620, 456, &ui_font_14, p.dim, "UPD --");

    s_last_w = -1;
    s_last_a = -1;
    s_last_state = -1;
}

static void fill_row(ar_row_t *r, const ai_rank_item_t *it, const char *tag)
{
    char b[96];

    snprintf(b, sizeof(b), "#%02d", it->rank);
    lv_label_set_text(r->rk, b);
    lv_label_set_text(r->en, it->name);

    /* 中文名与原英文名完全相同（如 DeepSeek）就不重复显示 */
    if (strcmp(it->cn, it->name) == 0 || it->cn[0] == 0)
    {
        lv_label_set_text(r->cn, "-");   /* 占位：保持三行结构一致，不留空洞 */
    }
    else
    {
        char cut[64];
        utf8_trunc(it->cn, cut, 58);
        lv_label_set_text(r->cn, cut);
    }

    lv_label_set_text(r->tag, tag);
}

static void fill_ar(void)
{
    ai_rank_data_t d;
    ai_rank_get(&d);

    char b[64];
    if (d.month[0])
    {
        snprintf(b, sizeof(b), "DATA %s / QUARTERLY", d.month);
        lv_label_set_text(s_month, b);
    }
    lv_label_set_text(s_state, (d.state == AI_RANK_OK) ? "OK" : "..");
    if (d.updated[0])
    {
        snprintf(b, sizeof(b), "UPD %s", d.updated);
        lv_label_set_text(s_upl, b);
    }

    for (int i = 0; i < AR_N; i++)
    {
        if (i < d.web_count)
        {
            fill_row(&s_web[i], &d.web[i], "WEB / VISITS");
        }
        if (i < d.app_count)
        {
            fill_row(&s_app[i], &d.app[i], "APP / MAU");
        }
    }

    s_last_w = d.web_count;
    s_last_a = d.app_count;
    s_last_state = (int)d.state;
}

void scene_ai_rank(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_ar();
        fill_ar();
    }
    else if ((frame % 25) == 0)
    {
        ai_rank_data_t d;
        ai_rank_get(&d);
        if ((int)d.state != s_last_state || d.web_count != s_last_w ||
            d.app_count != s_last_a)
        {
            fill_ar();
        }
    }

    lvgl_port_task();
}
