/** freertos/FreeRTOS.h (PC 模拟器 stub) */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned int UBaseType_t;

#define pdPASS   1
#define pdFAIL   0
#define pdTRUE   1
#define pdFALSE  0

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY     0xFFFFFFFFu

/* 临界区：PC 单线程，空实现 */
#define portMUX_TYPE            int
#define portMUX_INITIALIZER_UNLOCKED  0
#define portENTER_CRITICAL(mux)  ((void)(mux))
#define portEXIT_CRITICAL(mux)   ((void)(mux))
