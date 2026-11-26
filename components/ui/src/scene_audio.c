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

LV_IMG_DECLARE(bg_glow);      /* components/ui/src/bg_glow.c，音频页径向背景 */

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

/* 音乐 App 深色配色（本页强制深色，浅色米白没有音乐软件质感） */
/* 列表缩略图的假封面配色（没有专辑图，用色块代替） */
#define A_PAGE   0x0A0C10
#define A_SIDE   0x0D1015
#define A_CARD   0x14171E
#define A_TRACK  0x1E232D
#define A_TXT    0xECEEF2
#define A_SUB    0x8A93A3
#define A_ACCENT 0x8B5CF6

static const uint32_t COVER_COLORS[6] = {
    0x5B8DEF, 0xE86A92, 0xF59E0B, 0x2DD4BF, 0x8B5CF6, 0x38BDF8,
};


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
static lv_obj_t *s_pl_mark[16];    /* 每行左侧的荧光竖条 */
static const char *current_short_name(void);   /* 前向声明 */

/* 正式版界面的控件（build 里创建，轮询里更新） */
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_artist;     /* 底栏副标题 */
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
    /* 胶囊按钮文字随状态切换（用 ASCII 竖线，避免图标字体缺字） */
    if (s_lbl_play != NULL)
    {
        lv_label_set_text(s_lbl_play, on ? "|| 暂停" : "> 播放全部");
    }
    char b[64];
    if (on && what != NULL)
    {
        snprintf(b, sizeof(b), "%s", what);
    }
    else
    {
        snprintf(b, sizeof(b), "No track");
    }
    lv_label_set_text(s_lbl_title, b);
    /* ★ 不再写 s_lbl_state：它现在是歌单计数行（"N songs / SD CARD"） */
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
        /* 当前曲：半透明白底 + 左侧荧光竖条 */
        bool cur = (i == s_wav_idx);
        lv_obj_set_style_bg_color(s_pl_row[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(s_pl_row[i], cur ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        if (s_pl_mark[i] != NULL)
        {
            lv_obj_set_style_bg_opa(s_pl_mark[i], cur ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
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
        lv_label_set_text(s_lbl_play, "|| 暂停");
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

/* ---------------- 音乐 App 布局（2026-11-26 重做） ----------------
 * 参考 Spotify / Apple Music 的横屏结构：
 *     ┌─────────┬──────────────────────────┐
 *     │ 左栏导航 │  歌单（大标题 + 行列表）   │
 *     ├─────────┴──────────────────────────┤
 *     │ 底部播放器栏（封面 / 控制 / 进度 / 音量）│
 *     └────────────────────────────────────┘
 * 深色底是刻意的：这一页要"像音乐软件"，浅色米白没有那个质感。
 * 尺寸：左栏 160、主区 640、底栏 70。
 */


static lv_obj_t *mk_lbl2(lv_obj_t *p, int x, int y, const lv_font_t *f,
                         uint32_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

/** 一首歌的"封面"：圆角色块 + 音符 */
static void mk_cover(lv_obj_t *p, int x, int y, int size, uint32_t col)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_size(c, size, size);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(col), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex(col & 0x7F7F7F), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_label_create(c);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(ic, LV_SYMBOL_AUDIO);
    lv_obj_center(ic);
}

/* ---------------- 歌单页 v3：蓝色渐变风格（2026-11-26） ----------------
 * 依据用户给的两张实机截图重做。和 v2(Spotify 黑底) 的区别：
 *   底色 = 深蓝→亮青 的纵向渐变（不是纯黑）
 *   文字 = 白色 / 半透明白，字号整体放大一档
 *   左栏 = 大封面 + 歌单名 + 句点分隔信息 + 「播放全部」半透明胶囊
 *   右栏 = 序号 + 歌名 + 右侧歌手（**没有缩略图**，参考图就是这样）
 */

#define M_TOP    0x1E3A8A      /* 渐变：深靛蓝（不是亮蓝，太亮会发荧光） */
#define M_BOT    0x1E40AF      /*       深蓝（与顶几乎同色，只留极轻层次） */
#define M_WHITE  0xFFFFFF
#define M_SOFT   0xD6E4FF      /* 次要文字（配合 text_opa 用） */

static lv_obj_t *mk_lbl3(lv_obj_t *p, int x, int y, const lv_font_t *f,
                         uint32_t col, lv_opa_t opa, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

/** 从 "歌手 - 歌名.wav" 里拆出歌名/歌手；没有分隔符就整段当歌名 */
static void split_name(const char *src, char *title, size_t tcap,
                       char *artist, size_t acap)
{
    char nm[72];
    snprintf(nm, sizeof(nm), "%s", src);
    char *dot = strrchr(nm, '.');
    if (dot != NULL)
    {
        *dot = '\0';                     /* 去 .wav */
    }
    const char *sep = strstr(nm, " - ");
    if (sep != NULL)
    {
        size_t n = (size_t)(sep - nm);
        if (n >= tcap) { n = tcap - 1; }
        memcpy(artist, nm, n);
        artist[n] = '\0';
        snprintf(title, tcap, "%s", sep + 3);
    }
    else
    {
        snprintf(title, tcap, "%s", nm);
        artist[0] = '\0';
    }
}

/** 播放全部：从第 0 首开始 */
static void play_all_cb(lv_event_t *e)
{
    (void)e;
    s_wav_idx = 0;
    start_current_track();
}

/* ---------------- 歌单页 v4（2026-11-26） ----------------
 * 看完 Dribbble 上 8 个音乐 App 设计后的结论 —— 它们共有的骨架：
 *   1. 大封面做视觉锚点
 *   2. **列表每行都有缩略图**（这是"音乐感"的主要来源）
 *   3. 迷你播放条常驻底部
 *   4. 一个高饱和强调色（标当前曲 / 播放键）
 * v3 缺了 2/3/4，所以看起来像文件列表而不是音乐 App。
 */

#define N_GLOW   0x0196D8     /* 背景光晕主色（贴图里已有，这里只用于装饰） */
#define N_LIME   0xCCFF00     /* 荧光青柠：当前曲标记 / 播放键 */
#define N_WHITE  0xFFFFFF
#define N_SOFT   0xC7D2FE
#define N_PANEL  0x101826


static lv_obj_t *mk_lbl4(lv_obj_t *p, int x, int y, const lv_font_t *f,
                         uint32_t col, lv_opa_t opa, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, txt);
    return l;
}

/** 方形"封面"色块：渐变 + 音符 */
static void mk_art(lv_obj_t *p, int x, int y, int size, uint32_t col, bool music_note)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_size(c, size, size);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(col), 0);
    lv_obj_set_style_bg_grad_color(c, lv_color_hex((col >> 1) & 0x7F7F7F), 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, size >= 100 ? 16 : 10, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (music_note)
    {
        lv_obj_t *ic = lv_label_create(c);
        lv_obj_set_style_text_font(ic, size >= 100 ? &ui_font_hw48 : &ui_font_14, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(N_WHITE), 0);
        lv_obj_set_style_text_opa(ic, LV_OPA_80, 0);
        lv_label_set_text(ic, "\xEF\x80\x81");
        lv_obj_center(ic);
    }
}

static void build_audio(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_statusbar_show(false);   /* 深色/蓝光背景：浅色状态栏压上去很跳 */

    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    /* 背景：近黑 + 径向光晕贴图 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020811), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *bg = lv_image_create(scr);
        lv_image_set_src(bg, &bg_glow);
        lv_image_set_scale(bg, 2048);
        lv_obj_center(bg);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;

    /* ---- 顶栏 ---- */
    {
        lv_obj_t *back = lv_button_create(root);
        lv_obj_set_size(back, 34, 34);
        lv_obj_set_pos(back, 14, 8);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(N_WHITE), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_obj_set_style_text_font(bl, &ui_font_20, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(N_WHITE), 0);
        lv_label_set_text(bl, "<");
        lv_obj_center(bl);
    }
    mk_lbl4(root, 58, 12, &ui_font_28, N_WHITE, LV_OPA_COVER, "Playlist");

    /* ---- 左栏：封面 + 信息 + 播放全部 ---- */
    mk_art(root, 16, 54, 190, 0x4F46E5, true);

    mk_lbl4(root, 16, 256, &ui_font_24, N_WHITE, LV_OPA_COVER, "Local Songs");
    {
        char b[48];
        snprintf(b, sizeof(b), "%d songs  /  SD CARD", s_wav_n);
        mk_lbl4(root, 16, 288, &ui_font_14, N_SOFT, LV_OPA_80, b);
    }

    {
        lv_obj_t *pb = lv_button_create(root);
        lv_obj_set_size(pb, 140, 38);
        lv_obj_set_pos(pb, 16, 312);
        lv_obj_set_style_radius(pb, 19, 0);
        lv_obj_set_style_bg_color(pb, lv_color_hex(N_LIME), 0);
        lv_obj_set_style_bg_opa(pb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pb, 0, 0);
        lv_obj_set_style_shadow_width(pb, 0, 0);
        lv_obj_add_event_cb(pb, play_all_cb, LV_EVENT_CLICKED, NULL);

        s_lbl_play = lv_label_create(pb);
        lv_obj_set_style_text_font(s_lbl_play, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(0x0A0C10), 0);
        lv_label_set_text(s_lbl_play, "> 播放全部");
        lv_obj_center(s_lbl_play);
        s_btn_play = pb;
    }

    /* ---- 右栏：带缩略图的列表 ---- */
    s_pl_box = lv_obj_create(root);
    lv_obj_set_pos(s_pl_box, 224, 52);
    lv_obj_set_size(s_pl_box, 566, 348);
    lv_obj_set_style_bg_opa(s_pl_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pl_box, 0, 0);
    lv_obj_set_style_pad_all(s_pl_box, 0, 0);
    lv_obj_set_scrollbar_mode(s_pl_box, LV_SCROLLBAR_MODE_AUTO);

    if (s_wav_n <= 0)
    {
        mk_lbl4(s_pl_box, 8, 16, &ui_font_20, N_SOFT, LV_OPA_80, "no wav on SD card");
    }
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        char title[64] = { 0 };
        char artist[40] = { 0 };
        split_name(s_wav[i], title, sizeof(title), artist, sizeof(artist));

        lv_obj_t *row = lv_obj_create(s_pl_box);
        lv_obj_set_size(row, 558, 54);
        lv_obj_set_pos(row, 0, i * 58);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, pl_row_cb, LV_EVENT_CLICKED, NULL);

        /* 当前曲标记：左侧荧光竖条 */
        lv_obj_t *mark = lv_obj_create(row);
        s_pl_mark[i] = mark;
        lv_obj_set_size(mark, 3, 28);
        lv_obj_set_pos(mark, 0, 13);
        lv_obj_set_style_bg_color(mark, lv_color_hex(N_LIME), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);   /* 由 pl_highlight 打开 */
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_radius(mark, 2, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        mk_art(row, 12, 6, 42, COVER_COLORS[i % 6], false);

        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_style_text_font(t, &ui_font_20, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(N_WHITE), 0);
        lv_obj_set_pos(t, 66, 6);
        lv_obj_set_width(t, 330);
        lv_obj_set_height(t, 26);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, title);

        if (artist[0] != '\0')
        {
            mk_lbl4(row, 66, 32, &ui_font_14, N_SOFT, LV_OPA_70, artist);
        }

        /* 右侧播放箭头 */
        mk_lbl4(row, 520, 16, &ui_font_20, N_WHITE, LV_OPA_40, ">");

        s_pl_row[i] = row;
    }

    /* ---- 底部：迷你播放条 ---- */
    {
        lv_obj_t *mb = lv_obj_create(root);
        lv_obj_set_size(mb, 800, 76);
        lv_obj_set_pos(mb, 0, 404);
        lv_obj_set_style_bg_color(mb, lv_color_hex(N_PANEL), 0);
        lv_obj_set_style_bg_opa(mb, LV_OPA_90, 0);
        lv_obj_set_style_border_width(mb, 0, 0);
        lv_obj_set_style_radius(mb, 0, 0);
        lv_obj_set_style_pad_all(mb, 0, 0);
        lv_obj_clear_flag(mb, LV_OBJ_FLAG_SCROLLABLE);

        /* 顶部荧光细线 */
        lv_obj_t *ln = lv_obj_create(mb);
        lv_obj_set_size(ln, 800, 2);
        lv_obj_set_pos(ln, 0, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(N_LIME), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_70, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_style_pad_all(ln, 0, 0);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        mk_art(mb, 14, 16, 44, 0x4F46E5, false);

        s_lbl_title  = mk_lbl4(mb, 70, 16, &ui_font_20, N_WHITE, LV_OPA_COVER, "No track");
        s_lbl_artist = mk_lbl4(mb, 70, 40, &ui_font_14, N_SOFT, LV_OPA_70, "SD CARD");

        /* 上一首 / 播放 / 下一首 */
        {
            lv_obj_t *bp = lv_button_create(mb);
            lv_obj_set_size(bp, 32, 32);
            lv_obj_set_pos(bp, 254, 20);
            lv_obj_set_style_radius(bp, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(bp, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bp, 0, 0);
            lv_obj_set_style_shadow_width(bp, 0, 0);
            lv_obj_add_event_cb(bp, prev_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(bp);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(N_SOFT), 0);
            lv_label_set_text(l, LV_SYMBOL_PREV);
            lv_obj_center(l);
        }
        {
            lv_obj_t *mb2 = lv_button_create(mb);
            lv_obj_set_size(mb2, 44, 44);
            lv_obj_set_pos(mb2, 294, 14);
            lv_obj_set_style_radius(mb2, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(mb2, lv_color_hex(N_LIME), 0);
            lv_obj_set_style_border_width(mb2, 0, 0);
            lv_obj_set_style_shadow_width(mb2, 0, 0);
            lv_obj_add_event_cb(mb2, play_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(mb2);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0x0A0C10), 0);
            lv_label_set_text(l, LV_SYMBOL_PLAY);
            lv_obj_center(l);
        }
        {
            lv_obj_t *bn = lv_button_create(mb);
            lv_obj_set_size(bn, 32, 32);
            lv_obj_set_pos(bn, 346, 20);
            lv_obj_set_style_radius(bn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(bn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bn, 0, 0);
            lv_obj_set_style_shadow_width(bn, 0, 0);
            lv_obj_add_event_cb(bn, next_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(bn);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(N_SOFT), 0);
            lv_label_set_text(l, LV_SYMBOL_NEXT);
            lv_obj_center(l);
        }

        /* 进度 */
        s_bar = lv_bar_create(mb);
        lv_obj_set_size(s_bar, 190, 4);
        lv_obj_set_pos(s_bar, 396, 40);
        lv_bar_set_range(s_bar, 0, 1000);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x233044), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(N_LIME), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);

        s_lbl_pos = mk_lbl4(mb, 396, 48, &ui_font_14, N_SOFT, LV_OPA_70, "0:00");
        s_lbl_dur = mk_lbl4(mb, 548, 48, &ui_font_14, N_SOFT, LV_OPA_70, "0:00");
        lv_obj_set_width(s_lbl_dur, 38);
        lv_obj_set_style_text_align(s_lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);

        /* 音量 */
        lv_obj_t *vsl = lv_slider_create(mb);
        lv_obj_set_size(vsl, 96, 4);
        lv_obj_set_pos(vsl, 640, 40);
        lv_slider_set_range(vsl, 0, 100);
        lv_slider_set_value(vsl, audio_player_get_volume(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(0x233044), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(vsl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(N_SOFT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(N_WHITE), LV_PART_KNOB);
        lv_obj_set_style_pad_all(vsl, 3, LV_PART_KNOB);
        lv_obj_set_style_radius(vsl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(vsl, 0, LV_PART_KNOB);
        lv_obj_add_event_cb(vsl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_lbl_vol = mk_lbl4(mb, 748, 48, &ui_font_14, N_SOFT, LV_OPA_70, "40");
        s_lbl_state = mk_lbl4(mb, 748, 16, &ui_font_14, N_SOFT, LV_OPA_0, "");
    }
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
