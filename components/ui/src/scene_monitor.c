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
    lv_obj_t *bar;
    lv_obj_t *pct;
    lv_obj_t *sub;
    uint32_t accent;
} row_t;

static lv_obj_t *s_root;
static row_t     s_row[ROW_COUNT];
static lv_obj_t *s_lbl_net_up, *s_lbl_net_dn, *s_lbl_up, *s_lbl_online;

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_HOME);
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

static void make_row(lv_obj_t *parent, int idx, uint32_t accent,
                     const char *icon, const char *label)
{
    int y = 8 + idx * ROW_H;

    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_size(circle, 40, 40);
    lv_obj_set_pos(circle, 10, y + 8);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_20, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t *ic = lv_label_create(circle);
        lv_obj_set_style_text_font(ic, &ui_font_hw, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(accent), 0);
        lv_label_set_text(ic, icon);
        lv_obj_center(ic);
    }

    lv_obj_t *t = lv_label_create(parent);
    lv_obj_set_style_text_font(t, &ui_font_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(t, label);
    lv_obj_set_pos(t, 60, y + 16);

    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 230, 10);
    lv_obj_set_pos(bar, 130, y + 23);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(parent);
    lv_obj_set_style_text_font(pct, &ui_font_28, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(pct, "--");
    lv_obj_set_pos(pct, 378, y + 8);

    lv_obj_t *sub = lv_label_create(parent);
    lv_obj_set_style_text_font(sub, &ui_font_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_SUB), 0);
    lv_label_set_text(sub, "--");
    lv_obj_set_pos(sub, 490, y + 18);
    lv_obj_set_width(sub, 240);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);

    s_row[idx].bar    = bar;
    s_row[idx].pct    = pct;
    s_row[idx].sub    = sub;
    s_row[idx].accent = accent;
}

static void build_monitor(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;

    {
        lv_obj_t *btn = lv_button_create(root);
        lv_obj_set_size(btn, 88, 34);
        lv_obj_set_pos(btn, 14, 6);
        lv_obj_set_style_radius(btn, 17, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 10, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_offset_y(btn, 2, 0);
        {
            lv_obj_t *l = lv_label_create(btn);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), 0);
            lv_label_set_text(l, LV_SYMBOL_LEFT " HOME");
            lv_obj_center(l);
        }
        lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *title = lv_label_create(root);
        lv_obj_set_style_text_font(title, &ui_font_28, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
        lv_label_set_text(title, "PC MONITOR");
        lv_obj_set_pos(title, 120, 8);
    }

    {
        lv_obj_t *card = lv_obj_create(root);
        lv_obj_set_size(card, 752, 296);
        lv_obj_set_pos(card, 24, 62);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 18, 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_offset_y(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        make_row(card, 0, C_BLUE,   "\xEF\x8B\x9B", "CPU");    /* 芯片 */
        make_row(card, 1, C_PURPLE, "\xEF\x94\xB8", "MEM");    /* 内存条 */
        make_row(card, 2, C_ORANGE, "\xEF\xA1\xA3", "GPU");    /* 风扇（显卡特征） */
        make_row(card, 3, C_GREEN,  "\xEF\x82\xA0", "C:");     /* 硬盘 */
        make_row(card, 4, C_GREEN,  "\xEF\x82\xA0", "D:");     /* 硬盘 */
    }

    {
        lv_obj_t *card = lv_obj_create(root);
        lv_obj_set_size(card, 752, 74);
        lv_obj_set_pos(card, 24, 372);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 18, 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_offset_y(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ni = lv_label_create(card);
        lv_obj_set_style_text_font(ni, &ui_font_hw, 0);
        lv_obj_set_style_text_color(ni, lv_color_hex(C_BLUE), 0);
        lv_label_set_text(ni, "\xEF\x9B\xBF");   /* 网线 */
        lv_obj_set_pos(ni, 18, 8);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &ui_font_14, 0);
        lv_obj_set_style_text_color(l1, lv_color_hex(C_SUB), 0);
        lv_label_set_text(l1, "NET");
        lv_obj_set_pos(l1, 52, 14);

        s_lbl_net_up = lv_label_create(card);
        lv_obj_set_style_text_font(s_lbl_net_up, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_net_up, lv_color_hex(C_GREEN), 0);
        lv_label_set_text(s_lbl_net_up, "UP --");
        lv_obj_set_pos(s_lbl_net_up, 20, 36);

        s_lbl_net_dn = lv_label_create(card);
        lv_obj_set_style_text_font(s_lbl_net_dn, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_net_dn, lv_color_hex(C_BLUE), 0);
        lv_label_set_text(s_lbl_net_dn, "DOWN --");
        lv_obj_set_pos(s_lbl_net_dn, 240, 36);

        s_lbl_up = lv_label_create(card);
        lv_obj_set_style_text_font(s_lbl_up, &ui_font_14, 0);
        lv_obj_set_style_text_color(s_lbl_up, lv_color_hex(C_SUB), 0);
        lv_label_set_text(s_lbl_up, "PC UP --");
        lv_obj_set_pos(s_lbl_up, 470, 18);

        s_lbl_online = lv_label_create(card);
        lv_obj_set_style_text_font(s_lbl_online, &ui_font_14, 0);
        lv_obj_set_style_text_color(s_lbl_online, lv_color_hex(C_GREEN), 0);
        lv_label_set_text(s_lbl_online, "ONLINE");
        lv_obj_set_pos(s_lbl_online, 470, 42);
    }
}

static void update_ui(void)
{
    pc_stats_t st;
    pc_monitor_get(&st);

    char b[48];

    if (!st.valid)
    {
        for (int i = 0; i < ROW_COUNT; i++)
        {
            lv_label_set_text(s_row[i].pct, "--");
            lv_label_set_text(s_row[i].sub, "offline");
            lv_bar_set_value(s_row[i].bar, 0, LV_ANIM_OFF);
        }
        lv_label_set_text(s_lbl_online, "OFFLINE");
        lv_obj_set_style_text_color(s_lbl_online, lv_color_hex(C_RED), 0);
        lv_label_set_text(s_lbl_up, "PC UP --");
        lv_label_set_text(s_lbl_net_up, "UP --");
        lv_label_set_text(s_lbl_net_dn, "DOWN --");
        return;
    }

    {
        float p = st.cpu_pct;
        uint32_t c = level_color(p, s_row[0].accent);
        snprintf(b, sizeof(b), "%d%%", (int)(p + 0.5f));
        lv_label_set_text(s_row[0].pct, b);
        lv_obj_set_style_text_color(s_row[0].pct, lv_color_hex(c), 0);
        lv_bar_set_value(s_row[0].bar, (int)p, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_row[0].bar, lv_color_hex(c), LV_PART_INDICATOR);
        snprintf(b, sizeof(b), "%d MHz   %d cores", st.cpu_mhz, st.cpu_cores);
        lv_label_set_text(s_row[0].sub, b);
    }

    {
        float p = st.mem_pct;
        uint32_t c = level_color(p, s_row[1].accent);
        snprintf(b, sizeof(b), "%d%%", (int)(p + 0.5f));
        lv_label_set_text(s_row[1].pct, b);
        lv_obj_set_style_text_color(s_row[1].pct, lv_color_hex(c), 0);
        lv_bar_set_value(s_row[1].bar, (int)p, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_row[1].bar, lv_color_hex(c), LV_PART_INDICATOR);
        snprintf(b, sizeof(b), "%.1f / %.0f GB",
                 (double)st.mem_used_gb, (double)st.mem_total_gb);
        lv_label_set_text(s_row[1].sub, b);
    }

    {
        if (st.gpu_ok)
        {
            float p = (float)st.gpu_pct;
            uint32_t c = level_color(p, s_row[2].accent);
            snprintf(b, sizeof(b), "%d%%", st.gpu_pct);
            lv_label_set_text(s_row[2].pct, b);
            lv_obj_set_style_text_color(s_row[2].pct, lv_color_hex(c), 0);
            lv_bar_set_value(s_row[2].bar, st.gpu_pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_row[2].bar, lv_color_hex(c), LV_PART_INDICATOR);
            snprintf(b, sizeof(b), "%d C   %d W", st.gpu_temp, (int)st.gpu_power);
            lv_label_set_text(s_row[2].sub, b);
        }
        else
        {
            lv_label_set_text(s_row[2].pct, "--");
            lv_label_set_text(s_row[2].sub, "no gpu data");
            lv_bar_set_value(s_row[2].bar, 0, LV_ANIM_OFF);
        }
    }

    for (int i = 0; i < 2; i++)
    {
        int idx = 3 + i;
        if (i < st.disk_count)
        {
            float p = st.disks[i].percent;
            uint32_t c = level_color(p, s_row[idx].accent);
            snprintf(b, sizeof(b), "%d%%", (int)(p + 0.5f));
            lv_label_set_text(s_row[idx].pct, b);
            lv_obj_set_style_text_color(s_row[idx].pct, lv_color_hex(c), 0);
            lv_bar_set_value(s_row[idx].bar, (int)p, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_row[idx].bar, lv_color_hex(c), LV_PART_INDICATOR);
            snprintf(b, sizeof(b), "%.0f GB free", (double)st.disks[i].free_gb);
            lv_label_set_text(s_row[idx].sub, b);
        }
        else
        {
            lv_label_set_text(s_row[idx].pct, "--");
            lv_label_set_text(s_row[idx].sub, "no disk");
            lv_bar_set_value(s_row[idx].bar, 0, LV_ANIM_OFF);
        }
    }

    if (st.net_up_kbps >= 1024.0f)
    {
        snprintf(b, sizeof(b), "UP %.1f MB/s", (double)(st.net_up_kbps / 1024.0f));
    }
    else
    {
        snprintf(b, sizeof(b), "UP %.0f KB/s", (double)st.net_up_kbps);
    }
    lv_label_set_text(s_lbl_net_up, b);

    if (st.net_down_kbps >= 1024.0f)
    {
        snprintf(b, sizeof(b), "DOWN %.1f MB/s", (double)(st.net_down_kbps / 1024.0f));
    }
    else
    {
        snprintf(b, sizeof(b), "DOWN %.0f KB/s", (double)st.net_down_kbps);
    }
    lv_label_set_text(s_lbl_net_dn, b);

    char up[24];
    fmt_uptime(st.uptime_s, up, sizeof(up));
    lv_label_set_text(s_lbl_up, up);

    lv_label_set_text(s_lbl_online, "ONLINE");
    lv_obj_set_style_text_color(s_lbl_online, lv_color_hex(C_GREEN), 0);
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
