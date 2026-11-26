/**
 * @file    scene_audio.c
 * @brief   正式版音乐播放器界面（浅色）
 *
 * 布局参考 LVGL 官方 lv_demo_music（横向/landscape 模式）：
 *   左上   曲名 / 来源 / 格式 三段文字
 *   左中   控制栏：上一首、大播放键（紫色实心圆）、下一首
 *   左下   进度条 + 已播时间 / 总时长
 *   底部   ◀ 曲目切换 ▶
 *   右侧   圆形封面占位（程序绘制渐变 + 音符图标，零图片资源）
 *   右下   音量滑块 + 百分比
 *
 * ★ 不引入 demo 的 21MB 专辑图片：封面用 LVGL 渐变 + 字形绘制。
 * ★ 播放源只有 SD 卡；flash 分区的曲目不再有入口（硬件自检用）。
 */
#include <stdio.h>
#include <string.h>

#include "audio_player.h"
#include "sd_card.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"

/* ★ 11-20 01:35 主题化：原浅色硬编码全部指向 ui_theme() 色板（可切深色）。
 *   COL_BG 已无使用处（旧黑主题残留，保留定义防外链）。 */
#define COL_BG     (ui_theme()->panel_dark)
#define COL_TEXT   (ui_theme()->panel_text)
#define COL_DIM    (ui_theme()->panel_dim)
#define COL_ORANGE 0xF2A03D
#define COL_GREEN  0x35D07F
#define COL_TRACK  (ui_theme()->track)

/* 浅色主题（参考 LVGL 官方 lv_demo_music 的布局与配色） */
#define C_BG_TOP    (ui_theme()->bg_top)     /* 米白（第三档）：IPS 100% 背光下纯白太刺眼 */
#define C_BG_BOT    (ui_theme()->bg_bot)
#define C_CARD      (ui_theme()->card)       /* 卡片略灰，避免刺眼 */
#define C_TITLE     (ui_theme()->title)
#define C_SUB       (ui_theme()->sub)
#define C_FAINT     (ui_theme()->faint)
#define C_ACCENT    (ui_theme()->accent)     /* 紫 */
#define C_ACCENT_L  (ui_theme()->accent_l)   /* 浅紫（封面底） */
#define C_ACCENT_L2 (ui_theme()->accent_l2)
#define C_SHADOW    (ui_theme()->shadow)

static lv_obj_t *s_btn_play;
static lv_obj_t *s_lbl_play;       /* 播放按钮里的字 */
static lv_obj_t *s_lbl_vol;        /* 音量数字 */
static lv_obj_t *s_lbl_state;      /* 底部状态行 */
static int       s_track_idx = 0;  /* 当前曲目 */

/* SD 卡曲目表（扫描 /sdcard 根目录所有 .wav，最多 16 首） */
static char s_wav[16][64];
static int  s_wav_n;
static int  s_wav_idx;
static lv_obj_t *s_lbl_name;

/* 歌单（右侧列表，2026-11-26）：点击行即切歌 */
static lv_obj_t *s_pl_box;
static lv_obj_t *s_pl_row[16];
static const char *current_short_name(void);   /* 前向声明 */

/* 正式版界面的控件（build 里创建，轮询里更新） */
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_bar;
static lv_obj_t *s_lbl_pos;
static lv_obj_t *s_lbl_dur;
static bool      s_debug_visible;   /* 调试层（DIAG + 状态行）是否显示 */
static lv_obj_t *s_btn_diag;       /* 诊断按钮（嘶嘶声分层定位） */
static lv_obj_t *s_lbl_diag;

static const char *k_diag_name[8] = { "DIAG: OFF", "DIAG: MUTE", "DIAG: PA OFF",
                                      "DIAG: ALL OFF", "DIAG: SILENCE", "DIAG: SINE 1k",
                                      "DIAG: SINE 8k", "DIAG: RAM MUSIC" };

/* UI 显示名 / 分区里的文件名（8.3 短名，FATFS 无 LFN 也能开） */

/* ---------------- 回调 ---------------- */

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);
}

/* 把大圆播放键刷新成"播放中"外观 */
static void set_playing_ui(bool on, const char *what)
{
    lv_label_set_text(s_lbl_play, on ? "STOP" : "PLAY");
    /* 浅色主题：停止=主色紫，播放中=红粉（提示可停止）。
     * ★ 旧代码在"停止"时设成 0x161A22（黑），会把 build 里的紫色覆盖掉。 */
    lv_obj_set_style_bg_color(s_btn_play,
                              lv_color_hex(on ? 0xE8556D : C_ACCENT),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    char b[64];
    if (on)
    {
        snprintf(b, sizeof(b), "playing %s  vol %d", what, audio_player_get_volume());
    }
    else
    {
        snprintf(b, sizeof(b), "stopped  vol %d", audio_player_get_volume());
    }
    lv_label_set_text(s_lbl_state, b);
}

/* 按驱动状态机刷新大圆按钮 + 状态行（LOADING 时也可按 STOP 取消） */
/** 歌单高亮：当前曲目行用强调色 + 半透明底 */
static void pl_highlight(void)
{
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        if (s_pl_row[i] == NULL)
        {
            continue;
        }
        bool cur = (i == s_wav_idx);
        lv_obj_set_style_bg_color(s_pl_row[i],
                                  lv_color_hex(cur ? C_ACCENT : C_CARD), 0);
        lv_obj_set_style_bg_opa(s_pl_row[i], cur ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
}

static void refresh_state_ui(void)
{
    audio_player_state_t st = audio_player_state();
    pl_highlight();

    if (st == AUD_STATE_PLAYING)
    {
        set_playing_ui(true, current_short_name());
    }
    else if (st == AUD_STATE_LOADING)
    {
        char b[80];
        lv_label_set_text(s_lbl_play, "STOP");
        lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0xE8556D),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        snprintf(b, sizeof(b), "loading %s ...", current_short_name());
        lv_label_set_text(s_lbl_state, b);
    }
    else
    {
        set_playing_ui(false, NULL);
    }
}

/* 播放当前选中的 SD 曲目（大圆按钮与曲目名按钮共用） */
static void start_current_track(void)
{
    if (!sd_card_ready())
    {
        lv_label_set_text(s_lbl_state, "no SD card");
        return;
    }
    if (s_wav_n <= 0)
    {
        lv_label_set_text(s_lbl_state, "no wav on SD card");
        return;
    }

    esp_err_t err = audio_player_play_sd_by_name(s_wav[s_wav_idx]);
    if (err == ESP_OK)
    {
        refresh_state_ui();
    }
    else
    {
        lv_label_set_text(s_lbl_state, "player busy / sd error");
    }
}

static void play_cb(lv_event_t *e)
{
    (void)e;

    if (audio_player_state() != AUD_STATE_IDLE)   /* 播放中或加载中 → 停止/取消 */
    {
        audio_player_stop();
        set_playing_ui(false, NULL);
    }
    else
    {
        start_current_track();   /* ★ 大圆 Play 也走 SD（用户 11-18 要求） */
    }
}

/* 当前选中曲目的显示名（去掉 .wav 后缀）；没有曲目返回 "SD" */
static const char *current_short_name(void)
{
    static char nm[64];
    if (s_wav_n <= 0)
    {
        return "SD";
    }
    snprintf(nm, sizeof(nm), "%s", s_wav[s_wav_idx]);
    char *dot = strrchr(nm, '.');
    if (dot != NULL)
    {
        *dot = '\0';
    }
    return nm;
}

static void update_track_label(void)
{
    if (s_lbl_name == NULL)
    {
        return;
    }
    if (s_wav_n <= 0)
    {
        lv_label_set_text(s_lbl_name, "no wav on SD card");
        if (s_lbl_title != NULL)
        {
            lv_label_set_text(s_lbl_title, "No track");
        }
        return;
    }

    char nm[64];
    snprintf(nm, sizeof(nm), "%s", s_wav[s_wav_idx]);
    char *dot = strrchr(nm, '.');
    if (dot != NULL)
    {
        *dot = '\0';
    }

    char b[96];
    snprintf(b, sizeof(b), "%d/%d   %s", s_wav_idx + 1, s_wav_n, nm);
    lv_label_set_text(s_lbl_name, b);

    if (s_lbl_title != NULL)
    {
        lv_label_set_text(s_lbl_title, nm);
    }
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_wav_n <= 0)
    {
        return;
    }
    s_wav_idx = (s_wav_idx - 1 + s_wav_n) % s_wav_n;
    update_track_label();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_wav_n <= 0)
    {
        return;
    }
    s_wav_idx = (s_wav_idx + 1) % s_wav_n;
    update_track_label();
}

static void play_current_cb(lv_event_t *e)
{
    (void)e;

    start_current_track();
}


static void diag_cb(lv_event_t *e)
{
    (void)e;
    int mode = (audio_player_diag_get() + 1) % 8;
    audio_player_diag_set(mode);
    lv_label_set_text(s_lbl_diag, k_diag_name[mode]);

    static const char *hint[8] = {
        "diag off  (normal)",
        "DAC muted, PA on: hiss here = amp/power",
        "PA off: should be fully silent",
        "DAC muted + PA off: silent reference",
        "digital silence source (all-zero data)",
        "1kHz sine -20dBFS (clean digital source)",
        "8kHz sine -20dBFS (high-freq path test)",
        "music from internal RAM (bypass PSRAM)",
    };
    lv_label_set_text(s_lbl_state, hint[mode]);
}

/* 长按曲名：显示/隐藏调试层（DIAG 档位 + 状态行） */
static void debug_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_debug_visible = !s_debug_visible;

    if (s_btn_diag != NULL)
    {
        if (s_debug_visible) lv_obj_remove_flag(s_btn_diag, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(s_btn_diag, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_state != NULL)
    {
        if (s_debug_visible) lv_obj_remove_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
    }
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int vol = (int)lv_slider_get_value(slider);

    audio_player_set_volume(vol);

    char b[16];
    snprintf(b, sizeof(b), "%d%%", vol);
    lv_label_set_text(s_lbl_vol, b);

    if (audio_player_is_playing())
    {
        char s[112];
        snprintf(s, sizeof(s), "playing %s  vol %d", current_short_name(), vol);
        lv_label_set_text(s_lbl_state, s);
    }
}

/* ---------------- 小工具 ---------------- */

static lv_obj_t *home_button(lv_obj_t *scr)
{
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 90, 32);
    lv_obj_set_pos(btn, 12, 6);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ui_theme()->panel_dark), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(ui_theme()->panel_line), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(l, LV_SYMBOL_LEFT " HOME");
    lv_obj_center(l);

    lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *small_title(lv_obj_t *scr, int x, int y, const char *txt)
{
    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &ui_font_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(t, txt);
    lv_obj_set_pos(t, x, y);
    return t;
}

/* ---------------- 建界面 ---------------- */

/* 把毫秒格式化成 m:ss */
static void fmt_ms(char *buf, size_t len, uint32_t ms)
{
    uint32_t sec = ms / 1000u;
    snprintf(buf, len, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

/* 圆角白色按钮（带轻微阴影，模拟 demo 里的悬浮控件） */
static lv_obj_t *round_btn(lv_obj_t *parent, int x, int y, int size, int radius)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 18, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(C_SHADOW), 0);
    lv_obj_set_style_shadow_offset_y(b, 4, 0);
    return b;
}

static lv_obj_t *s_root;   /* 整页容器：切页动画用 */

/** 歌单行点击：切到该曲并立即播放 */
static void pl_row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        if (s_pl_row[i] == row)
        {
            s_wav_idx = i;
            audio_player_play_sd_by_name(s_wav[i]);
            refresh_state_ui();
            break;
        }
    }
}

static void build_audio(void)
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

    /* 浅色渐变背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 扫描 SD 卡曲目（重写界面时漏掉了这一行，导致曲名永远是 No track） */
    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    /* ---------------- 右上：圆形"封面"（程序绘制，占位） ---------------- */
    lv_obj_t *cover = lv_obj_create(root);
    lv_obj_set_size(cover, 100, 100);      /* 缩小，把下方让给歌单 */
    lv_obj_set_pos(cover, 616, 92);
    lv_obj_set_style_radius(cover, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(C_ACCENT_L), 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_hex(C_ACCENT_L2), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_shadow_width(cover, 40, 0);
    lv_obj_set_style_shadow_opa(cover, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(cover, lv_color_hex(C_SHADOW), 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *ic = lv_label_create(cover);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(C_ACCENT), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_center(ic);
    }

    /* ---------------- 右侧：歌单列表（点行即切歌，2026-11-26） ----------------
     * 曲目表来自 sd_card_scan_wav()（/sdcard 根目录 .wav，最多 16 首）。
     * 容器可滚动；当前曲目由 pl_highlight() 上色。 */
    s_pl_box = lv_obj_create(root);
    lv_obj_set_pos(s_pl_box, 548, 210);
    lv_obj_set_size(s_pl_box, 236, 168);   /* 4 行；再高会压到下方音量区(366~400) */
    lv_obj_set_style_bg_color(s_pl_box, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(s_pl_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pl_box, 14, 0);
    lv_obj_set_style_border_width(s_pl_box, 0, 0);
    lv_obj_set_style_pad_all(s_pl_box, 6, 0);
    lv_obj_set_scrollbar_mode(s_pl_box, LV_SCROLLBAR_MODE_AUTO);

    if (s_wav_n <= 0)
    {
        lv_obj_t *t = lv_label_create(s_pl_box);
        lv_obj_set_style_text_font(t, &ui_font_14, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_SUB), 0);
        lv_label_set_text(t, "no wav on SD");
        lv_obj_center(t);
    }
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        lv_obj_t *row = lv_obj_create(s_pl_box);
        lv_obj_set_size(row, 220, 38);
        lv_obj_set_pos(row, 0, i * 41);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, pl_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *n = lv_label_create(row);
        lv_obj_set_style_text_font(n, &ui_font_14, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(C_SUB), 0);
        char nb[8];
        snprintf(nb, sizeof(nb), "%02d", i + 1);
        lv_label_set_text(n, nb);
        lv_obj_set_pos(n, 10, 9);

        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_style_text_font(t, &ui_font_14, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(C_TITLE), 0);
        lv_obj_set_pos(t, 38, 9);
        lv_obj_set_width(t, 168);
        lv_obj_set_height(t, 22);          /* ★ 必须限高：LONG_DOT 不限高会被撑成多行 */
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, s_wav[i]);

        s_pl_row[i] = row;
    }

    /* ---------------- 左上：HOME 返回 ---------------- */
    {
        lv_obj_t *bh = round_btn(scr, 24, 12, 40, LV_RADIUS_CIRCLE);
        lv_obj_set_style_shadow_width(bh, 10, 0);
        lv_obj_t *lh = lv_label_create(bh);
        lv_obj_set_style_text_font(lh, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lh, lv_color_hex(C_SUB), 0);
        lv_label_set_text(lh, LV_SYMBOL_LEFT);
        lv_obj_center(lh);
        lv_obj_add_event_cb(bh, home_cb, LV_EVENT_CLICKED, NULL);
    }

    /* ---------------- 左：曲目信息 ---------------- */
    s_lbl_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_title, &ui_font_28, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(C_TITLE), 0);
    lv_label_set_text(s_lbl_title, "No track");
    lv_obj_set_pos(s_lbl_title, 36, 64);
    lv_obj_set_width(s_lbl_title, 470);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_title, debug_toggle_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *s_lbl_artist = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_artist, &ui_font_20, 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_lbl_artist, "SD CARD    -    Local Library");
    lv_obj_set_pos(s_lbl_artist, 36, 104);

    {
        lv_obj_t *fmt = lv_label_create(root);
        lv_obj_set_style_text_font(fmt, &ui_font_14, 0);
        lv_obj_set_style_text_color(fmt, lv_color_hex(C_FAINT), 0);
        lv_label_set_text(fmt, "44.1 kHz   |   16 bit   |   Mono");
        lv_obj_set_pos(fmt, 36, 136);
    }

    /* ---------------- 控制栏：上一首 / 播放（大） / 下一首 ---------------- */
    lv_obj_t *bprev = round_btn(scr, 36, 210, 64, LV_RADIUS_CIRCLE);
    {
        lv_obj_t *l = lv_label_create(bprev);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TITLE), 0);
        lv_label_set_text(l, LV_SYMBOL_PREV);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(bprev, prev_cb, LV_EVENT_CLICKED, NULL);

    /* 大播放键：紫色实心圆 */
    s_btn_play = lv_button_create(root);
    lv_obj_set_size(s_btn_play, 104, 104);
    lv_obj_set_pos(s_btn_play, 132, 190);
    lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* ★ 必须显式指定 part/state：lv_button 的主题样式会覆盖掉只写 0 的设置 */
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(0x5A3DD8), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_btn_play, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_btn_play, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_btn_play, 26, 0);
    lv_obj_set_style_shadow_opa(s_btn_play, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(s_btn_play, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_shadow_offset_y(s_btn_play, 6, 0);
    s_lbl_play = lv_label_create(s_btn_play);
    lv_obj_set_style_text_font(s_lbl_play, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
    lv_obj_center(s_lbl_play);
    lv_obj_add_event_cb(s_btn_play, play_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bnext = round_btn(scr, 268, 210, 64, LV_RADIUS_CIRCLE);
    {
        lv_obj_t *l = lv_label_create(bnext);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TITLE), 0);
        lv_label_set_text(l, LV_SYMBOL_NEXT);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(bnext, next_cb, LV_EVENT_CLICKED, NULL);

    /* ---------------- 进度条 + 时间 ---------------- */
    s_bar = lv_bar_create(root);
    lv_obj_set_size(s_bar, 460, 8);
    lv_obj_set_pos(s_bar, 36, 322);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xD2D7E6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);

    s_lbl_pos = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_pos, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_pos, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_lbl_pos, "0:00");
    lv_obj_set_pos(s_lbl_pos, 36, 338);

    s_lbl_dur = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_dur, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_dur, lv_color_hex(C_SUB), 0);
    lv_label_set_text(s_lbl_dur, "0:00");
    lv_obj_set_pos(s_lbl_dur, 462, 338);

    /* ---------------- 曲目切换：◀ [1/3 曲名] ▶ ---------------- */
    lv_obj_t *bsel_prev = round_btn(scr, 36, 372, 48, 14);
    {
        lv_obj_t *l = lv_label_create(bsel_prev);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), 0);
        lv_label_set_text(l, LV_SYMBOL_LEFT);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(bsel_prev, prev_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bname = lv_button_create(root);
    lv_obj_set_size(bname, 372, 48);
    lv_obj_set_pos(bname, 92, 372);
    lv_obj_set_style_radius(bname, 14, 0);
    lv_obj_set_style_bg_color(bname, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(bname, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bname, 0, 0);
    lv_obj_set_style_shadow_width(bname, 12, 0);
    lv_obj_set_style_shadow_opa(bname, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(bname, lv_color_hex(C_SHADOW), 0);
    lv_obj_set_style_shadow_offset_y(bname, 3, 0);
    s_lbl_name = lv_label_create(bname);
    lv_obj_set_style_text_font(s_lbl_name, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_name, lv_color_hex(C_TITLE), 0);
    lv_obj_set_width(s_lbl_name, 344);
    lv_label_set_long_mode(s_lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_center(s_lbl_name);
    lv_obj_add_event_cb(bname, play_current_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bsel_next = round_btn(scr, 472, 372, 48, 14);
    {
        lv_obj_t *l = lv_label_create(bsel_next);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_SUB), 0);
        lv_label_set_text(l, LV_SYMBOL_RIGHT);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(bsel_next, next_cb, LV_EVENT_CLICKED, NULL);

    /* ---------------- 右下：音量 ---------------- */
    {
        lv_obj_t *vl = lv_label_create(root);
        lv_obj_set_style_text_font(vl, &ui_font_14, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(C_FAINT), 0);
        lv_label_set_text(vl, "VOLUME");
        lv_obj_set_pos(vl, 548, 366);
    }

    s_lbl_vol = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_vol, &ui_font_20, 0);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(C_TITLE), 0);
    {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", audio_player_get_volume());
        lv_label_set_text(s_lbl_vol, b);
    }
    lv_obj_set_pos(s_lbl_vol, 736, 360);

    lv_obj_t *sl = lv_slider_create(root);
    lv_obj_set_size(sl, 236, 8);
    lv_obj_set_pos(sl, 548, 392);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, audio_player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xE2E5F0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sl, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---------------- 底部状态行（小字） ---------------- */
    s_lbl_state = lv_label_create(root);
    lv_obj_set_style_text_font(s_lbl_state, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(C_FAINT), 0);
    lv_label_set_text(s_lbl_state, "ready");
    lv_obj_set_pos(s_lbl_state, 36, 430);
    if (!s_debug_visible)
    {
        lv_obj_add_flag(s_lbl_state, LV_OBJ_FLAG_HIDDEN);
    }

    /* DIAG 调试档：缩小放右下角（正式界面不显眼但还在） */
    s_btn_diag = lv_button_create(root);
    lv_obj_set_size(s_btn_diag, 96, 26);
    lv_obj_set_pos(s_btn_diag, 688, 430);
    lv_obj_set_style_radius(s_btn_diag, 13, 0);
    lv_obj_set_style_bg_color(s_btn_diag, lv_color_hex(0xF2F4FA), 0);
    lv_obj_set_style_bg_opa(s_btn_diag, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_btn_diag, 0, 0);
    {
        lv_obj_t *l = lv_label_create(s_btn_diag);
        lv_obj_set_style_text_font(l, &ui_font_14, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_FAINT), 0);
        int di = audio_player_diag_get();
        lv_label_set_text(l, k_diag_name[(di >= 0 && di < 8) ? di : 0]);
        lv_obj_center(l);
        s_lbl_diag = l;
    }
    lv_obj_add_event_cb(s_btn_diag, diag_cb, LV_EVENT_CLICKED, NULL);
    if (!s_debug_visible)
    {
        lv_obj_add_flag(s_btn_diag, LV_OBJ_FLAG_HIDDEN);
    }

    update_track_label();
}

void scene_audio(uint16_t *fb, int frame)
{
    (void)fb;
    static int s_last_state = -1;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_audio();
        /* ★ 必须按播放器真实状态刷一次：否则"播放中退回桌面再进来"，
         *   按钮会停在默认的 PLAY（用户 11-18 抓到的 bug） */
        refresh_state_ui();
        s_last_state = (int)audio_player_state();
    }

    /* 进度条：每 10 帧（≈200ms）刷一次，避免每帧都动 LVGL 对象 */
    static int prog_tick;
    if (++prog_tick >= 5)     /* 100ms 更新一次，进度条更连续 */
    {
        prog_tick = 0;
        uint32_t pos = audio_player_position_ms();
        uint32_t dur = audio_player_duration_ms();

        if (s_bar != NULL)
        {
            int v = (dur > 0) ? (int)((uint64_t)pos * 1000ull / dur) : 0;
            if (v > 1000) v = 1000;
            lv_bar_set_value(s_bar, v, LV_ANIM_OFF);
        }

        char tb[16];
        fmt_ms(tb, sizeof(tb), pos);
        if (s_lbl_pos != NULL)
        {
            lv_label_set_text(s_lbl_pos, tb);
        }
        fmt_ms(tb, sizeof(tb), dur);
        if (s_lbl_dur != NULL)
        {
            lv_label_set_text(s_lbl_dur, tb);
        }
    }

    /* 50Hz 轻量轮询：loading → playing → idle 的迁移自动刷新 UI（比较一个 int） */
    int st = (int)audio_player_state();
    if (st != s_last_state)
    {
        s_last_state = st;
        refresh_state_ui();
    }

    lvgl_port_task();
}
