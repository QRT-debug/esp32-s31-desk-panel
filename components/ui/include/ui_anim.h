/**
 * @file    ui_anim.h
 * @brief   页面过渡动画（只依赖 LVGL，不碰 LCD 驱动，便于 PC 模拟器复用）
 */
#pragma once

#include "lvgl.h"

/** 切页滑入：整页容器从右侧 +160px 处滑回 0（180ms，ease-out） */
void ui_slide_in(lv_obj_t *root);
