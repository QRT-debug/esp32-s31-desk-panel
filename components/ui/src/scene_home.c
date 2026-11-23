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
#include "wifi_prov.h"

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

static lv_obj_t *s_val[9];   /* 每张卡的大数值（3x3 九宫格） */
static lv_obj_t *s_sub[9];   /* 每张卡的说明文字 */

/* ---------------- 跳转 ---------------- */
static void to_monitor_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_MONITOR); }
static void to_audio_cb(lv_event_t *e)   { (void)e; ui_goto_scene(UI_SCENE_AUDIO); }
static void to_system_cb(lv_event_t *e)  { (void)e; ui_goto_scene(UI_SCENE_SYSTEM); }
static void to_weather_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_HOME); }
static void to_hf_cb(lv_event_t *e)      { (void)e; ui_goto_scene(UI_SCENE_HF); }
static void to_airank_cb(lv_event_t *e)   { (void)e; ui_goto_scene(UI_SCENE_AIRANK); }
static void to_bili_cb(lv_event_t *e)      { (void)e; ui_goto_scene(UI_SCENE_BILI); }
/* ★ 屏保/语音/配网 三个入口从 SYSTEM 页搬到这里（它们本来就是"应用"，
 *   放在系统设置页是导航混乱的根源）。 */
static void to_clock_cb(lv_event_t *e)
{
    (void)e;
    g_clock_return_scene = UI_SCENE_APPS;   /* 触摸返回回应用页 */
    ui_goto_scene(UI_SCENE_CLOCK);
}
static void to_voice_cb(lv_event_t *e) { (void)e; ui_goto_scene(UI_SCENE_VOICE); }
static lv_obj_t *s_cfg_overlay;

static void config_overlay_close(lv_event_t *e)
{
    (void)e;
    if (s_cfg_overlay != NULL)
    {
        /* 异步删：在事件回调里同步删除自身对象，LVGL 内部删除链会踩到未完成的事件流 */
        lv_obj_delete_async(s_cfg_overlay);
        s_cfg_overlay = NULL;
    }
}

/* ★ 点 CONFIG 原本只调 wifi_prov_start()，界面毫无变化（用户实测"没反应"）。
 *   配网页在 STA 上已经常驻，用户真正需要的是「访问地址」——这里弹层告诉他。 */
static void to_config_cb(lv_event_t *e)
{
    (void)e;
    wifi_prov_start();      /* 兜底：开热点，路由器不给 DHCP 时也能连 */

    /* ★ 必须用 lv_obj_is_valid：切场景时 lv_obj_clean 已经把弹层删了，
     *   但静态指针还指着那块内存（悬空）——只判 != NULL 会导致"关一次后再也打不开"。 */
    if (s_cfg_overlay != NULL && lv_obj_is_valid(s_cfg_overlay))
    {
        return;
    }
    s_cfg_overlay = NULL;

    char ip[24] = { 0 };
    wifi_sta_get_ip(ip, sizeof(ip));

    /* ★★ 必须是「全屏遮罩 + 内层卡片」：
     *   之前只做一个 540x250 的框，框周围仍是应用页卡片区，点关闭时手指落在
     *   旁边的卡片上就会直接进那个应用（实测）。遮罩铺满 800x480 后，
     *   点到任何位置都落在遮罩上 ⇒ 只关闭，不穿透。 */
    s_cfg_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_cfg_overlay, 800, 480);
    lv_obj_set_pos(s_cfg_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_cfg_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_cfg_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_cfg_overlay, 0, 0);
    lv_obj_set_style_radius(s_cfg_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_cfg_overlay, 0, 0);
    lv_obj_clear_flag(s_cfg_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cfg_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cfg_overlay, config_overlay_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_cfg_overlay);
    lv_obj_set_size(card, 540, 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    /* 卡片可点击但不绑事件：挡住点击，避免穿透到遮罩导致"点卡片也关闭" */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lt = lv_label_create(card);
    lv_obj_set_style_text_font(lt, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(lt, lv_color_hex(C_TITLE), 0);
    lv_label_set_text_fmt(lt,
        "配置页面\n\n"
        "电脑或手机打开网页：\n"
        "http://%s/\n\n"
        "连不上时连热点 ESP32-S31-SETUP，\n"
        "再访问 http://192.168.4.1/\n\n"
        "轻触本框关闭", ip);
    lv_obj_set_pos(lt, 26, 20);
}

/* 一张卡片：图标圆 + 标题 + 大数值 + 说明 */
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y,
                           uint32_t accent, const char *icon, const char *title,
                           lv_event_cb_t cb, int idx)
{
    lv_obj_t *card = lv_button_create(parent);
    /* ★ 232x113：3 列 × 3 行。原为 360 宽但坐标只隔 248 —— 卡片互相重叠、
     *   第三张超出屏幕右侧，是"布局乱"的直接原因之一。 */
    lv_obj_set_size(card, 232, 113);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* ★★ 阴影 22 -> 6：LVGL 的阴影是逐像素模糊，9 张卡片每帧都要算，
     *   触摸按下触发该卡重绘时尤其明显（用户反馈"应用页触摸变卡"）。
     *   6px + 2px 偏移视觉上仍是轻微浮起，开销降到约 1/3。 */
    lv_obj_set_style_shadow_width(card, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(card, lv_color_hex(C_SHADOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_offset_y(card, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 按下时轻微缩放感（用背景变暗模拟） */
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F2FA), LV_PART_MAIN | LV_STATE_PRESSED);

    /* 图标圆：浅色底 + 主题色图标 */
    lv_obj_t *circle = lv_obj_create(card);
    lv_obj_set_size(circle, 42, 42);
    lv_obj_set_pos(circle, 14, 12);
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
        lv_obj_set_size(frame, 33, 22);
        lv_obj_set_pos(frame, 5, 11);
        lv_obj_set_style_radius(frame, 5, 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(frame, 3, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(PINK), 0);
        lv_obj_set_style_pad_all(frame, 0, 0);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 内屏 */
        lv_obj_t *scr = lv_obj_create(circle);
        lv_obj_set_size(scr, 27, 16);
        lv_obj_set_pos(scr, 8, 14);
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
            lv_obj_set_size(eye, 2, 6);
            lv_obj_set_pos(eye, e ? 26 : 13, 18);   /* 两眼拉到更开（用户要求） */
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
            lv_obj_set_size(arc, 6, 6);
            lv_obj_set_pos(arc, m ? 21 : 16, 23);
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
            lv_obj_set_size(ant, 2, 10);
            lv_obj_set_pos(ant, a ? 29 : 12, 1);   /* 底边 y=15 贴外框顶 */
            lv_obj_set_style_radius(ant, 1, 0);
            lv_obj_set_style_bg_color(ant, lv_color_hex(PINK), 0);
            lv_obj_set_style_bg_opa(ant, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ant, 0, 0);
            lv_obj_set_style_pad_all(ant, 0, 0);
            lv_obj_clear_flag(ant, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_transform_pivot_x(ant, 1, 0);
            /* ★ 两根都绕底部中心转：支点在顶部时旋转会把底端甩离电视框（右天线实测有缝） */
            lv_obj_set_style_transform_pivot_y(ant, 9, 0);
            lv_obj_set_style_transform_rotation(ant, a ? 320 : -320, 0);
        }

        /* 两只脚（外框底部两个小圆角块） */
        for (int f = 0; f < 2; f++)
        {
            lv_obj_t *foot = lv_obj_create(circle);
            lv_obj_set_size(foot, 5, 4);
            lv_obj_set_pos(foot, f ? 24 : 13, 34);
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
    lv_obj_set_pos(t, 64, 14);

    /* 大数值 */
    s_val[idx] = lv_label_create(card);
    lv_obj_set_style_text_font(s_val[idx], &ui_font_24, 0);
    lv_obj_set_style_text_color(s_val[idx], lv_color_hex(C_TITLE), 0);
    lv_label_set_text(s_val[idx], "--");
    lv_obj_set_pos(s_val[idx], 64, 40);

    /* 说明小字 */
    s_sub[idx] = lv_label_create(card);
    lv_obj_set_style_text_font(s_sub[idx], &ui_font_14, 0);
    lv_obj_set_style_text_color(s_sub[idx], lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_sub[idx], "");
    lv_obj_set_pos(s_sub[idx], 14, 82);
    lv_obj_set_width(s_sub[idx], 204);

    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    return card;
}

static lv_obj_t *s_root;   /* 整页容器：切页动画用 */

static void build_home(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    s_cfg_overlay = NULL;   /* 上一轮的弹层已被 clean 删除，指针必须同步重置 */
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

    /* ★ 3x3 九宫格（232x113，列间距 16、行间距 14）
     *   统一布局：所有功能入口都在这张网格里，不再散落到 SYSTEM 页。 */
    const int CX[3] = { 32, 280, 528 };
    const int CY[3] = { 96, 223, 350 };

    /* 第一行：监控 / 媒体 / 系统 */
    make_card(scr, CX[0], CY[0], C_BLUE,   "\xEF\x8B\x9B", "PC MONITOR", to_monitor_cb, 0);
    make_card(scr, CX[1], CY[0], C_ORANGE, "\xEF\x80\x81", "AUDIO",      to_audio_cb,   1);
    make_card(scr, CX[2], CY[0], C_GREEN,  "\xEF\x80\x93", "SYSTEM",     to_system_cb,  2);

    /* 第二行：三个榜单（信息类） */
    make_card(scr, CX[0], CY[1], C_PURPLE, "\xEF\x81\xAD", "HF TREND",   to_hf_cb,      3);
    make_card(scr, CX[1], CY[1], C_BLUE,   "\xEF\x82\x80", "AI RANK",    to_airank_cb,  4);
    make_card(scr, CX[2], CY[1], C_ORANGE, "\xEF\x89\xAC", "BILI HOT",   to_bili_cb,    5);

    /* 第三行：屏保 / 语音 / 配网（原来在 SYSTEM 页，搬过来） */
    make_card(scr, CX[0], CY[2], C_PURPLE, "\xEF\x80\x97", "CLOCK",      to_clock_cb,   6);
    make_card(scr, CX[1], CY[2], C_GREEN,  "\xEF\x84\xB0", "VOICE",      to_voice_cb,   7);
    make_card(scr, CX[2], CY[2], C_BLUE,   "\xEF\x87\x9E", "CONFIG",     to_config_cb,  8);
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
            /* 卡片说明区宽 204px（14px 字体约 26 字符）。模型名动辄 20+ 字符，
             * 直接拼会截断；这里用与其它卡片一致的描述性文案。 */
            snprintf(b, sizeof(b), "tap to view trend");
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

    /* 7) CLOCK / VOICE / CONFIG：入口类卡片，显示状态 + 操作提示
     *   ★ 卡片字体是 ASCII，故用英文（中文字库在 cn18，字号不一致会跳）。 */
    lv_label_set_text(s_val[6], "READY");
    lv_label_set_text(s_sub[6], "wallpaper + big clock");

    lv_label_set_text(s_val[7], "TAP");
    lv_label_set_text(s_sub[7], "speak a command");

    {
        char b[32];
        snprintf(b, sizeof(b), "%s", wifi_sta_connected() ? "ONLINE" : "OFFLINE");
        lv_label_set_text(s_val[8], b);
        lv_label_set_text(s_sub[8], "wifi setup page");
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
