/**
 * @file    app_cfg.h
 * @brief   运行时配置（NVS）：WiFi / 城市 / PC 中转地址 / 显示偏好
 *
 * ★★★ 11-22：把原本散在编译期宏里的"因人而异"的参数收进来（wifi_config.h 的
 *   WIFI_SSID / WIFI_PASS / PC_MONITOR_HOST / PC_MONITOR_PORT）。
 *   目的：**同一份固件发给不同的人、换一个网络，不用改代码重烧**。
 *
 *   编译期宏降级为**首次上电的默认值**（fallback），所以现在这版行为不变；
 *   以后加配网页（SoftAP + 表单）时，业务代码一行不用改，只是改成往这里写。
 *
 *   ★ 不该放进来的：屏幕时序（PCLK / porch / 采样沿）、字体、引脚 —— 那些是
 *     编译期的物理参数，不是用户偏好。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_CFG_VER   2        /* 结构体版本：字段变动时 +1，旧 blob 会自动回落到默认值 */

typedef struct
{
    uint16_t ver;

    /* 网络 */
    char     wifi_ssid[33];
    char     wifi_pass[65];

    /* 位置：city 为空且 lat/lon 为 0 → 按公网 IP 自动定位（wttr.in 的默认行为）
     * ★ 为什么要 lat/lon：open-meteo 的地理编码只认到**市级**（实测"海淀区"查不到），
     *   而空气质量和天气最终都要经纬度。想要街道级精度，只能直接填坐标。 */
    char     city[24];
    float    lat;          /* 0 = 未设置 */
    float    lon;

    /* PC 中转（pc_monitor_server.py）：B 站热搜 + PC 状态共用这一组 */
    char     pc_host[40];
    uint16_t pc_port;

    /* 显示偏好 */
    uint8_t  backlight;    /* 0~100，滑条同值 */
    int8_t   tz;           /* UTC 偏移小时，默认 +8 */
    uint8_t  theme;        /* 0=深色（默认） 1=浅色 */
} app_cfg_t;

/** 初始化：读 NVS，没有/版本不符则写入编译期默认值。必须在 nvs_flash_init() 之后调用。 */
esp_err_t app_cfg_init(void);

/** 只读访问（线程安全：值在 init 后不再被别的任务改） */
const app_cfg_t *app_cfg_get(void);

/** 可写访问：改完必须调 app_cfg_save()，否则只在 RAM 里 */
app_cfg_t *app_cfg_mut(void);

esp_err_t app_cfg_save(void);

/** 恢复默认值并写回（配网页的"恢复出厂"用） */
esp_err_t app_cfg_reset(void);

/** 打印当前配置（密码脱敏），供启动日志与配网页预览用 */
void app_cfg_dump(void);
