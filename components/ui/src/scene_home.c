/**
 * @file    scene_home.c
 * @brief   主屏 —— 平板/手机风格的 2x2 大卡片（浅色）
 *
 * 设计思路（参考 PicoPixel 的仪表盘范式 + 移动端 App 质感）：
 *   - 每张卡片三层信息：彩色图标圆（识别）→ 大数值（量）→ 灰色小字（说明）
 *   - 卡片本身就是"活的仪表"，不用进去就知道 PC 在不在线、在放什么歌
 *   - 大圆角 + 轻阴影 + 大留白，靠近 iPad/智能家居面板的观感
 *
 * 布局（800x480）：
 *   顶部  产品名（左） + 状态栏（右上，全局层）
 *   中部  2x2 卡片，每张 360x168
 *   底部  一行汇总（可选）
 */
#include <stdio.h>
#include <string.h>

#include "audio_player.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "pc_monitor.h"
#include "sd_card.h"
#include "ui_fonts.h"
#include "hf_trend.h"
#include "ai_rank.h"
#include "bili_hot.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "wifi_sta.h"

/* 浅色规范（与播放器页保持一致）——11-20 主题化，指向可切换色板 */
#define C_BG_TOP   (ui_theme()->bg_top)
#define C_BG_BOT   (ui_theme()->bg_bot)
#define C_CARD     (ui_theme()->card)
#define C_TITLE    (ui_theme()->title)
#define C_SUB      (ui_theme()->sub)
#define C_FAINT    (ui_theme()->faint)
#define C_SHADOW   (ui_theme()->shadow)

#define C_BLUE     0x4C9AFF
#define C_ORANGE   0xF2A03D
#define C_GREEN    0x35D07F
#define C_PURPLE   (ui_theme()->accent)

static lv_obj_t *s_val[6];   /* 每张卡的大数值 */
static lv_obj_t *s_sub[6];   /* 每张卡的说明文字 */

/* ---------------- 跳转 ---------------- */
static void to_monitor_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_MONITOR); }
static void to_audio_cb(lv_event_t *e)   { (void)e; ui_goto_scene(UI_SCENE_AUDIO); }
static void to_system_cb(lv_event_t *e)  { (void)e; ui_goto_scene(UI_SCENE_SYSTEM); }
static void to_weather_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_HOME); }
static void to_hf_cb(lv_event_t *e)      { (void)e; ui_goto_scene(UI_SCENE_HF); }
static void to_airank_cb(lv_event_t *e)   { (void)e; ui_goto_scene(UI_SCENE_AIRANK); }
static void to_bili_cb(lv_event_t *e)      { (void)e; ui_goto_scene(UI_SCENE_BILI); }

/* 一张卡片：图标圆 + 标题 + 大数值 + 说明 */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y,
                           uint32_t accent, const char *icon, const char *title,
                           lv_event_cb_t cb, int idx)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, 360, 168);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(card, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(card, lv_color_hex(C_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(card, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 按下时轻微缩放感（用背景变暗模拟） */
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F2FA), LV_PART_MAIN | LV_STATE_PRESSED);

    /* 图标圆：浅色底 + 主题色图标 */
    lv_obj_t *circle = lv_obj_create(card);
    lv_obj_set_size(circle, 56, 56);
    lv_obj_set_pos(circle, 22, 22);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_20, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    if (idx == 5)
    {
        /* ★ B 站小电视（按官方 logo 形态复刻，56x56 描边风）：
         *   外框(描边) + 内屏(描边) + 两只斜笑眼 + 两根天线 + 两只脚
         *   坑：① lv_line 画不出来（坐标受父 padding 影响）→ 全用旋转矩形拼线段
         *       ② 图标圆默认 padding 会让绝对坐标偏移 → 先 pad_all(0) */
        const uint32_t PINK = 0xFB7299;
        lv_obj_set_style_pad_all(circle, 0, 0);

        /* 外框 */
        lv_obj_t *frame = lv_obj_create(circle);
        lv_obj_set_size(frame, 44, 30);
        lv_obj_set_pos(frame, 6, 15);
        lv_obj_set_style_radius(frame, 5, 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(frame, 3, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(PINK), 0);
        lv_obj_set_style_pad_all(frame, 0, 0);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 内屏 */
        lv_obj_t *scr = lv_obj_create(circle);
        lv_obj_set_size(scr, 36, 22);
        lv_obj_set_pos(scr, 10, 19);
        lv_obj_set_style_radius(scr, 3, 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scr, 2, 0);
        lv_obj_set_style_border_color(scr, lv_color_hex(PINK), 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 两只斜笑眼（左 / 右 \，各 2x8 旋转 40 度） */
        for (int e = 0; e < 2; e++)
        {
            lv_obj_t *eye = lv_obj_create(circle);
            lv_obj_set_size(eye, 2, 8);
            lv_obj_set_pos(eye, e ? 35 : 17, 24);   /* 两眼拉到更开（用户要求） */
            lv_obj_set_style_radius(eye, 1, 0);
            lv_obj_set_style_bg_color(eye, lv_color_hex(PINK), 0);
            lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(eye, 0, 0);
            lv_obj_set_style_pad_all(eye, 0, 0);
            lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_transform_pivot_x(eye, 1, 0);
            lv_obj_set_style_transform_pivot_y(eye, 4, 0);
            /* 左眼 \ （+40°）、右眼 / （-40°）——向中间聚拢，才是 B 站的眯眼笑 */
            lv_obj_set_style_transform_rotation(eye, e ? -400 : 400, 0);
        }

        /* 嘴：两个下半圆弧拼成 ω（B 站 logo 的嘴；横线是错的） */
        for (int m = 0; m < 2; m++)
        {
            lv_obj_t *arc = lv_arc_create(circle);
            lv_obj_set_size(arc, 8, 8);
            lv_obj_set_pos(arc, m ? 28 : 21, 31);
            lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(PINK), LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);       /* 不要背景弧 */
            lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(arc, 0, 0);
            lv_obj_set_style_pad_all(arc, 0, 0);
            /* ★ LVGL 角度从 3 点方向顺时针增加：0~180 才是下半圆（下凸 ∪）
             *   写 180~360 会画成上半圆 ∩（像眉毛）——实测踩过。 */
            lv_arc_set_angles(arc, 0, 180);
            lv_obj_remove_style(arc, NULL, LV_PART_KNOB);   /* 去掉旋钮 */
            lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        }

        /* 两根天线（2x12，绕底部转 ±32 度） */
        for (int a = 0; a < 2; a++)
        {
            lv_obj_t *ant = lv_obj_create(circle);
            lv_obj_set_size(ant, 2, 13);
            lv_obj_set_pos(ant, a ? 38 : 16, 2);   /* 底边 y=15 贴外框顶 */
            lv_obj_set_style_radius(ant, 1, 0);
            lv_obj_set_style_bg_color(ant, lv_color_hex(PINK), 0);
            lv_obj_set_style_bg_opa(ant, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ant, 0, 0);
            lv_obj_set_style_pad_all(ant, 0, 0);
            lv_obj_clear_flag(ant, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_transform_pivot_x(ant, 1, 0);
            /* ★ 两根都绕底部中心转：支点在顶部时旋转会把底端甩离电视框（右天线实测有缝） */
            lv_obj_set_style_transform_pivot_y(ant, 12, 0);
            lv_obj_set_style_transform_rotation(ant, a ? 320 : -320, 0);
        }

        /* 两只脚（外框底部两个小圆角块） */
        for (int f = 0; f < 2; f++)
        {
            lv_obj_t *foot = lv_obj_create(circle);
            lv_obj_set_size(foot, 7, 5);
            lv_obj_set_pos(foot, f ? 32 : 17, 45);
            lv_obj_set_style_radius(foot, 2, 0);
            lv_obj_set_style_bg_color(foot, lv_color_hex(PINK), 0);
            lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(foot, 0, 0);
            lv_obj_set_style_pad_all(foot, 0, 0);
            lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }
    }
    else
    {
        lv_obj_t *ic = lv_label_create(circle);
        lv_obj_set_style_text_font(ic, &ui_font_hw, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(accent), 0);
        lv_label_set_text(ic, icon);
        lv_obj_center(ic);
    }

    /* 标题（右上） */
    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &ui_font_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 96, 30);

    /* 大数值 */
    s_val[idx] = lv_label_create(card);
    lv_obj_set_style_text_font(s_val[idx], &ui_font_28, 0);
    lv_obj_set_style_text_color(s_val[idx], lv_color_hex(C_TITLE), 0);
    lv_label_set_text(s_val[idx], "--");
    lv_obj_set_pos(s_val[idx], 24, 92);

    /* 说明小字 */
    s_sub[idx] = lv_label_create(card);
    lv_obj_set_style_text_font(s_sub[idx], &ui_font_14, 0);
    lv_obj_set_style_text_color(s_sub[idx], lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_sub[idx], "");
    lv_obj_set_pos(s_sub[idx], 26, 132);

    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    return card;
}

static lv_obj_t *s_root;   /* 整页容器：切页动画用 */

static void build_home(void)
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

    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部产品名 */
    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &ui_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(title, "ESP32-S31");
    lv_obj_set_pos(title, 32, 26);

    lv_obj_t *sub = lv_label_create(root);
    lv_obj_set_style_text_font(sub, &ui_font_14, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_SUB), 0);
    lv_label_set_text(sub, "desktop terminal");
    lv_obj_set_pos(sub, 34, 62);

    /* ★ 11-20：本页降级为"应用页"，右上角返回天气主页 */
    {
        lv_obj_t *back = lv_button_create(root);
        lv_obj_set_size(back, 120, 32);
        lv_obj_set_pos(back, 648, 58);   /* 状态栏本体到 y32、阴影到 y46，再留 12px 间隙 */
        lv_obj_set_style_radius(back, 17, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_shadow_width(back, 8, 0);
        lv_obj_set_style_shadow_opa(back, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(back, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_offset_y(back, 2, 0);
        lv_obj_add_event_cb(back, to_weather_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);   /* symbol 只在 montserrat 有字形 */
        lv_obj_set_style_text_color(bl, lv_color_hex(C_TITLE), 0);
        lv_label_set_text(bl, LV_SYMBOL_LEFT " WEATHER");
        lv_obj_center(bl);
    }

    /* 3 + 2 卡片（232 宽，间距 16） */
    make_card(scr,  32,  96, C_BLUE,   "\xEF\x8B\x9B", "PC MONITOR",  /* 芯片 */
              to_monitor_cb, 0);
    make_card(scr, 280,  96, C_ORANGE, "\xEF\x80\x81", "AUDIO",       /* 音符 */
              to_audio_cb, 1);
    make_card(scr, 528,  96, C_GREEN,  "\xEF\x80\x93", "SYSTEM",      /* 齿轮 */
              to_system_cb, 2);
    make_card(scr,  32, 280, C_PURPLE, "\xEF\x81\xAD", "HF TREND",    /* 火焰 */
              to_hf_cb, 3);
    make_card(scr, 280, 280, C_BLUE,   "\xEF\x82\x80", "AI RANK",     /* 榜单 */
              to_airank_cb, 4);
    make_card(scr, 528, 280, C_ORANGE, "\xEF\x89\xAC", "BILI HOT",    /* 播放 */
              to_bili_cb, 5);
}

/* 每 500ms 刷一次卡片数值（读各模块的真实/模拟状态） */
static void refresh_cards(void)
{
    /* 1) PC MONITOR */
    {
        pc_stats_t st;
        pc_monitor_get(&st);
        char b[32];
        if (st.valid)
        {
            snprintf(b, sizeof(b), "%d%%", (int)(st.cpu_pct + 0.5f));
            lv_label_set_text(s_val[0], b);
            snprintf(b, sizeof(b), "CPU LOAD    %.1f / %.0f GB",
                     (double)st.mem_used_gb, (double)st.mem_total_gb);
            lv_label_set_text(s_sub[0], b);
        }
        else
        {
            lv_label_set_text(s_val[0], "--");
            lv_label_set_text(s_sub[0], "PC OFFLINE");
        }
    }

    /* 2) AUDIO */
    {
        if (audio_player_is_playing())
        {
            uint32_t pos = audio_player_position_ms() / 1000u;
            uint32_t dur = audio_player_duration_ms() / 1000u;
            char b[32];
            snprintf(b, sizeof(b), "%u:%02u / %u:%02u",
                     (unsigned)(pos / 60), (unsigned)(pos % 60),
                     (unsigned)(dur / 60), (unsigned)(dur % 60));
            lv_label_set_text(s_val[1], "PLAYING");
            lv_label_set_text(s_sub[1], b);
        }
        else
        {
            lv_label_set_text(s_val[1], "PAUSED");
            lv_label_set_text(s_sub[1], "tap to open player");
        }
    }

    /* 3) SYSTEM：WiFi + 内存 */
    {
        char b[40];
        int rssi = wifi_sta_rssi();
        if (wifi_sta_connected())
        {
            snprintf(b, sizeof(b), "%d dBm", rssi);
            lv_label_set_text(s_val[2], b);
            lv_label_set_text(s_sub[2], "WIFI   MEM 210 KB FREE");
        }
        else
        {
            lv_label_set_text(s_val[2], "OFF");
            lv_label_set_text(s_sub[2], "WIFI DISCONNECTED");
        }
    }

    /* 4) HF TREND：模型热榜（原 STORAGE 位） */
    {
        char b[48];
        hf_data_t h;
        hf_get(&h);
        if (h.state == HF_STATE_OK && h.count > 0)
        {
            snprintf(b, sizeof(b), "%d", h.count);
            lv_label_set_text(s_val[3], b);
            snprintf(b, sizeof(b), "MODELS   #1 %.14s", h.item[0].id);
            lv_label_set_text(s_sub[3], b);
        }
        else if (h.state == HF_STATE_FAIL)
        {
            lv_label_set_text(s_val[3], "--");
            lv_label_set_text(s_sub[3], "fetch failed");
        }
        else
        {
            lv_label_set_text(s_val[3], "..");
            lv_label_set_text(s_sub[3], "loading ranking");
        }
    }
    /* 6) BILI HOT：B 站热搜 */
    {
        char b[48];
        bili_data_t bl;
        bili_get(&bl);
        if (bl.state == BILI_OK && bl.count > 0)
        {
            snprintf(b, sizeof(b), "%d", bl.count);
            lv_label_set_text(s_val[5], b);
            snprintf(b, sizeof(b), "TRENDING / TOP %d", bl.count);   /* 卡片字体无汉字，用 ASCII */
            lv_label_set_text(s_sub[5], b);
        }
        else if (bl.state == BILI_FAIL)
        {
            lv_label_set_text(s_val[5], "--");
            lv_label_set_text(s_sub[5], "fetch failed");
        }
        else
        {
            lv_label_set_text(s_val[5], "..");
            lv_label_set_text(s_sub[5], "loading");
        }
    }

    /* 5) AI RANK：国内 AI 产品榜（AICPB，季度更新） */
    {
        char b[48];
        ai_rank_data_t a;
        ai_rank_get(&a);
        if (a.state == AI_RANK_OK && a.web_count > 0)
        {
            snprintf(b, sizeof(b), "%d", a.web_count);
            lv_label_set_text(s_val[4], b);
            snprintf(b, sizeof(b), "CN TOP   #1 %.12s", a.web[0].name);
            lv_label_set_text(s_sub[4], b);
        }
        else if (a.state == AI_RANK_FAIL)
        {
            lv_label_set_text(s_val[4], "--");
            lv_label_set_text(s_sub[4], "fetch failed");
        }
        else
        {
            lv_label_set_text(s_val[4], "..");
            lv_label_set_text(s_sub[4], "loading ranking");
        }
    }
}

void scene_home(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_home();
        refresh_cards();
    }

    /* 每 30 帧（≈0.5s）刷一次，避免每帧动 LVGL 对象 */
    if ((frame % 15) == 0)     /* 约 250ms 刷一次，数值更"活" */
    {
        refresh_cards();
    }

    lvgl_port_task();
}
