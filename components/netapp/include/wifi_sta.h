/**
 * @file    wifi_sta.h
 * @brief   WiFi STA 连接（自动重连）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** 初始化并开始连接（非阻塞，连接结果用 wifi_sta_wait_connected 等） */
esp_err_t wifi_sta_init(void);

/** 当前是否已拿到 IP */
bool wifi_sta_connected(void);

/** 阻塞等待连接成功；返回 true = 已连上 */
bool wifi_sta_wait_connected(int timeout_ms);

/** 取本机 IP 字符串，未连接时写 "0.0.0.0" */
void wifi_sta_get_ip(char *buf, size_t len);

/** 当前信号强度 RSSI（dBm，负值；未连接/失败返回 0） */
int wifi_sta_rssi(void);

/** 手动断开（断开后保持离线，不会自动重连；用于系统信息页的开关） */
esp_err_t wifi_sta_disconnect_user(void);

/** 手动重连（配合 wifi_sta_disconnect_user 使用） */
esp_err_t wifi_sta_reconnect_user(void);
