/** freertos/task.h (PC 模拟器 stub) */
#pragma once

#include "freertos/FreeRTOS.h"

void     vTaskDelay(TickType_t ticks);
uint32_t xTaskGetTickCount(void);
UBaseType_t uxTaskGetNumberOfTasks(void);
