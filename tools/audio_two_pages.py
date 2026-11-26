#!/usr/bin/env python3
"""音频页改成两级：主播放页 + 二级歌单页（同风格，天空蓝）。"""
P = 'components/ui/src/scene_audio.c'
src = open(P, encoding='utf-8').read()

i = src.index('static void build_audio(void)')
j = src.index('void scene_audio(uint16_t *fb, int frame)')

body = r'''/* ---------------- 两级结构（2026-11-26） ----------------
 *   subpage = 0  播放页：大封面 + 进度 + 暂停/上下曲 + 音量（主页面）
 *   subpage = 1  歌单页：序号 + 歌名 + 歌手（点「歌单」进入，点歌曲回播放页）
 * ★ 之前把歌单做成唯一页面是错的 —— 那样没有暂停入口。
 */

#define P_BG     0x0284C7      /* 天空蓝（白字对比度 4.5:1） */
#define P_TXT    0xFFFFFF
#define P_SUB    0xD6E4FF
#define P_LINE   0x0B2B3C

static lv_obj_t *s_pg_box;         /* 歌单容器 */
static lv_obj_t *s_pg_row[16];
static lv_obj_t *s_pg_mark[16];
static lv_obj_t *s_nav_btn;        /* 右上角「歌单」按钮 */
static int       s_subpage = 0;

static lv_obj_t *mk_lbl5(lv_obj_t *p, int x, int y, const lv_font_t *f,
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

/** 方形封面（插画）带投影：lv_image 不能投影，套一层透明壳 */
static void mk_cover_box(lv_obj_t *p, int x, int y, int size, int radius)
{
    lv_obj_t *wrap = lv_obj_create(p);
    lv_obj_set_size(wrap, size, size);
    lv_obj_set_pos(wrap, x, y);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_radius(wrap, radius, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_shadow_width(wrap, 26, 0);
    lv_obj_set_style_shadow_opa(wrap, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(wrap, lv_color_hex(0x00243D), 0);
    lv_obj_set_style_shadow_ofs_y(wrap, 7, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_image_create(wrap);
    lv_image_set_src(img, &album_cover);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------- 顶部通栏：返回 + 标题 + 右侧切换 ---------- */
static void build_topbar(lv_obj_t *root, const char *title, const char *right_txt,
                         lv_event_cb_t right_cb)
{
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 14, 8);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_30, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &ui_font_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(P_TXT), 0);
    lv_label_set_text(bl, "<");
    lv_obj_center(bl);

    mk_lbl5(root, 58, 12, &ui_font_28, P_TXT, LV_OPA_COVER, title);

    if (right_txt != NULL)
    {
        lv_obj_t *rb = lv_button_create(root);
        lv_obj_set_size(rb, 96, 36);
        lv_obj_set_pos(rb, 688, 10);
        lv_obj_set_style_radius(rb, 18, 0);
        lv_obj_set_style_bg_color(rb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(rb, LV_OPA_20, 0);
        lv_obj_set_style_border_width(rb, 1, 0);
        lv_obj_set_style_border_color(rb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(rb, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(rb, 0, 0);
        lv_obj_add_event_cb(rb, right_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *rl = lv_label_create(rb);
        lv_obj_set_style_text_font(rl, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(P_TXT), 0);
        lv_label_set_text(rl, right_txt);
        lv_obj_center(rl);
        s_nav_btn = rb;
    }
}

/* ================= 主页面：播放 ================= */
static void build_player(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_statusbar_show(false);

    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(P_BG), 0);
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

    build_topbar(root, "音乐", "歌单 >", to_playlist_cb);

    /* 大封面 */
    mk_cover_box(root, 40, 72, 250, 20);

    /* 右侧信息 */
    s_lbl_title  = mk_lbl5(root, 320, 96, &ui_font_28, P_TXT, LV_OPA_COVER, "No track");
    {
        char b[48];
        snprintf(b, sizeof(b), "%d songs  /  SD CARD", s_wav_n);
        s_lbl_artist = mk_lbl5(root, 320, 136, &ui_font_14, P_SUB, LV_OPA_COVER, b);
    }

    /* 进度 */
    s_bar = lv_bar_create(root);
    lv_obj_set_size(s_bar, 380, 6);
    lv_obj_set_pos(s_bar, 320, 214);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x016DA5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(P_TXT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);

    s_lbl_pos = mk_lbl5(root, 320, 228, &ui_font_14, P_SUB, LV_OPA_COVER, "0:00");
    s_lbl_dur = mk_lbl5(root, 662, 228, &ui_font_14, P_SUB, LV_OPA_COVER, "0:00");
    lv_obj_set_width(s_lbl_dur, 38);
    lv_obj_set_style_text_align(s_lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);

    /* 传输控制：上一首 / 播放 / 下一首 */
    {
        lv_obj_t *bp = lv_button_create(root);
        lv_obj_set_size(bp, 48, 48);
        lv_obj_set_pos(bp, 344, 272);
        lv_obj_set_style_radius(bp, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bp, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(bp, LV_OPA_20, 0);
        lv_obj_set_style_border_width(bp, 0, 0);
        lv_obj_set_style_shadow_width(bp, 0, 0);
        lv_obj_add_event_cb(bp, prev_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(bp);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(P_TXT), 0);
        lv_label_set_text(l, LV_SYMBOL_PREV);
        lv_obj_center(l);
    }
    {
        s_btn_play = lv_button_create(root);
        lv_obj_set_size(s_btn_play, 72, 72);
        lv_obj_set_pos(s_btn_play, 424, 260);
        lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(P_TXT), 0);
        lv_obj_set_style_border_width(s_btn_play, 0, 0);
        lv_obj_set_style_shadow_width(s_btn_play, 18, 0);
        lv_obj_set_style_shadow_opa(s_btn_play, LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(s_btn_play, lv_color_hex(0x00243D), 0);
        lv_obj_add_event_cb(s_btn_play, play_cb, LV_EVENT_CLICKED, NULL);
        s_lbl_play = lv_label_create(s_btn_play);
        lv_obj_set_style_text_font(s_lbl_play, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(P_BG), 0);
        lv_label_set_text(s_lbl_play, LV_SYMBOL_PLAY);
        lv_obj_center(s_lbl_play);
    }
    {
        lv_obj_t *bn = lv_button_create(root);
        lv_obj_set_size(bn, 48, 48);
        lv_obj_set_pos(bn, 528, 272);
        lv_obj_set_style_radius(bn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bn, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(bn, LV_OPA_20, 0);
        lv_obj_set_style_border_width(bn, 0, 0);
        lv_obj_set_style_shadow_width(bn, 0, 0);
        lv_obj_add_event_cb(bn, next_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(bn);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(P_TXT), 0);
        lv_label_set_text(l, LV_SYMBOL_NEXT);
        lv_obj_center(l);
    }

    /* 音量 */
    mk_lbl5(root, 320, 372, &ui_font_14, P_SUB, LV_OPA_COVER, "音量");
    {
        lv_obj_t *vsl = lv_slider_create(root);
        lv_obj_set_size(vsl, 240, 6);
        lv_obj_set_pos(vsl, 372, 376);
        lv_slider_set_range(vsl, 0, 100);
        lv_slider_set_value(vsl, audio_player_get_volume(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(0x016DA5), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(vsl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(P_TXT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(vsl, lv_color_hex(P_TXT), LV_PART_KNOB);
        lv_obj_set_style_pad_all(vsl, 3, LV_PART_KNOB);
        lv_obj_set_style_radius(vsl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(vsl, 0, LV_PART_KNOB);
        lv_obj_add_event_cb(vsl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    s_lbl_vol = mk_lbl5(root, 626, 368, &ui_font_20, P_TXT, LV_OPA_COVER, "40");

    s_lbl_state = mk_lbl5(root, 320, 424, &ui_font_14, P_SUB, LV_OPA_0, "");

    pl_highlight();
}

/* ================= 二级页：歌单 ================= */
static void build_playlist(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_statusbar_show(false);

    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(P_BG), 0);
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

    build_topbar(root, "歌单", "< 返回播放", to_player_cb);

    mk_cover_box(root, 16, 60, 190, 26);
    mk_lbl5(root, 20, 268, &ui_font_28, P_TXT, LV_OPA_COVER, "Local Songs");
    {
        char b[48];
        snprintf(b, sizeof(b), "%d songs   /   SD CARD", s_wav_n);
        mk_lbl5(root, 20, 302, &ui_font_14, P_SUB, LV_OPA_COVER, b);
    }
    {
        lv_obj_t *pb = lv_button_create(root);
        lv_obj_set_size(pb, 160, 40);
        lv_obj_set_pos(pb, 16, 330);
        lv_obj_set_style_radius(pb, 20, 0);
        lv_obj_set_style_bg_color(pb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(pb, LV_OPA_20, 0);
        lv_obj_set_style_border_width(pb, 1, 0);
        lv_obj_set_style_border_color(pb, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(pb, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(pb, 0, 0);
        lv_obj_add_event_cb(pb, play_all_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(pb);
        lv_obj_set_style_text_font(l, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(P_TXT), 0);
        lv_label_set_text(l, "> 播放全部");
        lv_obj_center(l);
    }

    s_pg_box = lv_obj_create(root);
    lv_obj_set_pos(s_pg_box, 224, 56);
    lv_obj_set_size(s_pg_box, 566, 400);
    lv_obj_set_style_bg_opa(s_pg_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pg_box, 0, 0);
    lv_obj_set_style_pad_all(s_pg_box, 0, 0);
    lv_obj_set_scrollbar_mode(s_pg_box, LV_SCROLLBAR_MODE_AUTO);

    if (s_wav_n <= 0)
    {
        mk_lbl5(s_pg_box, 8, 12, &ui_font_20, P_SUB, LV_OPA_COVER, "no wav on SD card");
    }
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        char title[64] = { 0 };
        char artist[40] = { 0 };
        split_name(s_wav[i], title, sizeof(title), artist, sizeof(artist));

        lv_obj_t *row = lv_obj_create(s_pg_box);
        lv_obj_set_size(row, 558, 46);
        lv_obj_set_pos(row, 0, i * 50);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, pl_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *mark = lv_obj_create(row);
        lv_obj_set_size(mark, 3, 24);
        lv_obj_set_pos(mark, 0, 11);
        lv_obj_set_style_bg_color(mark, lv_color_hex(P_TXT), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_radius(mark, 2, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_pg_mark[i] = mark;

        char nb[8];
        snprintf(nb, sizeof(nb), "%02d", i + 1);
        mk_lbl5(row, 12, 13, &ui_font_14, P_SUB, LV_OPA_COVER, nb);

        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_style_text_font(t, &ui_font_20, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(P_TXT), 0);
        lv_obj_set_pos(t, 52, 9);
        lv_obj_set_width(t, 330);
        lv_obj_set_height(t, 26);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, title);

        if (artist[0] != '\0')
        {
            lv_obj_t *a = lv_label_create(row);
            lv_obj_set_style_text_font(a, &ui_font_14, 0);
            lv_obj_set_style_text_color(a, lv_color_hex(P_SUB), 0);
            lv_obj_set_width(a, 160);
            lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(a, 392, 14);
            lv_label_set_text(a, artist);
        }

        s_pg_row[i] = row;
    }

    pl_highlight();
}

/** 右下角切到歌单页 */
static void to_playlist_cb(lv_event_t *e)
{
    (void)e;
    s_subpage = 1;
    build_playlist();
    refresh_state_ui();
}

/** 切回播放页 */
static void to_player_cb(lv_event_t *e)
{
    (void)e;
    s_subpage = 0;
    build_player();
    refresh_state_ui();
}

'''
src = src[:i] + body + src[j:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('ok, lines =', src.count('\n'))
