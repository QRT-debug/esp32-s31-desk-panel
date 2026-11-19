/**
 * @file    scene_motion.c
 * @brief   画面 7：移动竖条（拖影/响应时间）
 *
 * ★ 逐帧驱动：每次 draw() 只画一帧，中间留给 manager 轮询触摸。
 *   之前把 60 帧全塞进一个 draw() 里，这 2 秒内点屏幕完全没反应。
 */
#include <stdio.h>

#include "ui_draw.h"
#include "ui_scenes.h"

#define MOTION_FRAMES 60
#define BAR_W         40

void scene_motion(uint16_t *fb, int frame)
{
    if (frame < 0)              frame = 0;
    if (frame >= MOTION_FRAMES) frame = MOTION_FRAMES - 1;

    ui_clear(fb, CLR_BLACK);

    /* 参考网格 */
    for (int x = 0; x < UI_W; x += 40) ui_rect(fb, x, 30, 1, UI_H - 30, RGB565(24, 24, 24));
    for (int y = 30; y < UI_H; y += 40) ui_rect(fb, 0, y, UI_W, 1, RGB565(24, 24, 24));

    /* 移动白条：40px 宽，前沿 4px 红 */
    int pos = (int)((long)frame * (UI_W - BAR_W) / (MOTION_FRAMES - 1));
    ui_rect(fb, pos, 30, BAR_W, UI_H - 30, CLR_WHITE);
    ui_rect(fb, pos + BAR_W - 4, 30, 4, UI_H - 30, CLR_RED);

    char buf[24];
    snprintf(buf, sizeof(buf), "MOTION %d/%d", frame + 1, MOTION_FRAMES);
    ui_badge(fb, 7, buf);
}
