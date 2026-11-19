/**
 * @file    touch.h
 * @brief   触摸抽象层（上层只依赖这里，不依赖具体 IC）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TOUCH_MAX_POINTS 5

typedef struct
{
    uint16_t x;
    uint16_t y;
} touch_point_t;

typedef struct
{
    bool          pressed;
    uint8_t       count;
    touch_point_t pt[TOUCH_MAX_POINTS];
} touch_state_t;

esp_err_t  touch_init(void);
esp_err_t  touch_read(touch_state_t *st);
const char *touch_id(void);

/* 诊断字符串："raw=0xXX n=N i2c_err=K"，供信息页显示 */
const char *touch_debug_str(void);
