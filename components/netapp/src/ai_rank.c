/**
 * @file    ai_rank.c
 * @brief   国内 AI 产品榜（AICPB）抓取 + 解析（设备端实现）
 *
 * 数据源（2026-11-21 实测）：
 *   https://www.aicpb.com/ai-rankings/products/china-ai-rankings
 *   - 可直连（200 / 1.0s / 121KB），服务器支持 gzip（27KB / 0.47s）
 *   - 完整榜单只在 SSR HTML 的 <script type="application/ld+json"> 里（JSON-LD ItemList）
 *   - JSON-LD 只含「排位 + 中文产品名」，**没有访问量数值**（数值在正文表格，不抓）
 *   - 站点数据月份 2026-06 —— 季度/半年级更新，所以刷新周期定 24h（不是每小时！）
 *
 * 解析策略（不引 JSON 库，纯字符串搜索）：
 *   1) 定位 ld+json 块
 *   2) 用 "by Website Visits" / "by App" 分别定位两份 ItemList（纯 ASCII 锚点）
 *   3) 各自循环取 "position":N 与 "name":"..."（顺序固定：position 在前）
 *   4) 中文名走内置映射表 → 英文（面板字库只有 ASCII）；未映射 → "AI PRODUCT #N"
 *
 * gzip：用 esp_rom 自带的 miniz（tinfl_decompress_mem_to_mem），自己跳 gzip 头。
 *        解压失败则回退为不压缩的普通请求再试一次。
 */
#include "ai_rank.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "miniz.h"          /* esp_rom 自带（ROM 实现，零 flash 成本） */
#include "wifi_sta.h"

static const char *TAG = "ai_rank";

#define AR_URL        "https://www.aicpb.com/ai-rankings/products/china-ai-rankings"
#define AR_RAW_MAX    (32 * 1024)     /* gzip 压缩后实测 27KB */
#define AR_PLAIN_MAX  (160 * 1024)    /* 未压缩实测 121KB */
#define AR_PERIOD_MS  (24 * 60 * 60 * 1000)   /* ★ 站点季度更新，一天一次足够 */
#define AR_FAIL_MS    (30 * 60 * 1000)
#define AR_TIMEOUT_MS 20000

static SemaphoreHandle_t s_lock;
static ai_rank_data_t    s_snap;
static uint8_t          *s_rx;        /* 原始响应（PSRAM） */
static uint8_t          *s_txt;       /* 解压后文本（PSRAM） */
static int               s_rx_len;
static volatile bool     s_force;
static bool              s_use_gzip = true;

/* 中文名 → 英文展示名（未映射时兜底 "AI PRODUCT #N"） */
typedef struct
{
    const char *cn;
    const char *en;
} ar_map_t;

static const ar_map_t s_map[] = {
    { "DeepSeek",                          "DeepSeek" },
    { "纳米AI搜索",                         "Nano AI Search" },
    { "豆包｜抖音",                         "Doubao" },
    { "纳米AI",                             "Nano AI" },
    { "AI 搜索｜百度",                      "Baidu AI Search" },
    { "千问 qianwen.com｜阿里",             "Qwen (Alibaba)" },
    { "Kimi｜月之暗面",                     "Kimi" },
    { "Qwen.ai｜阿里",                      "Qwen.ai" },
    { "百度文库",                           "Baidu Wenku" },
    { "腾讯元宝",                           "Tencent Yuanbao" },
    { "豆包 - 字节跳动旗下 AI 助手",         "Doubao" },
    { "千问 - 阿里最强模型官方AI助手",       "Qwen" },
    { "夸克-阿里AI旗舰应用",                 "Quark" },
    { "DeepSeek - AI 智能助手",             "DeepSeek" },
    { "腾讯元宝-「DeepSeek+」智能新体验",    "Tencent Yuanbao" },
    { "百度网盘",                           "Baidu Netdisk" },
    { "即梦AI - 抖音旗下AI图片和视频工具",   "Jimeng AI" },
    { "可灵AI - AI图片&视频创作工具",        "Kling AI" },
    { "纳米AI - 首创多智能体蜂群",           "Nano AI" },
    { "Kimi",                              "Kimi" },
};

static const char *map_name(const char *cn, int rank, char *buf, size_t bsz)
{
    for (size_t i = 0; i < sizeof(s_map) / sizeof(s_map[0]); i++)
    {
        if (strcmp(cn, s_map[i].cn) == 0)
        {
            return s_map[i].en;
        }
    }
    snprintf(buf, bsz, "AI PRODUCT #%d", rank);
    return buf;
}

/* ---------------- HTTP ---------------- */

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA)
    {
        int room = s_rx_len < AR_RAW_MAX ? AR_RAW_MAX - s_rx_len : 0;
        if (room > 0 && s_rx != NULL)
        {
            int n = e->data_len < room ? e->data_len : room;
            memcpy(s_rx + s_rx_len, e->data, n);
            s_rx_len += n;
        }
    }
    return ESP_OK;
}

/* 跳过 gzip 头，返回 deflate 数据指针并更新 len（已去掉尾部 8 字节 CRC/ISIZE） */
static const uint8_t *gz_payload(const uint8_t *p, size_t *len)
{
    if (*len < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8)
    {
        return NULL;
    }
    size_t off = 10;
    uint8_t flg = p[3];
    if (flg & 0x04)   /* FEXTRA */
    {
        if (off + 2 > *len) { return NULL; }
        off += 2 + (size_t)(p[off] | (p[off + 1] << 8));
    }
    if (flg & 0x08)   /* FNAME */
    {
        while (off < *len && p[off] != 0) { off++; }
        off++;
    }
    if (flg & 0x10)   /* FCOMMENT */
    {
        while (off < *len && p[off] != 0) { off++; }
        off++;
    }
    if (flg & 0x02) { off += 2; }   /* FHCRC */
    if (off + 8 >= *len) { return NULL; }
    *len = *len - off - 8;
    return p + off;
}

/* ---------------- JSON-LD 解析 ---------------- */

/* 在 [p, lim) 内抓 10 条：position + name，写入 items */
static int parse_list(const char *p, const char *lim, ai_rank_item_t *items, int max_n)
{
    int n = 0;
    char fb[24];
    while (n < max_n)
    {
        const char *pp = strstr(p, "\"position\":");
        if (pp == NULL || pp >= lim) { break; }
        int rank = atoi(pp + 11);

        const char *np = strstr(pp, "\"name\":\"");
        if (np == NULL || np >= lim) { break; }
        np += 8;
        const char *ne = strchr(np, '"');
        if (ne == NULL || ne >= lim) { break; }

        char cn[96];
        size_t cl = (size_t)(ne - np);
        if (cl >= sizeof(cn)) { cl = sizeof(cn) - 1; }
        memcpy(cn, np, cl);
        cn[cl] = 0;

        items[n].rank = rank;
        snprintf(items[n].name, sizeof(items[n].name), "%s", map_name(cn, rank, fb, sizeof(fb)));
        snprintf(items[n].cn, sizeof(items[n].cn), "%s", cn);   /* 中文原名（UTF-8 原样保存） */
        n++;
        p = ne;
    }
    return n;
}

static bool parse_page(const char *txt)
{
    const char *ld = strstr(txt, "application/ld+json");
    if (ld == NULL) { return false; }
    const char *ld_end = strstr(ld, "</script>");
    if (ld_end == NULL) { ld_end = txt + strlen(txt); }

    /* 两份 ItemList：Website 靠 "by Website Visits"，App 靠 "by App" */
    const char *wseg = strstr(ld, "by Website Visits");
    const char *aseg = strstr(ld, "by App");
    if (wseg == NULL || aseg == NULL || wseg > ld_end || aseg > ld_end)
    {
        ESP_LOGW(TAG, "ld segments not found");
        return false;
    }

    /* Website 段：从锚点到 App 段（若 App 在 Website 之后） */
    const char *wlim = (aseg > wseg) ? aseg : ld_end;
    int nw = parse_list(wseg, wlim, s_snap.web, AI_RANK_N);

    const char *alim = ld_end;
    int na = parse_list(aseg, alim, s_snap.app, AI_RANK_N);

    if (nw < 3 && na < 3)
    {
        ESP_LOGW(TAG, "parse too few items (web %d / app %d)", nw, na);
        return false;
    }

    /* 站点数据版本（"dateModified":"Jun 2026"）—— 界面据此标注季度更新 */
    {
        const char *dm = strstr(ld, "\"dateModified\":\"");
        if (dm != NULL && dm < ld_end)
        {
            dm += 16;
            const char *de = strchr(dm, '"');
            if (de != NULL && (size_t)(de - dm) < sizeof(s_snap.month))
            {
                memcpy(s_snap.month, dm, (size_t)(de - dm));
                s_snap.month[de - dm] = 0;
            }
        }
    }
    s_snap.web_count = nw;
    s_snap.app_count = na;
    ESP_LOGI(TAG, "parsed web=%d app=%d, #1 %s / %s", nw, na,
             nw > 0 ? s_snap.web[0].name : "-", na > 0 ? s_snap.app[0].name : "-");
    return true;
}

/* ---------------- 一次抓取 ---------------- */

static bool fetch_once(void)
{
    if (s_rx == NULL)
    {
        s_rx = heap_caps_malloc(AR_RAW_MAX, MALLOC_CAP_SPIRAM);
    }
    if (s_txt == NULL)
    {
        s_txt = heap_caps_malloc(AR_PLAIN_MAX, MALLOC_CAP_SPIRAM);
    }
    if (s_rx == NULL || s_txt == NULL)
    {
        ESP_LOGE(TAG, "no psram buffer");
        return false;
    }

    s_rx_len = 0;

    esp_http_client_config_t cfg = {
        .url               = AR_URL,
        .event_handler     = on_http,
        .timeout_ms        = AR_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = "Mozilla/5.0 (s31-panel)",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) { return false; }
    if (s_use_gzip)
    {
        esp_http_client_set_header(cli, "Accept-Encoding", "gzip");
    }

    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    s_snap.http_code = code;
    if (err != ESP_OK || code != 200 || s_rx_len < 64)
    {
        ESP_LOGW(TAG, "fetch fail: %s http %d rx %d", esp_err_to_name(err), code, s_rx_len);
        return false;
    }

    const char *txt = (const char *)s_rx;
    int         tlen = s_rx_len;

    if (s_rx[0] == 0x1f && s_rx[1] == 0x8b)
    {
        size_t plen = (size_t)s_rx_len;
        const uint8_t *payload = gz_payload(s_rx, &plen);
        if (payload == NULL)
        {
            ESP_LOGW(TAG, "gzip header bad, fallback plain");
            s_use_gzip = false;
            return false;
        }
        size_t out = tinfl_decompress_mem_to_mem(s_txt, AR_PLAIN_MAX, payload, plen, 0);
        if (out == 0 || out == (size_t)-1)
        {
            ESP_LOGW(TAG, "inflate failed (%d), fallback plain", (int)out);
            s_use_gzip = false;
            return false;
        }
        s_txt[out < AR_PLAIN_MAX ? out : AR_PLAIN_MAX - 1] = 0;
        txt  = (const char *)s_txt;
        tlen = (int)out;
        ESP_LOGI(TAG, "gzip %d -> %d bytes", s_rx_len, tlen);
    }
    else
    {
        /* 未压缩：直接补 0 当字符串 */
        s_rx[s_rx_len < AR_RAW_MAX ? s_rx_len : AR_RAW_MAX - 1] = 0;
        tlen = s_rx_len;
        ESP_LOGI(TAG, "plain %d bytes", tlen);
    }

    return parse_page(txt);
}

/* ---------------- 任务 ---------------- */

static void ar_task(void *arg)
{
    (void)arg;

    while (!wifi_sta_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(6000));      /* 与 weather / hf 错开 */

    while (1)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.state = AI_RANK_LOADING;
        xSemaphoreGive(s_lock);

        bool ok = fetch_once();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok)
        {
            /* 记录数据月份（页面 "Jun 2026 Edition"）与拉取时刻 */
            time_t now = 0;
            struct tm tm_now;
            time(&now);
            localtime_r(&now, &tm_now);
            /* strftime 而非 snprintf：-Werror=format-truncation 对 %02d 取值范围无法推断 */
            strftime(s_snap.updated, sizeof(s_snap.updated), "%m-%d %H:%M", &tm_now);
            s_snap.gen++;                  /* ★ 代号自增：顺序变化也能被 UI 感知 */
            s_snap.state = AI_RANK_OK;
        }
        else
        {
            s_snap.state = AI_RANK_FAIL;
        }
        xSemaphoreGive(s_lock);

        int wait_ms = ok ? AR_PERIOD_MS : AR_FAIL_MS;
        s_force = false;
        for (int w = 0; w < wait_ms; w += 1000)
        {
            if (s_force) { break; }
            if ((w % 60000) == 0 && !wifi_sta_connected()) { break; }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------------- 对外 ---------------- */

void ai_rank_start(void)
{
    if (s_lock != NULL) { return; }
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = AI_RANK_INIT;
    snprintf(s_snap.month, sizeof(s_snap.month), "%s", "");

    if (xTaskCreate(ar_task, "ai_rank", 12288, NULL, 3, NULL) == pdPASS)
    {
        ESP_LOGI(TAG, "ai_rank task started (aicpb.com, 24h)");
    }
    else
    {
        ESP_LOGE(TAG, "task create failed");
    }
}

void ai_rank_refresh_now(void)
{
    s_force = true;
}

void ai_rank_get(ai_rank_data_t *out)
{
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->state = AI_RANK_INIT;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}
