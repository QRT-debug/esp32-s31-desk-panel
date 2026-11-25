#!/usr/bin/env python3
"""监控页第二版：对齐 IQON 参考的排版精度（细条/无边框/统一环色/基线对齐）。"""
P = 'components/ui/src/scene_monitor.c'
src = open(P, encoding='utf-8').read()

i = src.index('static lv_obj_t *mk_card(lv_obj_t *p, int x, int y, int w, int h)')
j = src.index('void scene_monitor(uint16_t *fb, int frame)')

body = r'''/* ---------------- 监控页 v2：对齐 IQON 参考的排版精度（2026-11-26） ----------------
 * 和 v1 的差别（都是"廉价感"的来源）：
 *   1. 数值和单位用**同一个字体、同一个 label** —— v1 把 '%' 单独用 20px 塞在右下，
 *      基线不齐，看起来就是"歪"
 *   2. 进度条改 3px 细线（参考图是细白线），不是 8px 彩色粗条
 *   3. 卡片**去掉边框**，只靠底色比页面亮一档分层（边框会显得像贴纸）
 *   4. 环形统一浅蓝，不再一卡一色 —— 多色反而廉价
 *   5. 状态用**8px 圆点对象** + 小字，不用字符（'●' 字库没有）
 */

#define COL_PAGE   0x0A0C10
#define COL_CARD   0x14171E
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
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
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
    c->stxt = mk_lbl(k, 146, 15, &ui_font_14, COL_OK, "正常");

    c->sub = mk_lbl(k, 16, 40, &ui_font_14, COL_SUB, "—");

    mk_lbl(k, 16, 86, &ui_font_14, COL_SUB, "Usage");
    c->pct = mk_lbl(k, 196, 78, &ui_font_20, COL_TXT, "—");
    lv_obj_set_width(c->pct, 80);
    lv_obj_set_style_text_align(c->pct, LV_TEXT_ALIGN_RIGHT, 0);

    c->bar = mk_bar(k, 16, 112, 260, COL_RING);

    /* 环 */
    lv_obj_t *arc = lv_arc_create(k);
    lv_obj_set_size(arc, 110, 110);
    lv_obj_set_pos(arc, 254, 15);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, -45, 225);
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
    lv_obj_set_pos(c->val, 254, 56);
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
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_CARD), 0);
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
    s_net_stxt = mk_lbl(cn, 134, 15, &ui_font_14, COL_OK, "已连接");

    mk_lbl(cn, 16, 52, &ui_font_14, COL_SUB, "UP");
    s_net_up = mk_lbl(cn, 16, 70, &ui_font_24, COL_TXT, "0.0");
    mk_lbl(cn, 100, 76, &ui_font_14, COL_SUB, "MB/s");

    mk_lbl(cn, 220, 52, &ui_font_14, COL_SUB, "DOWN");
    s_net_dn = mk_lbl(cn, 220, 70, &ui_font_24, COL_TXT, "0.0");
    mk_lbl(cn, 304, 76, &ui_font_14, COL_SUB, "MB/s");

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
        set_text_if_changed(s_c[i].pct, b);
        lv_bar_set_value(s_c[i].bar, v, LV_ANIM_OFF);

        uint32_t col = util_color(v);
        lv_obj_set_style_bg_color(s_c[i].dot, lv_color_hex(col), 0);
        lv_obj_set_style_text_color(s_c[i].stxt, lv_color_hex(col), 0);
        set_text_if_changed(s_c[i].stxt, v >= 90 ? "偏高" : (v >= 70 ? "注意" : "正常"));
    }

    /* 副标题：型号 / 显存 / 容量 / 盘符 */
    set_text_if_changed(s_c[0].sub, st.cpu_name[0] ? st.cpu_name : "—");
    set_text_if_changed(s_c[1].sub, st.gpu_name[0] ? st.gpu_name : "无显卡数据");

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
    set_text_if_changed(s_net_stxt, st.valid ? "已连接" : "未连接");
}

'''
src = src[:i] + body + src[j:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('done lines =', src.count('\n'))
