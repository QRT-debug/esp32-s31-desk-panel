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
 * 配网页（SoftAP + 表单）上线后，改的是 NVS，这里连碰都不用碰。
 *
 * ★ 注意：改这里的值只对**全新设备**或**执行过 app_cfg_reset() 的设备**生效。
 *   已在用的设备会一直用 NVS 里那份。
 *
 * ★★★ 2026-11-23：真实 WiFi 凭据已移出本文件
 *   本仓库是公开的，把家里的 SSID/密码写在源码里等于公之于众。
 *   现在的做法：
 *     - 真实值放 `wifi_config_private.h`（同目录，**已加入 .gitignore**，不进仓库）
 *     - 本文件只留**空默认值** ⇒ 首次上电不预置网络，直接进配网流程（连 SoftAP 填表）
 *   本地开发想省掉配网这一步，就自己建那个私有头：
 *
 *     // components/netapp/include/wifi_config_private.h
 *     #define WIFI_SSID   "你的SSID"
 *     #define WIFI_PASS   "你的密码"
 *
 *   没有私有头也能编译、能跑，只是首次上电要走一遍配网。
 */
#pragma once

/* 本地私有覆盖（不进仓库） */
#if defined(__has_include)
#  if __has_include("wifi_config_private.h")
#    include "wifi_config_private.h"
#  endif
#endif

/* 首次上电默认 WiFi。留空 = 不预置网络，首次上电直接进配网流程。 */
#ifndef WIFI_SSID
#define WIFI_SSID           ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS           ""
#endif

/* 首次上电默认 PC 中转地址：示例网段，真实地址由配网页设置并存入 NVS。
 * 跑 tools/pc_monitor_server.py 时它会把本机 IP 打出来，照着填即可。 */
#ifndef PC_MONITOR_HOST
#define PC_MONITOR_HOST     "192.168.1.100"
#endif
#ifndef PC_MONITOR_PORT
#define PC_MONITOR_PORT     8080
#endif
