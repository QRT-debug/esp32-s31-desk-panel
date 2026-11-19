/**
 * @file    scene_info.c
 * @brief   画面 8：诊断信息页
 */
#include <stdio.h>

#include "esp_heap_caps.h"
#include "touch.h"
#include "ui_draw.h"
#include "ui_scenes.h"

/* 由 ui_draw.c 定义 */
extern volatile int g_touch_last_x;
extern volatile int g_touch_last_y;
extern void *lcd_buffer[2];

void scene_info(uint16_t *fb, int frame)
{
    (void)frame;
    ui_clear(fb, CLR_BLACK);

    float total_px = (float)(UI_W + LCD_HSYNC_WIDTH + LCD_HBP + LCD_HFP) *
                     (float)(UI_H + LCD_VSYNC_WIDTH + LCD_VBP + LCD_VFP);
    float fps_theory = (float)LCD_PIXEL_CLOCK_HZ / total_px;

    char l1[64], l2[64], l3[64], l4[64], l5[64], l6[64], l7[64];

    snprintf(l1, sizeof(l1), "PANEL  %dx%d  RGB565 16bit", UI_W, UI_H);
    snprintf(l2, sizeof(l2), "PCLK   %d MHz", LCD_PCLK_MHZ);
    snprintf(l3, sizeof(l3), "FPS    theory %.1f  measured %.1f", fps_theory, ui_measured_fps());
    snprintf(l4, sizeof(l4), "TIMING H:%d/%d/%d  V:%d/%d/%d",
             LCD_HSYNC_WIDTH, LCD_HBP, LCD_HFP, LCD_VSYNC_WIDTH, LCD_VBP, LCD_VFP);
    snprintf(l5, sizeof(l5), "PSRAM  total %u KB  free %u KB",
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    snprintf(l6, sizeof(l6), "FB     %p / %p", lcd_buffer[0], lcd_buffer[1]);
    snprintf(l7, sizeof(l7), "TOUCH  last = %d , %d", g_touch_last_x, g_touch_last_y);

    char l8[80];
    snprintf(l8, sizeof(l8), "DIAG   %s", touch_debug_str());

    int y = 74;
    ui_text(fb, 30, y, l1, 24, CLR_WHITE, CLR_BLACK);   y += 42;
    ui_text(fb, 30, y, l2, 24, CLR_CYAN,  CLR_BLACK);   y += 34;
    ui_text(fb, 30, y, l3, 24, CLR_YELLOW, CLR_BLACK);  y += 40;
    ui_text(fb, 30, y, l4, 16, CLR_GRAY,   CLR_BLACK);  y += 28;
    ui_text(fb, 30, y, l5, 16, CLR_GRAY,   CLR_BLACK);  y += 24;
    ui_text(fb, 30, y, l6, 16, CLR_GRAY,   CLR_BLACK);  y += 24;
    ui_text(fb, 30, y, l7, 16, CLR_GREEN,  CLR_BLACK);  y += 24;
    ui_text(fb, 30, y, l8, 16, CLR_MAGENTA, CLR_BLACK);  y += 36;

    ui_text(fb, 30, y, "Tap left / right half to switch scene", 16, CLR_CYAN, CLR_BLACK);
}
