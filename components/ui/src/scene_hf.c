/**
 * @file    scene_hf.c
 * @brief   HuggingFace Trending 监视器（复古终端风）
 *
 * 复刻自 B 站「Qwen4-Max 全方位实测」里的 S3 模型热榜机（2026-11-21）：
 *   浅底深字、序号框（浅填充+大数字+状态符）、行分隔线、♥ 点赞 / ↓ 下载、底部进度方块。
 *
 * 布局（800x480，一屏 3 条，10 条共 4 页 → 循环翻页）：
 *   y06   左上返回钮                          右上 状态栏（layer_top）
 *   y36   HF / TRENDING（32 号）              右上 UPD 时间 + OK + 刷新
 *   y84   MODEL INTELLIGENCE / TOP 10
 *   y108  分隔线
 *   116/224/332  三行条目（108 高）
 *   y448  进度方块 + 页码 + [HF-01] 页脚 + 翻页钮
 *
 * ★★ 架构铁律（2026-11-21 崩溃教训）：
 *   翻页 **只改文字/显隐，绝不创建或删除对象**。
 *   旧实现用 lv_obj_clean + 重建行，在 LVGL 事件回调里删对象树，
 *   indev 仍引用被删对象 → "Guru Meditation / Load address misaligned" 必崩。
 *   现在 3 个行槽在 build 时一次建好，翻页 = lv_label_set_text + flag。
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
#include "hf_trend.h"

#define HF_HEART_RED 0xE0443E   /* 心形红色（参考原视频） */
#define HF_ROWS      3          /* 一屏 3 条 */
#define HF_PAGES     4          /* 10 条 / 3 = 4 页 */
#define HF_ROW_H     108

/* 终端配色 */
typedef struct
{
    uint32_t bg, fg, dim, line, box_bg;
    uint32_t box_fill;   /* 序号框填充（比背景亮一档的浅色） */
} hf_pal_t;

static hf_pal_t pal(void)
{
    if (ui_theme_is_dark())
    {
        hf_pal_t p = { 0x0D1117, 0xB9D4C0, 0x6E8577, 0x24342A, 0x131A16, 0x1B2733 };
        return p;
    }
    hf_pal_t p = { 0xEFF2EC, 0x16202E, 0x5A6577, 0x9AA6B8, 0xFFFFFF, 0xD9E2EC };
    return p;
}

/* 行槽：所有对象预建，翻页只改文字 */
typedef struct
{
    lv_obj_t *row;      /* 整行容器（隐藏用） */
    lv_obj_t *num;      /* 序号 01..10 */
    lv_obj_t *st;       /* 框内状态 + / = */
    lv_obj_t *name;
    lv_obj_t *task;
    lv_obj_t *clock;    /* 钟表容器（圆+针） */
    lv_obj_t *time_hm;
    lv_obj_t *likes;
    lv_obj_t *dls;
} hf_slot_t;

static hf_slot_t  s_slot[HF_ROWS];
static lv_obj_t  *s_page_lbl;
static lv_obj_t  *s_time_lbl;
static lv_obj_t  *s_state_lbl;
static lv_obj_t  *s_blocks[10];
static int        s_page;           /* 0..HF_PAGES-1 */
static int        s_last_page = -1;
static int        s_last_count = -1;
static int        s_last_state = -1;
static unsigned   s_last_gen;
static hf_data_t  s_data;   /* 静态快照：避免在 LVGL 任务栈上开 1.1KB 局部 */

/* ---------------- 小工具 ---------------- */

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

/* 迷你钟表：15px 圆 + 两根指针（返回容器，便于随行隐藏） */
static lv_obj_t *mk_clock(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 15, 15);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 7, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_obj_create(c);
    lv_obj_set_size(v, 2, 5);
    lv_obj_set_pos(v, 6, 2);
    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *h = lv_obj_create(c);
    lv_obj_set_size(h, 5, 2);
    lv_obj_set_pos(h, 7, 6);
    lv_obj_set_style_radius(h, 0, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return c;
}

/* 数字缩写：1234 → 1.2k ; 1234567 → 1.2M */
static void fmt_count(int v, char *out, size_t osz)
{
    if (v >= 1000000)
    {
        snprintf(out, osz, "%.1fM", v / 1000000.0);
    }
    else if (v >= 1000)
    {
        snprintf(out, osz, "%.1fk", v / 1000.0);
    }
    else
    {
        snprintf(out, osz, "%d", v);
    }
}

/* 填一个行槽（纯文字/显隐，不创建对象） */
static void fill_slot(int r, int idx, const hf_item_t *it, const hf_pal_t *p)
{
    (void)p;
    hf_slot_t *s = &s_slot[r];
    char b[64];
    char nb[16];

    if (s->row == NULL)          /* 场景未构建完成/正在重建 —— 绝不碰野指针 */
    {
        return;
    }

    if (it == NULL)
    {
        lv_obj_add_flag(s->row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s->row, LV_OBJ_FLAG_HIDDEN);

    snprintf(b, sizeof(b), "%02d", idx + 1);
    lv_label_set_text(s->num, b);
    lv_label_set_text(s->st, it->is_new ? "+" : "=");

    /* 超长名字截断并加 ~ 提示（原视频同款）；34 字符 @24 号 ≈ 442px，恰不撞右侧 */
    if (strlen(it->id) > 33)
    {
        snprintf(b, sizeof(b), "%.33s~", it->id);
    }
    else
    {
        snprintf(b, sizeof(b), "%s", it->id);
    }
    lv_label_set_text(s->name, b);

    /* 类型显示限 28 字符（存储 40，足够容纳 HF 最长 tag） */
    snprintf(b, sizeof(b), "%.28s", it->task);
    lv_label_set_text(s->task, b);
    lv_label_set_text(s->time_hm, it->created);

    fmt_count(it->likes, nb, sizeof(nb));
    lv_label_set_text(s->likes, nb);
    fmt_count(it->downloads, nb, sizeof(nb));
    lv_label_set_text(s->dls, nb);
}

/* ---------------- 翻页（只改状态 + 文字，零对象操作） ---------------- */

static void fill_list(void);

static void hf_page_prev(void)
{
    s_page = (s_page <= 0) ? (HF_PAGES - 1) : (s_page - 1);
    fill_list();
    ESP_LOGI("hf_ui", "page -> %d", s_page);
}

static void hf_page_next(void)
{
    s_page = (s_page >= HF_PAGES - 1) ? 0 : (s_page + 1);
    fill_list();
    ESP_LOGI("hf_ui", "page -> %d", s_page);
}

static void page_prev_cb(lv_event_t *e) { (void)e; hf_page_prev(); }
static void page_next_cb(lv_event_t *e) { (void)e; hf_page_next(); }

/* ★ 不用 LVGL 手势引擎（全局阈值会影响其它页）：自己记按下/释放点算位移。
 *   水平位移 > 40px 且明显以水平为主 → 翻页。慢速滑动同样生效。 */
static lv_point_t s_press_pt;
static bool       s_pressed;

static void press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL)
    {
        lv_indev_get_point(indev, &s_press_pt);
        s_pressed = true;
    }
}

static void release_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pressed)
    {
        return;
    }
    s_pressed = false;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
    {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = p.x - s_press_pt.x;
    int dy = p.y - s_press_pt.y;
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;

    if (adx > 40 && adx > (ady * 3) / 2)
    {
        if (dx < 0)
        {
            hf_page_next();      /* 往左划 → 下一页 */
        }
        else
        {
            hf_page_prev();
        }
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
    hf_refresh_now();
}

/* ---------------- 建页 ---------------- */

static void build_hf(void)
{
    hf_pal_t p = pal();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* ★★★ 11-22 崩溃修复（coredump 实锤）：
     *   lv_obj_clean() 删掉了所有子对象，但下面的静态指针仍指向已释放的内存。
     *   崩溃栈：release_cb → hf_page_prev → fill_list → fill_slot(idx=0)
     *           → lv_label_set_text(s->st) → lv_obj_invalidate → lv_obj_get_parent（崩）
     *   触发条件：滑动翻页的瞬间切场景（残留的 RELEASED 事件携带旧对象指针）。
     *   修法：clean 之后立刻清空全部指针，配合 fill_list/fill_slot 的空指针保护。 */
    memset(s_slot, 0, sizeof(s_slot));
    s_page_lbl  = NULL;
    s_time_lbl  = NULL;
    s_state_lbl = NULL;
    memset(s_blocks, 0, sizeof(s_blocks));

    lv_obj_set_style_bg_color(scr, lv_color_hex(p.bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上返回（与状态栏同一行的左侧空白） */
    {
        lv_obj_t *bb = lv_button_create(scr);
        lv_obj_set_size(bb, 46, 26);
        lv_obj_set_pos(bb, 32, 6);
        lv_obj_set_style_radius(bb, 4, 0);
        lv_obj_set_style_bg_color(bb, lv_color_hex(p.box_bg), 0);
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

    /* 滑动翻页（自算位移；scr 需可点击才能接收空白处的按下事件）
     * ★★★ 11-22 崩溃真因：scr 是常驻对象，lv_obj_clean(scr) **只删子对象、
     *   不删 scr 自身的事件回调**。原来每次进 HF 页都 add 一次 ⇒ 回调累积：
     *   进 N 次就有 N 对，一次滑动触发 N 次 hf_page_next ⇒ 连续 fill_list，
     *   中间对象已被重建 ⇒ 拿到已释放的 label（free() 越界 panic）。
     *   ⇒ 必须先解绑再注册，保证永远只有一对。 */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb(scr, press_cb);
    lv_obj_remove_event_cb(scr, release_cb);
    lv_obj_add_event_cb(scr, press_cb,   LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(scr, release_cb, LV_EVENT_RELEASED, NULL);

    /* 头部 */
    mk_label(scr, 32, 36, &ui_font_32, p.fg, "HF / TRENDING");
    s_time_lbl  = mk_label(scr, 560, 44, &ui_font_14, p.dim, "UPD --:--");
    s_state_lbl = mk_label(scr, 690, 44, &ui_font_14, p.fg, "..");

    {
        lv_obj_t *rb = lv_button_create(scr);
        lv_obj_set_size(rb, 44, 30);
        lv_obj_set_pos(rb, 744, 40);
        lv_obj_set_style_radius(rb, 4, 0);
        lv_obj_set_style_bg_color(rb, lv_color_hex(p.box_bg), 0);
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

    mk_label(scr, 32, 84, &ui_font_14, p.dim, "MODEL INTELLIGENCE / RECENT TREND");
    mk_hline(scr, 32, 108, 736, p.fg);

    /* ---- 三行槽：一次建好，翻页只改文字 ---- */
    for (int r = 0; r < HF_ROWS; r++)
    {
        hf_slot_t *s = &s_slot[r];

        s->row = lv_obj_create(scr);
        lv_obj_set_size(s->row, 800, HF_ROW_H);
        lv_obj_set_pos(s->row, 0, 116 + r * HF_ROW_H);
        lv_obj_set_style_bg_opa(s->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s->row, 0, 0);
        lv_obj_set_style_pad_all(s->row, 0, 0);
        /* ★ 必须清 CLICKABLE：否则列表区把手势吞掉（2026-11-21 教训） */
        lv_obj_clear_flag(s->row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 序号框 */
        lv_obj_t *box = lv_obj_create(s->row);
        lv_obj_set_size(box, 74, 74);
        lv_obj_set_pos(box, 32, 12);
        lv_obj_set_style_radius(box, 2, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(p.box_fill), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        s->num = mk_label(box, 0, 6, &ui_font_32, p.fg, "--");
        lv_obj_align(s->num, LV_ALIGN_TOP_MID, 0, 6);
        s->st = mk_label(box, 0, 0, &ui_font_20, p.dim, "=");
        lv_obj_align(s->st, LV_ALIGN_BOTTOM_MID, 0, -6);

        s->name    = mk_label(s->row, 128, 4,  &ui_font_24, p.fg,  "");
        s->task    = mk_label(s->row, 128, 38, &ui_font_20, p.dim, "");
        s->clock   = mk_clock(s->row, 300, 65, p.dim);
        s->time_hm = mk_label(s->row, 324, 62, &ui_font_20, p.dim, "");

        mk_label(s->row, 572, 50, &ui_font_28, HF_HEART_RED, "\u2665");
        s->likes = mk_label(s->row, 604, 50, &ui_font_28, p.fg, "");

        mk_label(s->row, 692, 50, &ui_font_28, p.dim, "\u2193");
        s->dls = mk_label(s->row, 716, 50, &ui_font_28, p.fg, "");

        mk_hline(s->row, 32, HF_ROW_H - 4, 736, p.line);
    }

    /* ---- 底部：进度方块 / 页码 / 页脚 / 翻页钮 ---- */
    for (int i = 0; i < 10; i++)
    {
        lv_obj_t *blk = lv_obj_create(scr);
        lv_obj_set_size(blk, 12, 12);
        lv_obj_set_pos(blk, 32 + i * 17, 452);
        lv_obj_set_style_radius(blk, 1, 0);
        lv_obj_set_style_border_width(blk, 1, 0);
        lv_obj_set_style_border_color(blk, lv_color_hex(p.fg), 0);
        lv_obj_set_style_bg_color(blk, lv_color_hex(p.fg), 0);
        lv_obj_set_style_bg_opa(blk, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(blk, 0, 0);
        lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_blocks[i] = blk;
    }

    s_page_lbl = mk_label(scr, 224, 450, &ui_font_14, p.fg, "1/4");
    mk_label(scr, 320, 450, &ui_font_14, p.dim, "[HF-01] RECENT TREND / LIVE");

    {
        lv_obj_t *b1 = lv_button_create(scr);
        lv_obj_set_size(b1, 44, 30);
        lv_obj_set_pos(b1, 690, 444);
        lv_obj_set_style_radius(b1, 4, 0);
        lv_obj_set_style_bg_color(b1, lv_color_hex(p.box_bg), 0);
        lv_obj_set_style_bg_opa(b1, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b1, 1, 0);
        lv_obj_set_style_border_color(b1, lv_color_hex(p.line), 0);
        lv_obj_add_event_cb(b1, page_prev_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l1 = lv_label_create(b1);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l1, lv_color_hex(p.fg), 0);
        lv_label_set_text(l1, LV_SYMBOL_LEFT);
        lv_obj_center(l1);

        lv_obj_t *b2 = lv_button_create(scr);
        lv_obj_set_size(b2, 44, 30);
        lv_obj_set_pos(b2, 740, 444);
        lv_obj_set_style_radius(b2, 4, 0);
        lv_obj_set_style_bg_color(b2, lv_color_hex(p.box_bg), 0);
        lv_obj_set_style_bg_opa(b2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b2, 1, 0);
        lv_obj_set_style_border_color(b2, lv_color_hex(p.line), 0);
        lv_obj_add_event_cb(b2, page_next_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l2 = lv_label_create(b2);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l2, lv_color_hex(p.fg), 0);
        lv_label_set_text(l2, LV_SYMBOL_RIGHT);
        lv_obj_center(l2);
    }

    /* 首次进入：判据复位，强制填一次 */
    s_last_page  = -1;
    s_last_count = -1;
    s_last_state = -1;
}

/* ---------------- 填列表（三行槽 + 进度 + 页码） ---------------- */

static void fill_list(void)
{
    /* ★ 场景未构建完成（或正在重建）时直接返回。
     *   实测崩溃栈的入口就是这里被残留事件调用。 */
    if (s_slot[0].row == NULL || s_page_lbl == NULL ||
        s_time_lbl == NULL || s_state_lbl == NULL)
    {
        return;
    }

    hf_pal_t p = pal();
    hf_data_t *d = &s_data;
    hf_get(d);

    char b[40];

    /* 行槽 */
    for (int r = 0; r < HF_ROWS; r++)
    {
        int idx = s_page * HF_ROWS + r;
        if (idx < d->count)
        {
            fill_slot(r, idx, &d->item[idx], &p);
        }
        else
        {
            fill_slot(r, idx, NULL, &p);
        }
    }

    /* 页码 + 进度方块 */
    int pages = (d->count > 0) ? ((d->count + HF_ROWS - 1) / HF_ROWS) : HF_PAGES;
    snprintf(b, sizeof(b), "%d/%d", s_page + 1, pages);
    lv_label_set_text(s_page_lbl, b);

    for (int i = 0; i < 10; i++)
    {
        if (s_blocks[i] == NULL)
        {
            continue;
        }
        bool filled = (d->count > 0) && (i >= s_page * HF_ROWS) &&
                      (i < (s_page + 1) * HF_ROWS) && (i < d->count);
        lv_obj_set_style_bg_opa(s_blocks[i], filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }

    /* 头部时间/状态 */
    if (d->updated[0])
    {
        snprintf(b, sizeof(b), "UPD %s", d->updated);
        lv_label_set_text(s_time_lbl, b);
    }
    if (d->state == HF_STATE_OK)
    {
        lv_label_set_text(s_state_lbl, "OK");
    }
    else if (d->state == HF_STATE_FAIL)
    {
        snprintf(b, sizeof(b), "ERR %d", d->http_code);
        lv_label_set_text(s_state_lbl, b);
    }
    else
    {
        lv_label_set_text(s_state_lbl, "..");
    }

    s_last_page  = s_page;
    s_last_count = d->count;
    s_last_state = (int)d->state;
    s_last_gen   = d->gen;
}

/* ---------------- 场景 ---------------- */

void scene_hf(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_hf();
        fill_list();
    }
    else if ((frame % 25) == 0)
    {
        /* 数据/状态变化时刷新文字（不创建对象） */
        hf_get(&s_data);
        if (s_data.count != s_last_count || (int)s_data.state != s_last_state ||
            s_data.gen != s_last_gen)     /* ★ 排名顺序变化也重绘 */
        {
            fill_list();
        }
    }

    lvgl_port_task();
}
