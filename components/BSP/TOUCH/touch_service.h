/**
 * @file    touch_service.h
 * @brief   触摸服务：独立任务扫描 + 事件队列
 *
 * 为什么要独立任务：
 *   如果由 UI 主循环自己轮询，任何"一次刷很多帧"的画面（比如 60 帧动画）
 *   都会把触摸采集堵死，表现为"在这个页面上点屏幕没反应"。
 *   放在独立任务里，渲染再慢也不丢触摸事件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    TOUCH_EV_DOWN = 0,   /* 手指按下（上升沿） */
    TOUCH_EV_UP,         /* 手指抬起（下降沿） */
} touch_evt_type_t;

typedef struct
{
    touch_evt_type_t type;
    uint16_t         x;
    uint16_t         y;
} touch_evt_t;

/** 启动扫描任务（内部会调用 touch_init，失败则返回错误） */
esp_err_t touch_service_start(void);

/** 非阻塞取一个事件；取到返回 true */
bool touch_get_event(touch_evt_t *evt);

/** 取最近一次的坐标与按压状态（用于状态页显示） */
void touch_get_last(uint16_t *x, uint16_t *y, bool *pressed);
