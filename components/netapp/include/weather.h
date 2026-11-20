/**
 * @file    weather.h
 * @brief   天气数据层（wttr.in 免 key · IP 自动定位）
 *
 * 设计（11-20）：
 *   - 数据源可替换：UI 只依赖 wx_data_t 快照，换 API 只改 weather.c；
 *   - 刷新节奏：WiFi 就绪首拉 → 成功每 30 分钟 / 失败每 5 分钟重试；
 *   - weather_refresh_now() 供 UI 手动刷新（插队一次）。
 */
#pragma once

#include <stdbool.h>

typedef enum
{
    WX_STATE_INIT = 0,   /* 刚启动，还没拉过 */
    WX_STATE_LOADING,    /* 正在拉取 */
    WX_STATE_OK,         /* 有有效数据 */
    WX_STATE_FAIL,       /* 上次拉取失败（保留旧数据可继续显示） */
} wx_state_t;

typedef struct
{
    wx_state_t state;
    int  temp_c;         /* 当前温度 °C */
    int  feels_c;        /* 体感 */
    int  humidity;       /* 相对湿度 % */
    int  wind_kmph;      /* 风速 km/h */
    char wind_dir[8];    /* 风向 16 方位（"SE"） */
    char desc[40];       /* 天气描述（英文，如"Smoky haze"） */
    char city[40];       /* 城市名 */
    char region[32];     /* 省/州 */
    int  today_max, today_min;
    int  tmrw_max, tmrw_min;
    /* 今天逐 3 小时温度（00/03/06/…/21），-99 = 无效 */
    int  hourly_t[8];
    char updated[12];    /* 数据时刻 "HH:MM" */
    int  http_code;      /* 最近一次 HTTP 状态码（调试用） */
} wx_data_t;

/* 启动后台刷新任务（幂等；WiFi 未连时任务内部等待） */
void weather_start(void);

/* 请求立即刷新（异步，不阻塞 UI） */
void weather_refresh_now(void);

/* 拷贝当前快照（线程安全） */
void weather_get(wx_data_t *out);
