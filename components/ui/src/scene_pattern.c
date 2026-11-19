/**
 * @file    scene_pattern.c
 * @brief   画面 4~6：棋盘格 / 1px 竖条纹 / 对齐标记
 */
#include "ui_draw.h"
#include "ui_scenes.h"

/* ---------------- 4. 棋盘格 8/4/2/1 px ---------------- */

void scene_checker(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    static const int sizes[4] = { 8, 4, 2, 1 };
    int qw = UI_W / 2;
    int qh = (UI_H - 30) / 2;

    for (int q = 0; q < 4; q++)
    {
        int ox = (q % 2) * qw;
        int oy = 30 + (q / 2) * qh;
        int s  = sizes[q];

        for (int y = 0; y < qh; y++)
        {
            for (int x = 0; x < qw; x++)
            {
                int on = (((x / s) + (y / s)) & 1);
                ui_px(fb, ox + x, oy + y, on ? CLR_WHITE : CLR_BLACK);
            }
        }
        for (int x = 0; x < qw; x++) ui_px(fb, ox + x, oy, CLR_GRAY);
        for (int y = 0; y < qh; y++) ui_px(fb, ox, oy + y, CLR_GRAY);
    }
}

/* ---------------- 5. 1px 黑白竖条纹 ---------------- */

void scene_stripe(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    for (int x = 0; x < UI_W; x++)
    {
        uint16_t c = (x & 1) ? CLR_WHITE : CLR_BLACK;
        for (int y = 30; y < UI_H; y++)
        {
            ui_px(fb, x, y, c);
        }
    }
}

/* ---------------- 6. 对齐：边框 + 四角 L + 中心十字 + 斜线 ---------------- */

void scene_alignment(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    /* 1px 外框 */
    for (int x = 0; x < UI_W; x++)
    {
        ui_px(fb, x, 0, CLR_WHITE);
        ui_px(fb, x, UI_H - 1, CLR_WHITE);
    }
    for (int y = 0; y < UI_H; y++)
    {
        ui_px(fb, 0, y, CLR_WHITE);
        ui_px(fb, UI_W - 1, y, CLR_WHITE);
    }

    /* 四角 L 标（60px） */
    for (int i = 0; i < 60; i++)
    {
        for (int t = 0; t < 3; t++)
        {
            ui_px(fb, i, 3 + t, CLR_RED);
            ui_px(fb, 3 + t, i, CLR_RED);

            ui_px(fb, UI_W - 1 - i, 3 + t, CLR_GREEN);
            ui_px(fb, UI_W - 4 - t, i, CLR_GREEN);

            ui_px(fb, i, UI_H - 4 - t, CLR_BLUE);
            ui_px(fb, 3 + t, UI_H - 1 - i, CLR_BLUE);

            ui_px(fb, UI_W - 1 - i, UI_H - 4 - t, CLR_YELLOW);
            ui_px(fb, UI_W - 4 - t, UI_H - 1 - i, CLR_YELLOW);
        }
    }

    /* 中心十字 */
    for (int x = UI_W / 2 - 40; x <= UI_W / 2 + 40; x++) ui_px(fb, x, UI_H / 2, CLR_WHITE);
    for (int y = UI_H / 2 - 40; y <= UI_H / 2 + 40; y++) ui_px(fb, UI_W / 2, y, CLR_WHITE);

    /* 两条对角斜线 */
    for (int i = 0; i < UI_H; i++)
    {
        int x = i * (UI_W - 1) / (UI_H - 1);
        ui_px(fb, x, i, CLR_CYAN);
        ui_px(fb, UI_W - 1 - x, i, CLR_MAGENTA);
    }

    /* 每 100px 刻度 */
    for (int x = 100; x < UI_W; x += 100) ui_rect(fb, x, 20, 1, 12, CLR_WHITE);
    for (int y = 100; y < UI_H; y += 100) ui_rect(fb, 20, y, 12, 1, CLR_WHITE);

    ui_text(fb, UI_W / 2 + 50, UI_H / 2 - 20, "CENTER", 16, CLR_WHITE, CLR_BLACK);
}
