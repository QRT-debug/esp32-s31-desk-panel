/**
 * @file    hf_trend.h
 * @brief   HuggingFace Trending Top 10（hf-mirror 国内镜像）
 *
 * 数据源（2026-11-21 实测）：
 *   https://huggingface.co   → 国内超时不可达
 *   https://hf-mirror.com    → 200 / 1.8s ✓
 *   GET /api/models?sort=trendingScore&direction=-1&limit=10   （响应约 5KB）
 *
 * 字段：id / pipeline_tag / likes / downloads / createdAt
 * 刷新：上电首拉 → 成功每小时一次 / 失败 5 分钟重试
 */
#pragma once

#include <stdbool.h>

#define HF_TOP_N 10

typedef enum
{
    HF_STATE_INIT = 0,
    HF_STATE_LOADING,
    HF_STATE_OK,
    HF_STATE_FAIL,
} hf_state_t;

typedef struct
{
    char id[56];         /* 模型名（过长的截断显示） */
    char task[40];       /* pipeline_tag（null → "--"；最长 automatic-speech-recognition 28） */
    int  likes;
    int  downloads;
    char created[12];    /* "MM-DD HH:MM" */
    bool is_new;         /* 相比上次刷新是新进榜（框内 ⊕/= 用） */
} hf_item_t;

typedef struct
{
    hf_state_t state;
    int        count;                  /* 有效条目数（正常 10） */
    hf_item_t  item[HF_TOP_N];
    char       updated[12];            /* 拉取时刻 "HH:MM" */
    unsigned   gen;                    /* 数据代号：每次成功刷新 +1。★ 排名顺序变化时
                                        * count/state 都不变，必须靠它触发 UI 重绘。 */
    int        http_code;
} hf_data_t;

void hf_start(void);          /* 启动刷新任务（幂等） */
void hf_refresh_now(void);    /* 立即刷新（手动按钮） */
void hf_get(hf_data_t *out);  /* 拷贝快照 */
