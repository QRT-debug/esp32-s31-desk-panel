/**
 * @file    scene_basic.c
 * @brief   画面 1~3：纯色轮播 / 灰阶 / RGB 渐变
 */
#include "ui_draw.h"
#include "ui_scenes.h"

/* ---------------- 1. 纯色 ---------------- */

static const uint16_t k_solid_col[] = {
    CLR_WHITE, CLR_BLACK, CLR_RED, CLR_GREEN, CLR_BLUE, CLR_CYAN, CLR_MAGENTA, CLR_YELLOW
};
static const char *k_solid_name[] = {
    "WHITE", "BLACK", "RED", "GREEN", "BLUE", "CYAN", "MAGENTA", "YELLOW"
};

void scene_solid(uint16_t *fb, int frame)
{
    int i = frame & 7;          /* frame 由 manager 递增，8 色循环 */
    ui_clear(fb, k_solid_col[i]);
    ui_badge(fb, 1, k_solid_name[i]);
}

/* ---------------- 2. 灰阶 16 级 ---------------- */

void scene_grayscale(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    for (int i = 0; i < 16; i++)
    {
        int v = i * 255 / 15;
        ui_rect(fb, i * (UI_W / 16), 40, UI_W / 16 - 1, UI_H - 80, RGB565(v, v, v));
    }

    ui_text(fb, 8, UI_H - 30, "0", 16, CLR_WHITE, CLR_BLACK);
    ui_text(fb, UI_W - 48, UI_H - 30, "255", 16, CLR_WHITE, CLR_BLACK);
}

/* ---------------- 3. RGB 三原色渐变 ---------------- */

void scene_rgb_ramp(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    int band = (UI_H - 60) / 3;

    for (int x = 0; x < UI_W; x++)
    {
        int v = x * 255 / (UI_W - 1);
        for (int y = 0; y < band; y++)
        {
            ui_px(fb, x, 40 + y,            RGB565(v, 0, 0));
            ui_px(fb, x, 40 + band + y,     RGB565(0, v, 0));
            ui_px(fb, x, 40 + band * 2 + y, RGB565(0, 0, v));
        }
    }

    ui_text(fb, 8, 46,            "R", 24, CLR_WHITE, CLR_BLACK);
    ui_text(fb, 8, 46 + band,     "G", 24, CLR_WHITE, CLR_BLACK);
    ui_text(fb, 8, 46 + band * 2, "B", 24, CLR_WHITE, CLR_BLACK);
}
