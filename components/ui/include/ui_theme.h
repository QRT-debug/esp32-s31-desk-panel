/**
 * @file    ui_theme.h
 * @brief   UI 主题层：浅色/深色两套色板，运行时切换（NVS 持久化）
 *
 * 设计（2026-11-20 01:35）：
 *   - 各 scene 原有的 #define C_XXX 0x... 全部改指向 ui_theme() 的字段，
 *     129 处调用点零改动；
 *   - 切换 = ui_theme_set() + ui_goto_scene(自己) 重建（frame==0 会 lv_obj_clean
 *     重画）+ ui_statusbar_refresh()；
 *   - 品牌色（紫/蓝/橙/绿/红/黄）两套同值，不进主题表。
 *
 * 为什么加深色：IPS 白底上手指油脂/滑动痕迹像噪点，深色底不显指纹。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint32_t bg_top, bg_bot;     /* 屏幕底色（system/monitor 单色用 bg_top） */
    uint32_t card;               /* 卡片底 */
    uint32_t card_line;          /* 卡片/按钮描边（浅色 0xD5DAE8 系） */
    uint32_t title;              /* 主标题字 */
    uint32_t sub;                /* 次要文字 */
    uint32_t faint;              /* 弱文字 */
    uint32_t shadow;             /* 卡片阴影色 */
    uint32_t accent;             /* 主题紫 */
    uint32_t accent_l, accent_l2;/* 封面底浅紫（深色用深紫底） */
    uint32_t track;              /* 进度条/电平条轨迹底 */
    uint32_t panel_dark;         /* 深色面板底（audio 曲目列表按钮，恒深） */
    uint32_t panel_line;         /* 深色面板描边 */
    uint32_t panel_text;         /* 深色面板上的文字（恒浅） */
    uint32_t panel_dim;          /* 深色面板上的弱字 */
    uint32_t sb_capsule;         /* 状态栏胶囊底 */
    uint32_t sb_text;            /* 胶囊内文字 */
    uint32_t sb_off;             /* 状态栏灰图标（未连接/关闭色） */
} ui_theme_t;

/* 当前主题（全局指针，切换后所有 ui_theme() 调用立即拿到新色板） */
const ui_theme_t *ui_theme(void);
bool ui_theme_is_dark(void);

/* 切换主题并写入 NVS（不重建界面——重建由调用方 ui_goto_scene 触发） */
void ui_theme_set(bool dark);

/* 开机读 NVS 恢复上次选择（main.c 在 ui_manager_run 前调一次） */
void ui_theme_init(void);
