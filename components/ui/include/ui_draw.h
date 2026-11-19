/**
 * @file    ui_draw.h
 * @brief   屏幕绘制原语（RGB565 帧缓冲直接操作）
 *
 * 所有函数都直接写帧缓冲（PSRAM），不经过 esp_lcd 的局部刷新接口——
 * 因为本屏无 GRAM，双缓冲必须整帧重绘再提交。
 */
#pragma once

#include <stdint.h>

#include "lcd.h"

#define UI_W  LCD_H_RES
#define UI_H  LCD_V_RES

/* ---- 颜色 ---- */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define CLR_BLACK   0x0000
#define CLR_WHITE   0xFFFF
#define CLR_RED     0xF800
#define CLR_GREEN   0x07E0
#define CLR_BLUE    0x001F
#define CLR_CYAN    0x07FF
#define CLR_MAGENTA 0xF81F
#define CLR_YELLOW  0xFFE0
#define CLR_GRAY    0x8410

/* ---- 主题色（UI 模拟用） ---- */
#define UI_BG      RGB565(15, 17, 21)
#define UI_CARD    RGB565(26, 29, 36)
#define UI_LINE    RGB565(42, 46, 56)
#define UI_TXT     RGB565(240, 242, 245)
#define UI_DIM     RGB565(138, 143, 153)
#define UI_CYAN    RGB565(0, 212, 200)
#define UI_PURPLE  RGB565(123, 97, 255)
#define UI_AMBER   RGB565(255, 193, 7)

/* ---- 帧缓冲 ---- */
uint16_t *ui_fb_begin(void);   /* 取当前可写的那块 fb */
void      ui_fb_commit(void);  /* 提交并等待 vsync，然后切换到另一块 */

/* ---- 原语 ---- */
void ui_clear(uint16_t *fb, uint16_t c);
void ui_px(uint16_t *fb, int x, int y, uint16_t c);
void ui_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void ui_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t c);
void ui_circle(uint16_t *fb, int cx, int cy, int r, uint16_t c);
void ui_tri_right(uint16_t *fb, int x, int y, int w, int h, uint16_t c);
void ui_tri_left(uint16_t *fb, int x, int y, int w, int h, uint16_t c);

/* ASCII 点阵文字（size 只支持 16 / 24） */
void ui_text(uint16_t *fb, int x, int y, const char *s, int size, uint16_t fg, uint16_t bg);

/* 左上角编号条：n/total 名称 */
void ui_badge(uint16_t *fb, int idx, const char *name);

/* 两色线性插值（t 从 0 到 tmax） */
uint16_t ui_lerp(uint16_t a, uint16_t b, int t, int tmax);

/* ---- 实测帧率（启动时测一次，供信息页显示） ---- */
float ui_measured_fps(void);
void  ui_set_measured_fps(float v);

/* ---- 最后一次触摸坐标（manager 更新，信息页显示） ---- */
extern volatile int g_touch_last_x;
extern volatile int g_touch_last_y;
