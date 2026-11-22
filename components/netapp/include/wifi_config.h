/**
 * @file    wifi_config.h
 * @brief   ★ 编译期**默认值**（不再是你需要改的地方）
 *
 * 2026-11-22 改造：这些值已经搬进 NVS 的运行时配置（components/netapp/ 的 app_cfg.h）。
 * 这里只在**首次上电**（NVS 里没有 "appcfg" 记录）和**恢复出厂**时被读一次。
 *
 * ⇒ 换网络 / 换 PC / 换城市，**不需要再改这个文件**，也不要靠重烧解决 ——
 *   那是"配置"该干的事，不是"固件"该干的事。
 *
 * 以后配网页（SoftAP + 表单）上线后，改的是 NVS，这里连碰都不用碰。
 *
 * ★ 注意：改这里的值只对**全新设备**或**执行过 app_cfg_reset() 的设备**生效。
 *   已在用的设备会一直用 NVS 里那份。
 */
#pragma once

/* 首次上电默认 WiFi */
#define WIFI_SSID           "MY_WIFI"
#define WIFI_PASS           "REDACTED_PASS"

/* 首次上电默认 PC 中转地址（跑 tools/pc_monitor_server.py 时它会把本机 IP 打出来） */
#define PC_MONITOR_HOST     "192.168.1.100"
#define PC_MONITOR_PORT     8080
