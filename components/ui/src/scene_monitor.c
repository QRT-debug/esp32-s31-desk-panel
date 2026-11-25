/**
 * @file    scene_monitor.c
 * @brief   局域网 PC 状态监控 —— 横条仪表盘（浅色）
 *
 * 设计参考 mcu-pc-dashboard / MonitorX（都是 800x480 的 PC 副屏）：
 *   横向进度条比圆环更省渲染（矩形填充 vs 逐角度画弧），
 *   同样宽度能显示 5~6 项指标，信息密度高得多。
 *
 * 布局（800x480）：
 *   顶栏   返回 + 标题 + 状态栏（全局层）
 *   主卡   5 行：CPU / MEM / GPU / C: / D:   （图标圆 + 标签 + 进度条 + 大数字 + 副信息）
 *   底卡   网络上下行 + PC 运行时长 + 在线状态
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_port.h"
#include "pc_monitor.h"
#include "ui_fonts.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "wifi_sta.h"

/* 11-20 主题化：指向可切换色板 */
#define C_BG       (ui_theme()->bg_top)
#define C_CARD     (ui_theme()->card)
#define C_TITLE    (ui_theme()->title)
#define C_SUB      (ui_theme()->sub)
#define C_TRACK    (ui_theme()->track)
#define C_SHADOW   (ui_theme()->shadow)

#define C_BLUE     0x4C9AFF
#define C_PURPLE   (ui_theme()->accent)
#define C_ORANGE   0xF2A03D
#define C_GREEN    0x35D07F
#define C_RED      0xE8556D
#define C_YELLOW   0xF2C14E

#define ROW_H      56
#define ROW_COUNT  5

typedef struct
{
    lv_obj_t *arc;        /* 270° 环形仪表 */
    lv_obj_t *val;        /* 环内大数字 */
    lv_obj_t *sub;        /* 卡片底部：型号 / 容量 */
    uint32_t  color;
} gauge_t;

static lv_obj_t *s_root;
static gauge_t   s_g[3];                 /* 0=CPU 1=MEM 2=GPU */
static lv_obj_t *s_lbl_host;
static lv_obj_t *s_lbl_online;
static lv_obj_t *s_net_up, *s_net_dn;
static lv_obj_t *s_bar_up, *s_bar_dn;
static lv_obj_t *s_disk_name[2], *s_disk_pct[2], *s_disk_free[2], *s_disk_bar[2];

/**
 * ★ 文本没变就不 set（2026-11-25）。
 *
 * LVGL 的 `lv_label_set_text` **不比较内容** —— 同一串也照样标脏重画。
 * 本页每 300ms 刷 **34 个 label**，而 CPU/内存这些数值每次只变个位数，
 * 结果整页被拉出 10+ 块脏区、render 20ms+（实测）。
 * 加这道比对后，只有真的变了的那 1~2 个字段会重绘。
 *
 * ★ 不用"整页快照 memcmp"：pc_stats_t 里 CPU/内存每秒都在变，
 *   快照永远不同 ⇒ 比对永不命中，等于没做。
 * ★ 必须定义在所有调用点之前（本文件前面的 build 分支里也有调用）。
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

static void fmt_uptime(int sec, char *out, size_t cap)
{
    if (sec < 0)
    {
        sec = 0;
    }
    int d = sec / 86400;
    int h = (sec % 86400) / 3600;
    int m = (sec % 3600) / 60;
    if (d > 0)
    {
        snprintf(out, cap, "UP %dd %02dh", d, h);
    }
    else
    {
        snprintf(out, cap, "UP %02dh %02dm", h, m);
    }
}

/* 阈值配色：<70 正常，70~85 黄，>85 红 */
static uint32_t level_color(float pct, uint32_t normal)
{
    if (pct >= 85.0f)
    {
        return C_RED;
    }
    if (pct >= 70.0f)
    {
        return C_YELLOW;
    }
    return normal;
}

/* ---------------- 监控页 v2：对齐 IQON 参考的排版精度（2026-11-26） ----------------
 * 和 v1 的差别（都是"廉价感"的来源）：
 *   1. 数值和单位用**同一个字体、同一个 label** —— v1 把 '%' 单独用 20px 塞在右下，
 *      基线不齐，看起来就是"歪"
 *   2. 进度条改 3px 细线（参考图是细白线），不是 8px 彩色粗条
 *   3. 卡片**去掉边框**，只靠底色比页面亮一档分层（边框会显得像贴纸）
 *   4. 环形统一浅蓝，不再一卡一色 —— 多色反而廉价
 *   5. 状态用**8px 圆点对象** + 小字，不用字符（'●' 字库没有）
 */

#define COL_PAGE   0x0A0C10
#define COL_CARD_TOP 0x191D26   /* 卡片渐变：顶部 */
#define COL_CARD_BOT 0x101319   /*          底部 */
#define COL_TRACK  0x1E232D
#define COL_TXT    0xECEEF2
#define COL_SUB    0x8A93A3
#define COL_RING   0x7DD3FC
#define COL_OK     0x34D399
#define COL_WARN   0xFBBF24
#define COL_BAD    0xF87171

typedef struct
{
    lv_obj_t *arc;
    lv_obj_t *val;      /* 环心大数字（含单位） */
    lv_obj_t *pct;      /* Usage 行右侧的百分比 */
    lv_obj_t *bar;
    lv_obj_t *sub;      /* 型号 / 容量 */
    lv_obj_t *dot;      /* 状态圆点 */
    lv_obj_t *stxt;     /* 状态文字 */
} card_t;

static card_t s_c[4];   /* 0=CPU 1=GPU 2=MEM 3=DISK */
static lv_obj_t *s_net_up, *s_net_dn, *s_bar_up, *s_bar_dn;
static lv_obj_t *s_lbl_host, *s_net_dot, *s_net_stxt;

static lv_obj_t *mk_card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD_TOP), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(COL_CARD_BOT), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);   /* 顶部略亮：玻璃感 */
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);          /* ★ 无边框 */
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *mk_lbl(lv_obj_t *p, int x, int y, const lv_font_t *f,
                        uint32_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

static void mk_dot(lv_obj_t *p, int x, int y, uint32_t col, lv_obj_t **out)
{
    lv_obj_t *d = lv_obj_create(p);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (out != NULL)
    {
        *out = d;
    }
}

/** 3px 细进度条（参考图那种细白线） */
static lv_obj_t *mk_bar(lv_obj_t *p, int x, int y, int w, uint32_t col)
{
    lv_obj_t *b = lv_bar_create(p);
    lv_obj_set_size(b, w, 3);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    return b;
}

/**
 * 统计卡：左文右环。
 * ★ 环内数字与 '%' **同一个 label**（ui_font_28 含 %），所以不会出现基线和位置偏差。
 *   卡片 380×140 的排布：
 *     y=10  标题 20px          y=36 型号 14px 灰
 *     y=84  Usage 行（标签左、数值右，同一基线）
 *     y=112 细进度条
 *     右侧 环 110px，环心数字
 */
static void mk_stat_card(lv_obj_t *p, int x, int y, const char *title, card_t *c)
{
    lv_obj_t *k = mk_card(p, x, y, 380, 140);

    mk_lbl(k, 16, 12, &ui_font_20, COL_TXT, title);
    mk_dot(k, 132, 20, COL_OK, &c->dot);
    c->stxt = mk_lbl(k, 146, 15, &ui_font_14, COL_OK, "Optimal");

    c->sub = mk_lbl(k, 16, 40, &ui_font_14, COL_SUB, "—");

    mk_lbl(k, 16, 86, &ui_font_14, COL_SUB, "Usage");
    c->bar = mk_bar(k, 16, 112, 260, COL_RING);

    /* 环 */
    lv_obj_t *arc = lv_arc_create(k);
    lv_obj_set_size(arc, 110, 110);
    lv_obj_set_pos(arc, 254, 15);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, -45, 225);
    lv_arc_set_rotation(arc, 180);       /* ★ 缺口默认落在正上方，转 180° 才是正下方（参考图那样） */
    lv_obj_set_style_arc_width(arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_RING), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    /* 环心数字：一个 label 装下 "56%"，居中在环内 */
    c->val = lv_label_create(k);
    lv_obj_set_style_text_font(c->val, &ui_font_28, 0);
    lv_obj_set_style_text_color(c->val, lv_color_hex(COL_TXT), 0);
    lv_obj_set_width(c->val, 110);
    lv_obj_set_style_text_align(c->val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->val, 254, 52);
    lv_label_set_text(c->val, "0%");

    c->arc = arc;
}

static void build_monitor(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_statusbar_show(false);

    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_PAGE), 0);
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

    /* 顶栏 */
    mk_lbl(root, 16, 8, &ui_font_28, COL_TXT, "My PC");
    s_lbl_host = mk_lbl(root, 108, 18, &ui_font_14, COL_SUB, "—");

    /* 返回 */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 32, 32);
    lv_obj_set_pos(back, 752, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD_TOP), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &ui_font_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_SUB), 0);
    lv_label_set_text(bl, "<");
    lv_obj_center(bl);

    /* 2×2 统计卡 */
    mk_stat_card(root, 16, 46, "Processor", &s_c[0]);
    mk_stat_card(root, 404, 46, "Video Card", &s_c[1]);
    mk_stat_card(root, 16, 194, "Memory", &s_c[2]);
    mk_stat_card(root, 404, 194, "Storage", &s_c[3]);
    s_c[3].pct  = s_c[3].pct;   /* Storage 的 sub 显示盘符 */

    /* 底部：网络 */
    lv_obj_t *cn = mk_card(root, 16, 342, 768, 122);
    mk_lbl(cn, 16, 12, &ui_font_20, COL_TXT, "Network");
    mk_dot(cn, 120, 20, COL_OK, &s_net_dot);
    s_net_stxt = mk_lbl(cn, 134, 15, &ui_font_14, COL_OK, "Connected");

    mk_lbl(cn, 16, 52, &ui_font_14, COL_SUB, "UP");
    s_net_up = mk_lbl(cn, 16, 70, &ui_font_24, COL_TXT, "0.0");
    mk_lbl(cn, 62, 76, &ui_font_14, COL_SUB, "MB/s");

    mk_lbl(cn, 220, 52, &ui_font_14, COL_SUB, "DOWN");
    s_net_dn = mk_lbl(cn, 220, 70, &ui_font_24, COL_TXT, "0.0");
    mk_lbl(cn, 266, 76, &ui_font_14, COL_SUB, "MB/s");

    s_bar_up = mk_bar(cn, 420, 60, 320, COL_RING);
    s_bar_dn = mk_bar(cn, 420, 84, 320, COL_RING);
}

static uint32_t util_color(int v)
{
    return (v >= 90) ? COL_BAD : (v >= 70) ? COL_WARN : COL_OK;
}

static void update_ui(void)
{
    pc_stats_t st;
    pc_monitor_get(&st);

    char b[48];
    int  vals[4];
    vals[0] = st.valid ? (int)(st.cpu_pct + 0.5f) : 0;
    vals[1] = (st.valid && st.gpu_ok) ? st.gpu_pct : 0;
    vals[2] = st.valid ? (int)(st.mem_pct + 0.5f) : 0;
    vals[3] = st.valid ? (int)(st.disks[0].percent + 0.5f) : 0;

    for (int i = 0; i < 4; i++)
    {
        int v = vals[i];
        if (v < 0)   { v = 0; }
        if (v > 100) { v = 100; }

        lv_arc_set_value(s_c[i].arc, v);

        snprintf(b, sizeof(b), "%d%%", v);
        set_text_if_changed(s_c[i].val, b);
        lv_bar_set_value(s_c[i].bar, v, LV_ANIM_OFF);

        uint32_t col = util_color(v);
        lv_obj_set_style_bg_color(s_c[i].dot, lv_color_hex(col), 0);
        lv_obj_set_style_text_color(s_c[i].stxt, lv_color_hex(col), 0);
        set_text_if_changed(s_c[i].stxt, v >= 90 ? "High" : (v >= 70 ? "Busy" : "Optimal"));
    }

    /* 副标题：型号 / 显存 / 容量 / 盘符 */
    set_text_if_changed(s_c[0].sub, st.cpu_name[0] ? st.cpu_name : "—");
    set_text_if_changed(s_c[1].sub, st.gpu_name[0] ? st.gpu_name : "no gpu");

    snprintf(b, sizeof(b), "%.0f / %.0f GB", (double)st.mem_used_gb, (double)st.mem_total_gb);
    set_text_if_changed(s_c[2].sub, b);

    if (st.disk_count > 0)
    {
        snprintf(b, sizeof(b), "%s  free %.0f GB", st.disks[0].name, (double)st.disks[0].free_gb);
    }
    else
    {
        snprintf(b, sizeof(b), "—");
    }
    set_text_if_changed(s_c[3].sub, b);

    /* 顶栏 */
    set_text_if_changed(s_lbl_host, st.host[0] ? st.host : "—");

    /* 网络 */
    float up = st.net_up_kbps / 1024.0f;
    float dn = st.net_down_kbps / 1024.0f;
    snprintf(b, sizeof(b), "%.1f", (double)up);
    set_text_if_changed(s_net_up, b);
    snprintf(b, sizeof(b), "%.1f", (double)dn);
    set_text_if_changed(s_net_dn, b);

    int bu = (int)(up / 50.0f * 100.0f);
    int bd = (int)(dn / 50.0f * 100.0f);
    lv_bar_set_value(s_bar_up, bu > 100 ? 100 : bu, LV_ANIM_OFF);
    lv_bar_set_value(s_bar_dn, bd > 100 ? 100 : bd, LV_ANIM_OFF);

    uint32_t ncol = st.valid ? COL_OK : COL_BAD;
    lv_obj_set_style_bg_color(s_net_dot, lv_color_hex(ncol), 0);
    lv_obj_set_style_text_color(s_net_stxt, lv_color_hex(ncol), 0);
    set_text_if_changed(s_net_stxt, st.valid ? "Connected" : "Offline");
}

void scene_monitor(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_monitor();
        update_ui();
    }
    else if ((frame % 15) == 0)
    {
        update_ui();
    }

    lvgl_port_task();
}
