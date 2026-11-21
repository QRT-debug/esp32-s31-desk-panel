/**
 * @file    bili_hot.h
 * @brief   B 站热搜榜（api.bilibili.com，免 key）
 *
 * 数据源（2026-11-21 实测）：https://api.bilibili.com/x/web-interface/search/square?limit=10
 *   200 / 0.2s，返回 data.trending.list[]：
 *     keyword（热搜词，中文）/ show_name / heat_score（热度值，如 39354137）
 * ★ 热搜变化快（分钟级），刷新周期 30 分钟；界面显示拉取时刻。
 */
#pragma once

#include <stdbool.h>

#define BILI_N 10

typedef enum
{
    BILI_INIT = 0,
    BILI_LOADING,
    BILI_OK,
    BILI_FAIL,
} bili_state_t;

typedef struct
{
    int  rank;
    char word[96];       /* 热搜词（UTF-8，可能较长） */
    long heat;           /* 热度值（失败/缺失为 0） */
} bili_item_t;

typedef struct
{
    bili_state_t state;
    int          count;
    bili_item_t  item[BILI_N];
    char         updated[20];   /* 拉取时刻 "MM-DD HH:MM" */
    unsigned     gen;           /* 数据代号：每次成功 +1（内容变化触发重绘） */
    int          http_code;
} bili_data_t;

void bili_start(void);
void bili_refresh_now(void);
void bili_get(bili_data_t *out);
