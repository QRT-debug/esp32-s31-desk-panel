/**
 * @file    wifi_config.h
 * @brief   ★ 需要你改的就这一个文件
 *
 * WIFI_SSID / WIFI_PASS 填你家路由器的名称和密码。
 * PC_MONITOR_HOST 填电脑的局域网 IP —— 跑 tools/pc_monitor_server.py
 * 时它会直接打印出来（形如 192.168.1.2）。
 */
#pragma once

#define WIFI_SSID           "MY_WIFI"
#define WIFI_PASS           "REDACTED_PASS"

#define PC_MONITOR_HOST     "192.168.1.100"
#define PC_MONITOR_PORT     8080
