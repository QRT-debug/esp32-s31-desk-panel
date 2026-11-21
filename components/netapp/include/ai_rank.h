/**
 * @file    ai_rank.h
 * @brief   国内 AI 产品榜（AICPB）数据接口
 *
 * 数据源：https://www.aicpb.com/ai-rankings/products/china-ai-rankings
 *   - 站点可直连（实测 200 / 1.0s / 121KB，gzip 27KB）
 *   - 完整 Top10 只在 SSR HTML 的 JSON-LD 里（排位 + 中文产品名，无访问量数值）
 *   - ★ 站点数据季度/半年更新（当前 2026-06），刷新周期定 24h，界面须标注
 *   - 产品名为中文，本模块内置「中文→英文」映射表（面板字库仅 ASCII）
 */
#pragma once

#include <stdbool.h>

#define AI_RANK_N 10

typedef enum
{
    AI_RANK_INIT = 0,
    AI_RANK_LOADING,
    AI_RANK_OK,
    AI_RANK_FAIL,
} ai_rank_state_t;

typedef struct
{
    int  rank;
    char name[32];     /* 英文展示名（映射表产出；未映射 = "AI PRODUCT #N"） */
    char cn[96];       /* 中文原名（UTF-8，用于小字显示；可能含 ASCII 混排） */
} ai_rank_item_t;

typedef struct
{
    ai_rank_state_t state;
    char            month[16];     /* 站点数据版本，如 "Jun 2026"（季度更新） */
    char            updated[20];   /* 本机拉取时刻 "MM-DD HH:MM" */
    int             web_count;
    int             app_count;
    unsigned        gen;           /* 数据代号：每次成功刷新 +1（排名互换时触发重绘） */
    int             http_code;
    ai_rank_item_t  web[AI_RANK_N];
    ai_rank_item_t  app[AI_RANK_N];
} ai_rank_data_t;

void ai_rank_start(void);          /* 启动任务：WiFi 就绪后首拉，成功 24h / 失败 30min */
void ai_rank_refresh_now(void);    /* 手动刷新 */
void ai_rank_get(ai_rank_data_t *out);
