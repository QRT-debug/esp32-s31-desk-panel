/**
 * @file    ui_fonts.h
 * @brief   界面字体（Segoe UI，由 lv_font_conv 生成）
 *
 * 为什么不用 LVGL 自带的 Montserrat：
 *   Montserrat 的 G 是开口设计（没有中间那道横），用户反馈看着别扭。
 *   Segoe UI 的 G 带横杠，更接近日常所见。
 *
 * 字模只含 ASCII 0x20~0x7E，4bpp 抗锯齿，四个字号合计约 246 KB Flash。
 * 重新生成：
 *   npx lv_font_conv --font C:/Windows/Fonts/segoeui.ttf --size <N> --bpp 4 \
 *       --format lvgl --range 0x20-0x7E --no-compress \
 *       --lv-include lvgl.h --lv-font-name ui_font_<N> -o ui_font_<N>.c
 */
#pragma once

#include "lvgl.h"

extern const lv_font_t ui_font_14;
extern const lv_font_t ui_font_20;
extern const lv_font_t ui_font_24;
extern const lv_font_t ui_font_28;
extern const lv_font_t ui_font_32;

/* 屏保大钟：96px 等宽数字（Consolas，只含 0-9 和 ':'，约 84KB flash）。
 * ★ 必须等宽：比例字体在秒数变化时整串宽度会跳，居中位置跟着抖。
 *   生成：npx lv_font_conv --font C:/Windows/Fonts/consola.ttf --size 96 --bpp 4 \
 *         --format lvgl --range 0x30-0x3A --no-compress \
 *         --lv-include lvgl.h --lv-font-name ui_font_digit_96 -o ui_font_digit_96.c */
extern const lv_font_t ui_font_digit_96;
extern const lv_font_t ui_font_digit_64;
extern const lv_font_t ui_font_digit_48;   /* 48px Consolas 等宽：时钟页（64 仍嫌大） */   /* 64px Consolas 等宽：时钟页（96px 压人物） */

/* FontAwesome 图标字体（24px，只含 7 个硬件图标字形，16KB）：
 *   f2db 芯片(CPU)  f538 内存条  f108 显示器(GPU)  f0a0 硬盘
 *   f6ff 网线       f1eb WiFi    f2c9 温度计
 * 用法：lv_label_set_text(lbl, "\xEF\x8B\x9B")  // 芯片 */
extern const lv_font_t ui_font_hw;
extern const lv_font_t ui_font_cn18;   /* 黑体 18 + ASCII，fallback 链 head */
extern const lv_font_t ui_font_cn18b;  /* GB2312 一级字库 2/4 块（~939 字） */
extern const lv_font_t ui_font_cn18c;  /* 3/4 块 */
extern const lv_font_t ui_font_cn18d;  /* 4/4 块 —— 合计覆盖 3755 常用汉字，榜单换名也不会缺字 */
