/**
 * @file    lv_conf.h  (PC 模拟器专用)
 * @brief   只覆盖与板子对齐的关键项，其余走 LVGL 的默认值
 *
 * 与板子保持一致：RGB565、LVGL 9.x、无 OS（单线程）。
 */
#pragma once

/* 颜色深度：与板子相同（RGB565） */
#define LV_COLOR_DEPTH 16

/* 单线程（PC 上用主循环驱动 lv_timer_handler） */
#define LV_USE_OS LV_OS_NONE

/* 内存：PC 上给足，避免长字符串/多页面切换时不够 */
#define LV_MEM_SIZE (8 * 1024 * 1024)

/* 字体：界面用到的都开 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* 日志：调试时可改成 1 */
#define LV_USE_LOG 0

/* 性能面板：右上角会挡住状态栏，关掉 */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* LVGL 官方音乐播放器 demo（作为正式播放器界面的参考） */
#define LV_USE_DEMO_MUSIC 1
#define LV_DEMO_MUSIC_SQUARE 0
#define LV_DEMO_MUSIC_LANDSCAPE 1
#define LV_DEMO_MUSIC_ROUND 0
#define LV_DEMO_MUSIC_LARGE 0
#define LV_DEMO_MUSIC_AUTO_PLAY 0
