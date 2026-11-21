/**
 * @file    net_gate.c
 * @brief   全局 HTTPS 串行闸门实现（互斥量，懒初始化）
 */
#include "net_gate.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mtx;

void net_gate_init(void)
{
    if (s_mtx == NULL)
    {
        s_mtx = xSemaphoreCreateMutex();
        configASSERT(s_mtx != NULL);
    }
}

bool net_gate_take(int timeout_ms)
{
    if (s_mtx == NULL)
    {
        net_gate_init();
    }
    return xSemaphoreTake(s_mtx, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

void net_gate_give(void)
{
    if (s_mtx != NULL)
    {
        xSemaphoreGive(s_mtx);
    }
}
