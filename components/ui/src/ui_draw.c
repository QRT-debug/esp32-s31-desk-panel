/**
 * @file    ui_draw.c
 * @brief   绘制原语实现（直接操作 PSRAM 帧缓冲）
 */
#include "ui_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui_draw";

/* lcd.c 中的双缓冲游标 */
extern void    *lcd_buffer[2];
extern uint8_t  buffer_sw;
extern uint8_t  refresh_done_flag;

/* lcdfont.h 中的 ASCII 字模 */
extern const unsigned char asc2_1608[95][16];
extern const unsigned char asc2_2412[95][36];

static float s_fps = 0.0f;

volatile int g_touch_last_x = -1;
volatile int g_touch_last_y = -1;

/* ---------------- 帧缓冲 ---------------- */

uint16_t *ui_fb_begin(void)
{
    return (uint16_t *)lcd_buffer[buffer_sw];
}

void ui_fb_commit(void)
{
    esp_lcd_panel_draw_bitmap(lcddev.lcd_panel_handle, 0, 0, UI_W, UI_H, lcd_buffer[0]);

    refresh_done_flag = 0;
    int64_t t0 = esp_timer_get_time();

    while (!refresh_done_flag)
    {
        vTaskDelay(1);
        __sync_synchronize();
        if (esp_timer_get_time() - t0 > 200000)      /* 200 ms 超时保护 */
        {
            ESP_LOGW(TAG, "vsync timeout");
            break;
        }
    }
    /* 单缓冲：不切换（num_fbs = 1，lcd_buffer[1] 不存在） */
}

/* ---------------- 颜色 ---------------- */

uint16_t ui_lerp(uint16_t a, uint16_t b, int t, int tmax)
{
    int r1 = (a >> 11) & 0x1F, g1 = (a >> 5) & 0x3F, b1 = a & 0x1F;
    int r2 = (b >> 11) & 0x1F, g2 = (b >> 5) & 0x3F, b2 = b & 0x1F;

    if (tmax <= 0) tmax = 1;
    if (t < 0) t = 0;
    if (t > tmax) t = tmax;

    int r  = r1 + (r2 - r1) * t / tmax;
    int g  = g1 + (g2 - g1) * t / tmax;
    int bl = b1 + (b2 - b1) * t / tmax;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* ---------------- 原语 ---------------- */

void ui_px(uint16_t *fb, int x, int y, uint16_t c)
{
    if ((unsigned)x < (unsigned)UI_W && (unsigned)y < (unsigned)UI_H)
    {
        fb[(size_t)y * UI_W + x] = c;
    }
}

void ui_clear(uint16_t *fb, uint16_t c)
{
    uint32_t v = ((uint32_t)c << 16) | c;
    uint32_t *p = (uint32_t *)fb;
    for (size_t i = 0; i < (size_t)UI_W * UI_H / 2; i++)
    {
        p[i] = v;
    }
}

void ui_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++)
    {
        uint16_t *row = fb + (size_t)(y + j) * UI_W + x;
        for (int i = 0; i < w; i++)
        {
            row[i] = c;
        }
    }
}

void ui_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;

    for (int j = 0; j < h; j++)
    {
        int inset = 0;
        if (j < r)
        {
            int dy = r - 1 - j;
            inset = r - (int)sqrtf((float)(r * r - dy * dy));
        }
        else if (j >= h - r)
        {
            int dy = j - (h - r);
            inset = r - (int)sqrtf((float)(r * r - dy * dy));
        }
        for (int i = inset; i < w - inset; i++)
        {
            ui_px(fb, x + i, y + j, c);
        }
    }
}

void ui_circle(uint16_t *fb, int cx, int cy, int r, uint16_t c)
{
    for (int j = -r; j <= r; j++)
    {
        int w = (int)sqrtf((float)(r * r - j * j));
        for (int i = -w; i <= w; i++)
        {
            ui_px(fb, cx + i, cy + j, c);
        }
    }
}

void ui_tri_right(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int i = 0; i < w; i++)
    {
        int hh = h * i / w;
        for (int j = -hh / 2; j <= hh / 2; j++)
        {
            ui_px(fb, x + i, y + j, c);
        }
    }
}

void ui_tri_left(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int i = 0; i < w; i++)
    {
        int hh = h * i / w;
        for (int j = -hh / 2; j <= hh / 2; j++)
        {
            ui_px(fb, x + w - 1 - i, y + j, c);
        }
    }
}

void ui_text(uint16_t *fb, int x, int y, const char *s, int size, uint16_t fg, uint16_t bg)
{
    const unsigned char *font;
    switch (size)
    {
        case 16: font = (const unsigned char *)asc2_1608; break;
        case 24: font = (const unsigned char *)asc2_2412; break;
        default: return;
    }

    int cw     = size / 2;
    int nbytes = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2);
    int cx     = x;

    for (; *s; s++, cx += cw)
    {
        unsigned char ch = (unsigned char)*s;
        if (ch < ' ' || ch > '~') ch = '?';

        const unsigned char *p = font + (size_t)(ch - ' ') * nbytes;
        int row = 0, col = 0;

        for (int t = 0; t < nbytes; t++)
        {
            unsigned char tmp = p[t];
            for (int b = 0; b < 8; b++)
            {
                ui_px(fb, cx + col, y + row, (tmp & 0x80) ? fg : bg);
                tmp <<= 1;
                if (++row == size)
                {
                    row = 0;
                    col++;
                    break;
                }
            }
        }
    }
}

/* 场景总数由 ui_manager.c 定义 */
extern const int g_scene_count;

void ui_badge(uint16_t *fb, int idx, const char *name)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d/%d  %s", idx, g_scene_count, name);
    ui_rect(fb, 0, 0, 430, 26, CLR_BLACK);
    ui_text(fb, 8, 5, buf, 16, CLR_WHITE, CLR_BLACK);
}

/* ---------------- 帧率 ---------------- */

float ui_measured_fps(void)
{
    return s_fps;
}

void ui_set_measured_fps(float v)
{
    s_fps = v;
}
