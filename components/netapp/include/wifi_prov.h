/**
 * @file    wifi_prov.h
 * @brief   配网：SoftAP + 网页表单（把运行时配置填进 NVS）
 *
 * 为什么用网页而不是屏幕输入：屏幕上有触摸，但输 WiFi 密码要虚拟键盘，
 * 体验极差；而且配置项会越来越多（地址、PC 地址、亮度…）。
 * 手机浏览器填表是唯一顺手的方式，设备只负责"告诉你连哪个热点、开哪个网址"。
 *
 * ★ 串口兜底：WiFi 配错导致连不上时，网页也进不去（连不上就没有网络）。
 *   那种情况只能靠 `idf.py monitor` 里的 cfg 命令救，见 app_cfg 的串口命令。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** AP 前缀：实际 SSID 是 SmartPanel-XXXX（MAC 后 2 字节，多台设备不冲突） */
#define PROV_AP_PREFIX   "SmartPanel-"
#define PROV_AP_PASS     "12345678"
#define PROV_AP_IP       "192.168.4.1"

/**
 * 启动配网：开 SoftAP（与 STA 共存）+ HTTP 服务。
 * 已在运行则直接返回 ESP_OK。
 */
esp_err_t wifi_prov_start(void);

/** 停止配网：关 HTTP 服务 + 切回纯 STA 模式 */
esp_err_t wifi_prov_stop(void);

bool wifi_prov_running(void);

/** 当前 AP 的 SSID（供屏幕显示），未启动时返回空串 */
const char *wifi_prov_ssid(void);
