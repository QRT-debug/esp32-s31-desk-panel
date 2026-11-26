#!/usr/bin/env python3
"""音频页改成手机音乐 App 风格（横屏）：左栏库导航 + 主区歌单 + 底部播放器栏。"""
P = 'components/ui/src/scene_audio.c'
src = open(P, encoding='utf-8').read()

i = src.index('static void build_audio(void)')
j = src.index('void scene_audio(uint16_t *fb, int frame)')

body = r'''/* ---------------- 音乐 App 布局（2026-11-26 重做） ----------------
 * 参考 Spotify / Apple Music 的横屏结构：
 *     ┌─────────┬──────────────────────────┐
 *     │ 左栏导航 │  歌单（大标题 + 行列表）   │
 *     ├─────────┴──────────────────────────┤
 *     │ 底部播放器栏（封面 / 控制 / 进度 / 音量）│
 *     └────────────────────────────────────┘
 * 深色底是刻意的：这一页要"像音乐软件"，浅色米白没有那个质感。
 * 尺寸：左栏 160、主区 640、底栏 70。
 */

#define A_PAGE   0x0A0C10
#define A_SIDE   0x0D1015
#define A_CARD   0x14171E
#define A_TRACK  0x1E232D
#define A_TXT    0xECEEF2
#define A_SUB    0x8A93A3
#define A_ACCENT 0x8B5CF6

/* 列表缩略图的假封面配色（没有专辑图，用色块代替） */
static const uint32_t COVER_COLORS[6] = {
    0x5B8DEF, 0xE86A92, 0xF59E0B, 0x2DD4BF, 0x8B5CF6, 0x38BDF8,
};

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

static void build_audio(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    /* 扫描 SD 卡曲目 */
    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(A_PAGE), 0);
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

    /* ================= 左栏：库导航 ================= */
    lv_obj_t *side = lv_obj_create(root);
    lv_obj_set_size(side, 160, 410);
    lv_obj_set_pos(side, 0, 0);
    lv_obj_set_style_bg_color(side, lv_color_hex(A_SIDE), 0);
    lv_obj_set_style_bg_opa(side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(side, 0, 0);
    lv_obj_set_style_radius(side, 0, 0);
    lv_obj_set_style_pad_all(side, 0, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    mk_lbl2(side, 18, 16, &ui_font_14, A_SUB, "LIBRARY");

    /* 当前项：歌曲库（强调色 + 左侧竖条） */
    lv_obj_t *cur = lv_obj_create(side);
    lv_obj_set_size(cur, 160, 44);
    lv_obj_set_pos(cur, 0, 44);
    lv_obj_set_style_bg_color(cur, lv_color_hex(A_ACCENT), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_20, 0);
    lv_obj_set_style_border_width(cur, 0, 0);
    lv_obj_set_style_radius(cur, 0, 0);
    lv_obj_set_style_pad_all(cur, 0, 0);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_SCROLLABLE);
    mk_lbl2(cur, 18, 12, &ui_font_20, A_ACCENT, "Songs");

    mk_lbl2(side, 18, 104, &ui_font_20, A_SUB, "Liked");
    mk_lbl2(side, 18, 148, &ui_font_20, A_SUB, "Recent");

    /* 返回（左上角，与其他页一致） */
    lv_obj_t *back = lv_button_create(side);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 14, 360);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(A_CARD), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &ui_font_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(A_SUB), 0);
    lv_label_set_text(bl, "<");
    lv_obj_center(bl);

    /* ================= 主区：歌单 ================= */
    mk_lbl2(root, 180, 6, &ui_font_28, A_TXT, "Local Library");
    {
        char b[32];
        snprintf(b, sizeof(b), "%d songs  ·  SD CARD", s_wav_n);
        s_lbl_state = mk_lbl2(root, 180, 40, &ui_font_14, A_SUB, b);
    }

    s_pl_box = lv_obj_create(root);
    lv_obj_set_pos(s_pl_box, 168, 66);
    lv_obj_set_size(s_pl_box, 624, 336);
    lv_obj_set_style_bg_opa(s_pl_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pl_box, 0, 0);
    lv_obj_set_style_pad_all(s_pl_box, 0, 0);
    lv_obj_set_scrollbar_mode(s_pl_box, LV_SCROLLBAR_MODE_AUTO);

    if (s_wav_n <= 0)
    {
        mk_lbl2(s_pl_box, 12, 12, &ui_font_20, A_SUB, "no wav on SD card");
    }
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        lv_obj_t *row = lv_obj_create(s_pl_box);
        lv_obj_set_size(row, 616, 54);
        lv_obj_set_pos(row, 0, i * 58);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, pl_row_cb, LV_EVENT_CLICKED, NULL);

        char nb[8];
        snprintf(nb, sizeof(nb), "%d", i + 1);
        mk_lbl2(row, 10, 18, &ui_font_14, A_SUB, nb);

        mk_cover(row, 36, 7, 40, COVER_COLORS[i % 6]);

        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_style_text_font(t, &ui_font_20, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(A_TXT), 0);
        lv_obj_set_pos(t, 90, 8);
        lv_obj_set_width(t, 480);
        lv_obj_set_height(t, 26);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, s_wav[i]);

        mk_lbl2(row, 90, 34, &ui_font_14, A_SUB, "SD CARD");

        s_pl_row[i] = row;
    }

    /* ================= 底栏：播放器 ================= */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, 800, 70);
    lv_obj_set_pos(bar, 0, 410);
    lv_obj_set_style_bg_color(bar, lv_color_hex(A_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部一条强调色细线（Spotify 播放器栏的特征） */
    lv_obj_t *line = lv_obj_create(bar);
    lv_obj_set_size(line, 800, 2);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(A_ACCENT), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_60, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    mk_cover(bar, 16, 12, 46, COVER_COLORS[0]);

    s_lbl_title  = mk_lbl2(bar, 74, 12, &ui_font_20, A_TXT, "—");
    s_lbl_artist = mk_lbl2(bar, 74, 38, &ui_font_14, A_SUB, "SD CARD");

    /* 传输控制 */
    {
        lv_obj_t *bp = lv_button_create(bar);
        lv_obj_set_size(bp, 34, 34);
        lv_obj_set_pos(bp, 262, 18);
        lv_obj_set_style_radius(bp, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bp, 0, 0);
        lv_obj_set_style_shadow_width(bp, 0, 0);
        lv_obj_add_event_cb(bp, prev_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lp = lv_label_create(bp);
        lv_obj_set_style_text_font(lp, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lp, lv_color_hex(A_SUB), 0);
        lv_label_set_text(lp, LV_SYMBOL_PREV);
        lv_obj_center(lp);
    }
    {
        s_btn_play = lv_button_create(bar);
        lv_obj_set_size(s_btn_play, 46, 46);
        lv_obj_set_pos(s_btn_play, 306, 12);
        lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(A_TXT), 0);
        lv_obj_set_style_border_width(s_btn_play, 0, 0);
        lv_obj_set_style_shadow_width(s_btn_play, 0, 0);
        lv_obj_add_event_cb(s_btn_play, play_cb, LV_EVENT_CLICKED, NULL);
        s_lbl_play = lv_label_create(s_btn_play);
        lv_obj_set_style_text_font(s_lbl_play, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(A_PAGE), 0);
        lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
        lv_obj_center(s_lbl_play);
    }
    {
        lv_obj_t *bn = lv_button_create(bar);
        lv_obj_set_size(bn, 34, 34);
        lv_obj_set_pos(bn, 364, 18);
        lv_obj_set_style_radius(bn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bn, 0, 0);
        lv_obj_set_style_shadow_width(bn, 0, 0);
        lv_obj_add_event_cb(bn, next_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ln = lv_label_create(bn);
        lv_obj_set_style_text_font(ln, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ln, lv_color_hex(A_SUB), 0);
        lv_label_set_text(ln, LV_SYMBOL_NEXT);
        lv_obj_center(ln);
    }

    /* 进度 */
    s_bar = lv_bar_create(bar);
    lv_obj_set_size(s_bar, 220, 4);
    lv_obj_set_pos(s_bar, 414, 38);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(A_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(A_TXT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);

    s_lbl_pos = mk_lbl2(bar, 414, 46, &ui_font_14, A_SUB, "0:00");
    s_lbl_dur = mk_lbl2(bar, 596, 46, &ui_font_14, A_SUB, "0:00");
    lv_obj_set_width(s_lbl_dur, 38);
    lv_obj_set_style_text_align(s_lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);

    /* 音量 */
    lv_obj_t *vsl = lv_slider_create(bar);
    lv_obj_set_size(vsl, 90, 4);
    lv_obj_set_pos(vsl, 660, 38);
    lv_slider_set_range(vsl, 0, 100);
    lv_slider_set_value(vsl, audio_player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vsl, lv_color_hex(A_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vsl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vsl, lv_color_hex(A_SUB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vsl, lv_color_hex(A_TXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(vsl, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(vsl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(vsl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(vsl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_vol = mk_lbl2(bar, 760, 28, &ui_font_14, A_SUB, "40");
}

'''
src = src[:i] + body + src[j:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('rewritten, lines =', src.count('\n'))
