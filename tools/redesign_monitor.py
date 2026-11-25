#!/usr/bin/env python3
"""把 scene_monitor.c 重写成科技风仪表盘（三环 + 网络 + 存储）。跑完可删。"""
P = 'components/ui/src/scene_monitor.c'
src = open(P, encoding='utf-8').read()

# ---------- 1) 数据结构与变量 ----------
old_struct = '''typedef struct
{
    lv_obj_t *bar;
    lv_obj_t *pct;
    lv_obj_t *sub;
    uint32_t accent;
} row_t;'''
new_struct = '''typedef struct
{
    lv_obj_t *arc;        /* 270° 环形仪表 */
    lv_obj_t *val;        /* 环内大数字 */
    lv_obj_t *sub;        /* 卡片底部：型号 / 容量 */
    uint32_t  color;
} gauge_t;'''
assert old_struct in src, 'struct'
src = src.replace(old_struct, new_struct, 1)

old_vars = '''static lv_obj_t *s_root;
static row_t     s_row[ROW_COUNT];
static lv_obj_t *s_lbl_net_up, *s_lbl_net_dn, *s_lbl_up, *s_lbl_online;'''
new_vars = '''static lv_obj_t *s_root;
static gauge_t   s_g[3];                 /* 0=CPU 1=MEM 2=GPU */
static lv_obj_t *s_lbl_host;
static lv_obj_t *s_lbl_online;
static lv_obj_t *s_net_up, *s_net_dn;
static lv_obj_t *s_bar_up, *s_bar_dn;
static lv_obj_t *s_disk_name[2], *s_disk_pct[2], *s_disk_free[2], *s_disk_bar[2];'''
assert old_vars in src, 'vars'
src = src.replace(old_vars, new_vars, 1)

# ---------- 2) 替换 make_row .. update_ui ----------
i = src.index('static void make_row(lv_obj_t *parent, int idx, uint32_t accent,')
j = src.index('void scene_monitor(uint16_t *fb, int frame)')   # 精确锚点：旧 update_ui 到 scene_monitor 之间全部换掉

new_body = r'''/* ---------------- 科技风仪表盘（2026-11-26 重做） ----------------
 * 参考 IQON / Eclipse 这类系统监控面板的共性：
 *   近黑底 + 单色霓虹强调 + 270° 环形仪表 + 大数字嵌在环内。
 * 刻意不做：渐变背景、大面积阴影、位移动画 —— 它们都在吃渲染预算，
 *   而这页要的是"信息密度高、静止时 0 重绘"。
 */

#define COL_PAGE   0x070B14
#define COL_PANEL_ 0x0E1522
#define COL_EDGE   0x1B2638
#define COL_TRACK  0x18202E
#define COL_TEXT_  0xE2E8F0
#define COL_MUTE   0x64748B
#define COL_CYAN   0x22D3EE
#define COL_VIOLET 0x8B5CF6
#define COL_AMBER  0xFB923C

static lv_obj_t *mk_card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_PANEL_), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_EDGE), 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *mk_text(lv_obj_t *p, int x, int y, int w,
                         const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0)
    {
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(l, txt);
    return l;
}

/** 环形仪表：270° 弧（-45°~225°），大数字嵌在环心 */
static void mk_gauge(lv_obj_t *c, int x, int y, int d, uint32_t color,
                     const char *label, gauge_t *g)
{
    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_set_size(arc, d, d);
    lv_obj_set_pos(arc, x, y);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_bg_angles(arc, -45, 225);
    lv_obj_set_style_arc_width(arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = mk_text(c, x, y + d / 2 - 28, d, &ui_font_digit_48, color, "--");

    (void)mk_text(c, x, y + d + 4, d, &ui_font_14, COL_MUTE, label);

    g->arc   = arc;
    g->val   = v;
    g->color = color;
}

static void build_monitor(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

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

    /* ---- 顶栏 ---- */
    mk_text(root, 16, 14, 0, &ui_font_14, COL_CYAN, "PC MONITOR");

    s_lbl_host = mk_text(root, 130, 14, 0, &ui_font_14, COL_MUTE, "—");
    s_lbl_online = mk_text(root, 640, 14, 0, &ui_font_14, COL_MUTE, "● OFFLINE");

    /* 返回 */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 32, 32);
    lv_obj_set_pos(back, 752, 6);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_PANEL_), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COL_EDGE), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &ui_font_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_MUTE), 0);
    lv_label_set_text(bl, "<");
    lv_obj_center(bl);

    /* ---- 三张环形卡 ---- */
    const uint32_t cols[3]   = { COL_CYAN, COL_VIOLET, COL_AMBER };
    const char    *names[3]  = { "CPU", "MEM", "GPU" };
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *c = mk_card(root, 16 + i * 260, 48, 248, 200);
        mk_gauge(c, 59, 10, 130, cols[i], names[i], &s_g[i]);
        s_g[i].sub = mk_text(c, 0, 174, 248, &ui_font_14, 0x94A3B8, "—");
    }

    /* ---- 网络卡 ---- */
    lv_obj_t *cn = mk_card(root, 16, 258, 376, 210);
    mk_text(cn, 16, 12, 0, &ui_font_14, COL_CYAN, "NETWORK");

    mk_text(cn, 16, 44, 0, &ui_font_14, COL_MUTE, "UP");
    s_net_up = mk_text(cn, 16, 62, 0, &ui_font_24, COL_TEXT_, "0.0");

    mk_text(cn, 196, 44, 0, &ui_font_14, COL_MUTE, "DOWN");
    s_net_dn = mk_text(cn, 196, 62, 0, &ui_font_24, COL_TEXT_, "0.0");

    mk_text(cn, 300, 68, 0, &ui_font_14, COL_MUTE, "MB/s");

    s_bar_up = lv_bar_create(cn);
    lv_obj_set_size(s_bar_up, 344, 8);
    lv_obj_set_pos(s_bar_up, 16, 116);
    lv_bar_set_range(s_bar_up, 0, 100);
    lv_obj_set_style_bg_color(s_bar_up, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_up, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_up, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_up, lv_color_hex(COL_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_up, 4, LV_PART_INDICATOR);

    s_bar_dn = lv_bar_create(cn);
    lv_obj_set_size(s_bar_dn, 344, 8);
    lv_obj_set_pos(s_bar_dn, 16, 140);
    lv_bar_set_range(s_bar_dn, 0, 100);
    lv_obj_set_style_bg_color(s_bar_dn, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_dn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_dn, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_dn, lv_color_hex(COL_VIOLET), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_dn, 4, LV_PART_INDICATOR);

    /* ---- 存储卡 ---- */
    lv_obj_t *cd = mk_card(root, 408, 258, 376, 210);
    mk_text(cd, 16, 12, 0, &ui_font_14, COL_AMBER, "STORAGE");

    for (int i = 0; i < 2; i++)
    {
        int yy = 46 + i * 76;
        s_disk_name[i] = mk_text(cd, 16, yy, 0, &ui_font_20, COL_TEXT_, "—");
        s_disk_pct[i]  = mk_text(cd, 250, yy + 2, 0, &ui_font_14, COL_MUTE, "—");
        s_disk_free[i] = mk_text(cd, 16, yy + 26, 0, &ui_font_14, COL_MUTE, "—");

        s_disk_bar[i] = lv_bar_create(cd);
        lv_obj_set_size(s_disk_bar[i], 344, 8);
        lv_obj_set_pos(s_disk_bar[i], 16, yy + 50);
        lv_bar_set_range(s_disk_bar[i], 0, 100);
        lv_obj_set_style_bg_color(s_disk_bar[i], lv_color_hex(COL_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_disk_bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_disk_bar[i], 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_disk_bar[i], lv_color_hex(COL_AMBER), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_disk_bar[i], 4, LV_PART_INDICATOR);
    }
}

static void update_ui(void)
{
    pc_stats_t st;
    pc_monitor_get(&st);

    char b[64];

    /* ---- 环形 ---- */
    float vals[3];
    vals[0] = st.valid ? st.cpu_pct : 0.0f;
    vals[1] = st.valid ? st.mem_pct : 0.0f;
    vals[2] = (st.valid && st.gpu_ok) ? (float)st.gpu_pct : 0.0f;

    for (int i = 0; i < 3; i++)
    {
        int v = (int)(vals[i] + 0.5f);
        if (v < 0)   { v = 0; }
        if (v > 100) { v = 100; }
        lv_arc_set_value(s_g[i].arc, v);
        snprintf(b, sizeof(b), "%d%%", v);
        set_text_if_changed(s_g[i].val, b);
    }

    /* ---- 卡片底部：型号 / 容量 ---- */
    set_text_if_changed(s_g[0].sub, st.cpu_name[0] ? st.cpu_name : "—");

    snprintf(b, sizeof(b), "%.0f / %.0f GB", (double)st.mem_used_gb, (double)st.mem_total_gb);
    set_text_if_changed(s_g[1].sub, b);

    set_text_if_changed(s_g[2].sub, st.gpu_name[0] ? st.gpu_name : "no gpu");

    /* ---- 顶栏 ---- */
    set_text_if_changed(s_lbl_host, st.host[0] ? st.host : "—");
    set_text_if_changed(s_lbl_online, st.valid ? "● ONLINE" : "● OFFLINE");
    lv_obj_set_style_text_color(s_lbl_online,
                                lv_color_hex(st.valid ? 0x34D399 : 0xEF4444), 0);

    /* ---- 网络（MB/s = kbps/1024） ---- */
    float up = st.net_up_kbps / 1024.0f;
    float dn = st.net_down_kbps / 1024.0f;
    snprintf(b, sizeof(b), "%.1f", (double)up);
    set_text_if_changed(s_net_up, b);
    snprintf(b, sizeof(b), "%.1f", (double)dn);
    set_text_if_changed(s_net_dn, b);

    int bu = (int)(up / 50.0f * 100.0f);      /* 0~50 MB/s 映射满格 */
    int bd = (int)(dn / 50.0f * 100.0f);
    lv_bar_set_value(s_bar_up, bu > 100 ? 100 : bu, LV_ANIM_OFF);
    lv_bar_set_value(s_bar_dn, bd > 100 ? 100 : bd, LV_ANIM_OFF);

    /* ---- 存储 ---- */
    for (int i = 0; i < 2; i++)
    {
        if (i < st.disk_count)
        {
            int p = (int)(st.disks[i].percent + 0.5f);
            set_text_if_changed(s_disk_name[i], st.disks[i].name);
            snprintf(b, sizeof(b), "%d%%", p);
            set_text_if_changed(s_disk_pct[i], b);
            snprintf(b, sizeof(b), "free %.0f GB", (double)st.disks[i].free_gb);
            set_text_if_changed(s_disk_free[i], b);
            lv_bar_set_value(s_disk_bar[i], p, LV_ANIM_OFF);
        }
        else
        {
            set_text_if_changed(s_disk_name[i], "—");
            set_text_if_changed(s_disk_pct[i], "");
            set_text_if_changed(s_disk_free[i], "");
            lv_bar_set_value(s_disk_bar[i], 0, LV_ANIM_OFF);
        }
    }
}

'''
src = src[:i] + new_body + src[j:]

open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('done, lines =', src.count('\n'))
