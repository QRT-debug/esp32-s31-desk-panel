/**
 * @file    esp_log.h  (PC 模拟器 stub)
 * @brief   把 ESP_LOGx 映射到 printf（带颜色可选，这里从简）
 */
#pragma once

#include <stdarg.h>
#include <stdio.h>

/* ui_logbuf.c 用到的日志钩子接口（PC 上不做缓冲，直接返回 NULL） */
typedef int (*vprintf_like_t)(const char *, va_list);
static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t fn) { (void)fn; return NULL; }

#define ESP_LOGE(tag, fmt, ...) printf("[E %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_LOGV(tag, fmt, ...) ((void)0)
