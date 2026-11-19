/**
 * @file    lvgl_port.h
 * @brief   LVGL 9.x 移植层（RGB 面板直接模式 + 触摸）
 *
 * 设计要点：
 *   - 显示：LVGL 直接渲染进 panel 的双帧缓冲（direct mode），零拷贝。
 *           flush_cb 只做"提交 + 等 vsync + 切下一块"，不搬运像素。
 *   - 触摸：indev 读的是 touch_service 维护的"最近坐标"，
 *           不消费事件队列 —— 事件队列留给 ui_manager 做场景切换。
 *   - 调度：单线程。由 ui_manager 的循环调用 lvgl_port_task()，
 *           不另起任务，也就没有加锁问题（LV_OS_NONE）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 初始化 LVGL（一次即可）。必须在 lcd_init() 之后调用。 */
esp_err_t lvgl_port_init(void);

/** 是否初始化成功 */
bool lvgl_port_ready(void);

/** 驱动 LVGL 定时器/渲染，需周期性调用（建议 5 ms 一次） */
void lvgl_port_task(void);

/** 启动官方 benchmark 界面（会清空当前 LVGL 对象树） */
void lvgl_port_start_benchmark(void);
