/**
 * @file    ui_wallpaper.h
 * @brief   壁纸背景（从 SD 卡随机挑一张 800×480 JPEG 解码成 LVGL 图像）
 *
 * 从 scene_clock.c 里提炼出来的公共版本 —— 时钟页和 APPS 桌面都要用。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

/**
 * @brief  把一张随机壁纸贴到 parent 的最底层
 * @param  parent  目标容器（通常是屏幕或整页 root）
 * @retval true=成功；false=没有可用壁纸，调用方应回退到自己的背景色
 *
 * ★ 必须在 LVGL 任务上下文调用。
 * ★ 解码是同步的（硬件 JPEG，约 150~250ms），会在这一帧内卡住渲染；
 *   页面切换本来就有一帧空档，可以接受。
 */
bool wallpaper_apply_bg(lv_obj_t *parent);

/**
 * @brief  贴一张**指定文件名**的壁纸（在 /sdcard/wallpaper/ 下）
 * @param  name  文件名，如 "desktop.jpg"
 * @retval true=成功；false=文件不存在或解码失败
 *
 * 用途：APPS 桌面要固定一张，不能被"随机挑"换掉；时钟页继续用随机版。
 */
bool wallpaper_apply_named(lv_obj_t *parent, const char *name);
