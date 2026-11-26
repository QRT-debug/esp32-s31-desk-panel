#!/usr/bin/env python3
"""音频页第 3 版：按用户给的参考图做「蓝色渐变 + 白色大字 + 半透明」风格。"""
P = 'components/ui/src/scene_audio.c'
src = open(P, encoding='utf-8').read()

i = src.index('static void build_audio(void)')
j = src.index('void scene_audio(uint16_t *fb, int frame)')

body = r'''/* ---------------- 歌单页 v3：蓝色渐变风格（2026-11-26） ----------------
 * 依据用户给的两张实机截图重做。和 v2(Spotify 黑底) 的区别：
 *   底色 = 深蓝→亮青 的纵向渐变（不是纯黑）
 *   文字 = 白色 / 半透明白，字号整体放大一档
 *   左栏 = 大封面 + 歌单名 + 句点分隔信息 + 「播放全部」半透明胶囊
 *   右栏 = 序号 + 歌名 + 右侧歌手（**没有缩略图**，参考图就是这样）
 */

#define M_TOP    0x1D4ED8      /* 渐变：深蓝 */
#define M_BOT    0x38BDF8      /*       亮青 */
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

static void build_audio(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_statusbar_show(true);             /* 参考图右上角有状态栏 */

    s_wav_n = sd_card_ready() ? sd_card_scan_wav(s_wav, 16) : 0;
    if (s_wav_idx >= s_wav_n)
    {
        s_wav_idx = 0;
    }

    /* 背景：深蓝 -> 亮青 纵向渐变 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(M_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(M_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
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
    {
        lv_obj_t *back = lv_button_create(root);
        lv_obj_set_size(back, 34, 34);
        lv_obj_set_pos(back, 14, 8);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, lv_color_hex(M_WHITE), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_obj_set_style_text_font(bl, &ui_font_20, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(M_WHITE), 0);
        lv_label_set_text(bl, "<");
        lv_obj_center(bl);
    }
    mk_lbl3(root, 58, 12, &ui_font_28, M_WHITE, LV_OPA_COVER, "歌单");

    /* ---- 左栏：封面 + 歌单信息 ---- */
    {
        lv_obj_t *cover = lv_obj_create(root);
        lv_obj_set_size(cover, 200, 200);
        lv_obj_set_pos(cover, 20, 56);
        lv_obj_set_style_radius(cover, 16, 0);
        lv_obj_set_style_bg_color(cover, lv_color_hex(0x6366F1), 0);
        lv_obj_set_style_bg_grad_color(cover, lv_color_hex(0x22D3EE), 0);
        lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cover, 0, 0);
        lv_obj_set_style_shadow_width(cover, 30, 0);
        lv_obj_set_style_shadow_opa(cover, LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(cover, lv_color_hex(0x0B2A6B), 0);
        lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ic = lv_label_create(cover);
        lv_obj_set_style_text_font(ic, &ui_font_hw48, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(M_WHITE), 0);
        lv_obj_set_style_text_opa(ic, LV_OPA_80, 0);
        lv_label_set_text(ic, "\xEF\x80\x81");     /* fa-music */
        lv_obj_center(ic);
    }
    mk_lbl3(root, 20, 268, &ui_font_28, M_WHITE, LV_OPA_COVER, "Local Songs");
    {
        char b[48];
        snprintf(b, sizeof(b), "%d 首  /  SD 卡", s_wav_n);
        mk_lbl3(root, 20, 302, &ui_font_14, M_SOFT, LV_OPA_90, b);
    }

    /* 「播放全部」半透明胶囊 */
    {
        lv_obj_t *pb = lv_button_create(root);
        lv_obj_set_size(pb, 150, 38);
        lv_obj_set_pos(pb, 20, 330);
        lv_obj_set_style_radius(pb, 19, 0);
        lv_obj_set_style_bg_color(pb, lv_color_hex(M_WHITE), 0);
        lv_obj_set_style_bg_opa(pb, LV_OPA_30, 0);
        lv_obj_set_style_border_width(pb, 0, 0);
        lv_obj_set_style_shadow_width(pb, 0, 0);
        lv_obj_add_event_cb(pb, play_all_cb, LV_EVENT_CLICKED, NULL);

        s_lbl_play = lv_label_create(pb);
        lv_obj_set_style_text_font(s_lbl_play, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_play, lv_color_hex(M_WHITE), 0);
        lv_label_set_text(s_lbl_play, "> 播放全部");
        lv_obj_center(s_lbl_play);
        s_btn_play = pb;
    }

    /* ---- 右栏：歌曲列表 ---- */
    s_pl_box = lv_obj_create(root);
    lv_obj_set_pos(s_pl_box, 252, 52);
    lv_obj_set_size(s_pl_box, 532, 412);
    lv_obj_set_style_bg_opa(s_pl_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pl_box, 0, 0);
    lv_obj_set_style_pad_all(s_pl_box, 0, 0);
    lv_obj_set_scrollbar_mode(s_pl_box, LV_SCROLLBAR_MODE_AUTO);

    if (s_wav_n <= 0)
    {
        mk_lbl3(s_pl_box, 16, 20, &ui_font_20, M_SOFT, LV_OPA_90, "SD 卡里没有 wav");
    }
    for (int i = 0; i < s_wav_n && i < 16; i++)
    {
        char title[64] = { 0 };
        char artist[40] = { 0 };
        split_name(s_wav[i], title, sizeof(title), artist, sizeof(artist));

        lv_obj_t *row = lv_obj_create(s_pl_box);
        lv_obj_set_size(row, 524, 44);
        lv_obj_set_pos(row, 0, i * 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, pl_row_cb, LV_EVENT_CLICKED, NULL);

        char nb[8];
        snprintf(nb, sizeof(nb), "%02d", i + 1);
        mk_lbl3(row, 4, 13, &ui_font_14, M_SOFT, LV_OPA_70, nb);

        lv_obj_t *t = lv_label_create(row);
        lv_obj_set_style_text_font(t, &ui_font_20, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(M_WHITE), 0);
        lv_obj_set_pos(t, 44, 10);
        lv_obj_set_width(t, 300);
        lv_obj_set_height(t, 26);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, title);

        if (artist[0] != '\0')
        {
            lv_obj_t *a = lv_label_create(row);
            lv_obj_set_style_text_font(a, &ui_font_14, 0);
            lv_obj_set_style_text_color(a, lv_color_hex(M_WHITE), 0);
            lv_obj_set_style_text_opa(a, LV_OPA_70, 0);
            lv_obj_set_width(a, 160);
            lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(a, 356, 14);
            lv_label_set_text(a, artist);
        }

        s_pl_row[i] = row;
    }
}

'''
src = src[:i] + body + src[j:]
open(P, 'w', encoding='utf-8', newline='\n').write(src)
print('ok, lines =', src.count('\n'))
