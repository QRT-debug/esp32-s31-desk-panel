/**
 * @file    scene_ui.c
 * @brief   画面 9：静态 UI 模拟（深色音乐播放器）
 *
 * 用途：看真实界面观感 —— 文字清晰度、色块、渐变、圆角、图标、小尺寸图形。
 * 说明：纯静态；ASCII 字库，无中文；点阵字无抗锯齿。
 */
#include <math.h>

#include "ui_draw.h"
#include "ui_scenes.h"

/* ---------------- 图标 ---------------- */

static void icon_wifi(uint16_t *fb, int cx, int cy, uint16_t c)
{
    for (int r = 7; r <= 19; r += 6)
    {
        for (int a = 218; a <= 322; a++)
        {
            float rad = (float)a * 3.14159265f / 180.0f;
            int x = cx + (int)(cosf(rad) * (float)r);
            int y = cy + (int)(sinf(rad) * (float)r);
            ui_px(fb, x, y, c);
            ui_px(fb, x + 1, y, c);
        }
    }
    ui_circle(fb, cx, cy + 6, 2, c);
}

static void icon_battery(uint16_t *fb, int x, int y, int level)
{
    ui_round_rect(fb, x, y, 32, 18, 4, UI_DIM);
    ui_round_rect(fb, x + 2, y + 2, 28, 14, 3, UI_BG);
    int w = 28 * level / 100;
    if (w > 3) ui_round_rect(fb, x + 2, y + 2, w, 14, 3, UI_CYAN);
    ui_rect(fb, x + 33, y + 6, 3, 6, UI_DIM);
}

/* ---------------- 频谱柱 ---------------- */

static void draw_spectrum(uint16_t *fb, int x, int y, int w, int h)
{
    const int N = 32;
    int bw  = 10;
    int gap = (w - N * bw) / (N - 1);

    for (int i = 0; i < N; i++)
    {
        float t    = (float)i / (float)(N - 1);
        float base = expf(-t * 2.4f);
        float wig  = 0.55f + 0.45f * sinf(t * 21.0f) * cosf(t * 7.0f);
        int   bh   = (int)((0.12f + 0.85f * base * wig) * (float)h);
        if (bh < 4) bh = 4;
        if (bh > h) bh = h;

        int bx = x + i * (bw + gap);
        for (int j = 0; j < bh; j++)
        {
            uint16_t c = ui_lerp(UI_PURPLE, UI_CYAN, j, bh - 1);
            for (int k = 0; k < bw; k++)
            {
                ui_px(fb, bx + k, y + h - 1 - j, c);
            }
        }
        ui_rect(fb, bx, y + h - bh, bw, 2, UI_AMBER);
    }
}

/* ---------------- 主画面 ---------------- */

void scene_ui(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, UI_BG);

    /* 顶部状态栏 */
    ui_rect(fb, 0, 0, UI_W, 44, UI_CARD);
    ui_rect(fb, 0, 44, UI_W, 1, UI_LINE);

    ui_text(fb, 16, 12, "22:53", 24, UI_TXT, UI_CARD);
    ui_text(fb, 300, 16, "ESP32-S31  800x480 RGB", 16, UI_DIM, UI_CARD);

    icon_wifi(fb, 668, 28, UI_CYAN);
    icon_battery(fb, 720, 13, 78);

    /* 专辑封面：圆角 + 对角渐变 */
    int cx = 24, cy = 64, cs = 232, cr = 18;

    for (int j = 0; j < cs; j++)
    {
        for (int i = 0; i < cs; i++)
        {
            int t = (i + j) * 100 / (2 * cs);
            ui_px(fb, cx + i, cy + j, ui_lerp(UI_PURPLE, UI_CYAN, t, 100));
        }
    }
    for (int j = 0; j < cr; j++)
    {
        for (int i = 0; i < cr; i++)
        {
            int dx = cr - 1 - i, dy = cr - 1 - j;
            if (dx * dx + dy * dy > cr * cr)
            {
                ui_px(fb, cx + i, cy + j, UI_BG);
                ui_px(fb, cx + cs - 1 - i, cy + j, UI_BG);
                ui_px(fb, cx + i, cy + cs - 1 - j, UI_BG);
                ui_px(fb, cx + cs - 1 - i, cy + cs - 1 - j, UI_BG);
            }
        }
    }
    ui_circle(fb, cx + cs / 2, cy + cs / 2, 52, UI_BG);
    ui_circle(fb, cx + cs / 2, cy + cs / 2, 48, CLR_WHITE);
    ui_circle(fb, cx + cs / 2, cy + cs / 2, 22, UI_BG);

    /* 右侧信息 */
    int rx = 288;
    ui_text(fb, rx, 72,  "Midnight City", 24, UI_TXT, UI_BG);
    ui_text(fb, rx, 112, "M83  /  Hurry Up, We're Dreaming", 16, UI_DIM, UI_BG);

    /* 进度条 */
    int pbx = rx, pby = 152, pbw = 490, pbh = 8;
    ui_round_rect(fb, pbx, pby, pbw, pbh, 4, UI_LINE);
    ui_round_rect(fb, pbx, pby, pbw * 37 / 100, pbh, 4, UI_CYAN);
    ui_circle(fb, pbx + pbw * 37 / 100, pby + pbh / 2, 7, CLR_WHITE);

    ui_text(fb, pbx, pby + 16, "01:23", 16, UI_DIM, UI_BG);
    ui_text(fb, pbx + pbw - 44, pby + 16, "03:45", 16, UI_DIM, UI_BG);

    /* 频谱 */
    ui_text(fb, rx, 204, "SPECTRUM   32 BANDS", 16, UI_DIM, UI_BG);
    draw_spectrum(fb, rx, 228, 490, 118);

    /* 底部控制栏 */
    ui_rect(fb, 0, 372, UI_W, 1, UI_LINE);

    ui_circle(fb, 326, 426, 34, UI_CARD);
    ui_rect(fb, 306, 412, 4, 28, UI_TXT);
    ui_tri_left(fb, 314, 412, 16, 28, UI_TXT);

    ui_circle(fb, 420, 426, 44, UI_CYAN);
    ui_tri_right(fb, 408, 406, 24, 36, UI_BG);

    ui_circle(fb, 514, 426, 34, UI_CARD);
    ui_tri_right(fb, 500, 412, 16, 28, UI_TXT);
    ui_rect(fb, 518, 412, 4, 28, UI_TXT);

    ui_text(fb, 588, 418, "VOL", 16, UI_DIM, UI_BG);
    ui_round_rect(fb, 634, 422, 142, 8, 4, UI_LINE);
    ui_round_rect(fb, 634, 422, 100, 8, 4, UI_PURPLE);
}
