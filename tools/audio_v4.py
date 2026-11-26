#!/usr/bin/env python3
"""音频页 v4：大封面 + 带缩略图的列表 + 底部迷你播放条 + 荧光强调色。"""
P = 'components/ui/src/scene_audio.c'
src = open(P, encoding='utf-8').read()

i = src.index('static void build_audio(void)')
j = src.index('void scene_audio(uint16_t *fb, int frame)')

body = r'''/* ---------------- 歌单页 v4（2026-11-26） ----------------
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

'''
src = src[:i] + body + src[j:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('ok, lines =', src.count('\n'))
