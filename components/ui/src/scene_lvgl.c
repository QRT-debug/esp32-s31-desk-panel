/**
 * @file    scene_lvgl.c
 * @brief   画面 10：LVGL 官方 benchmark
 *
 * 这个画面的价值不在"好看"，而在于给出两组硬数据：
 *   - 每种控件组合的渲染耗时（CPU time）
 *   - 整体 fps（受屏的 48.5 fps 上限约束）
 * V3 要不要用 LVGL、要不要上 PPA 加速，就看这里的数。
 */
#include "lvgl_port.h"
#include "ui_scenes.h"

void scene_lvgl(uint16_t *fb, int frame)
{
    (void)fb;       /* LVGL 直接渲染进帧缓冲，不使用 manager 传入的指针 */

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        lvgl_port_start_benchmark();    /* 首次进入：重建 benchmark 界面 */
    }

    lvgl_port_task();                   /* 驱动 LVGL：定时器 + 渲染 + flush */
}
