/**
 * @file    bili_hot.c
 * @brief   B 站热搜抓取（api.bilibili.com，免 key；实测 200 / 0.2s）
 *
 * 响应结构：
 *   {"code":0,"data":{"trending":{"list":[{"keyword":"…","show_name":"…",
 *     "icon":"…","uri":"…","goto":"…","heat_score":39354137}, …]}}}
 * 解析：以 "keyword":" 为锚点逐条取；每条范围内再找 "heat_score":
 *      （show_name 常与 keyword 相同，不另取）
 */
#include "data_cache.h"
#include "bili_hot.h"

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
#include "wifi_sta.h"
#include "net_gate.h"

static const char *TAG = "bili_hot";

/* ★★ 走 PC 中转（2026-11-21 实测定案）：
 *   设备直连 B 站任何域名都失败——不是内存（内部 RAM 余 136KB、PSRAM 14MB）、
 *   不是 IPv6、不是证书，而是 esp-tls 反复 `select() timeout`：设备所在网络到
 *   B 站 CDN 节点（221.204.x / 218.60.x）TCP 建连超时，PC 上 curl 却 200。
 *   ⇒ 由 PC 的 pc_monitor_server.py 代抓（端点 /api/bili.txt，纯文本 `词|热度`），
 *     设备只拉 HTTP，不碰 TLS。代价：PC 需开机跑服务。
 */
#include "app_cfg.h"
#include "wifi_config.h"   /* PC_MONITOR_HOST / PC_MONITOR_PORT */

/* ★ 11-22：host/port 改为与 pc_monitor 共用同一组运行时配置（同一台 PC 上的中转脚本） */
#define BILI_PATH      "/api/bili.txt"
#define BILI_RX_MAX    (24 * 1024)      /* 实测响应 ~2KB，留足余量 */
#define BILI_PERIOD_MS (10 * 60 * 1000)
#define BILI_FAIL_MS   (5 * 60 * 1000)
#define BILI_TIMEOUT   12000

static SemaphoreHandle_t s_lock;
static bili_data_t       s_snap;
static char             *s_rx;
static int               s_rx_len;
static volatile bool     s_force;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA)
    {
        int room = BILI_RX_MAX - 1 - s_rx_len;
        if (room > 0 && s_rx != NULL)
        {
            int n = e->data_len < room ? e->data_len : room;
            memcpy(s_rx + s_rx_len, e->data, n);
            s_rx_len += n;
            s_rx[s_rx_len] = 0;
        }
    }
    return ESP_OK;
}

/* 解析纯文本：每行 `热搜词|热度`（PC 端产出），最多 BILI_N 条 */
static int parse_list(bili_data_t *out, const char *txt, int len)
{
    int  n = 0;
    const char *p = txt;
    const char *lim = txt + len;

    while (n < BILI_N && p < lim)
    {
        const char *e = p;
        while (e < lim && *e != '\n' && *e != '\r')
        {
            e++;
        }
        if (e > p + 1)
        {
            /* 拆 `词|热度` */
            const char *bar = p;
            while (bar < e && *bar != '|')
            {
                bar++;
            }
            size_t wl = (size_t)(bar - p);
            if (wl >= sizeof(out->item[n].word))
            {
                wl = sizeof(out->item[n].word) - 1;
            }
            memcpy(out->item[n].word, p, wl);
            out->item[n].word[wl] = 0;
            out->item[n].heat = (bar < e) ? atol(bar + 1) : 0;
            out->item[n].rank = n + 1;
            n++;
        }
        p = e;
        while (p < lim && (*p == '\n' || *p == '\r'))
        {
            p++;
        }
    }
    return n;
}

static bool fetch_once(bili_data_t *out)
{
    if (s_rx == NULL)
    {
        s_rx = heap_caps_malloc(BILI_RX_MAX, MALLOC_CAP_SPIRAM);
        if (s_rx == NULL)
        {
            s_rx = heap_caps_malloc(BILI_RX_MAX, MALLOC_CAP_DEFAULT);
        }
    }
    if (s_rx == NULL)
    {
        return false;
    }

    s_rx_len = 0;
    s_rx[0]  = 0;

    char url[128];
    const app_cfg_t *acfg = app_cfg_get();
    snprintf(url, sizeof(url), "http://%s:%u" BILI_PATH, acfg->pc_host, (unsigned)acfg->pc_port);

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = on_http,
        .timeout_ms        = BILI_TIMEOUT,
        .user_agent        = "s31-panel",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL)
    {
        return false;
    }

    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    out->http_code = code;
    if (err != ESP_OK || code != 200 || s_rx_len < 40)
    {
        ESP_LOGW(TAG, "fetch fail: %s http %d rx %d", esp_err_to_name(err), code, s_rx_len);
        return false;
    }

    int n = parse_list(out, s_rx, s_rx_len);
    out->count = n;
    if (n < 3)
    {
        ESP_LOGW(TAG, "parse too few (%d)", n);
        return false;
    }
    ESP_LOGI(TAG, "OK %d items, #1 %s (heat %ld)", n, out->item[0].word, out->item[0].heat);
    return true;
}

static void bili_task(void *arg)
{
    (void)arg;

    while (!wifi_sta_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(22000));   /* ★ 22s：weather(9s)+AQI(~18s)、hf(6.5s)、ai_rank(15s) 都跑完再上，
                                    *   实测 9s 时与 AQI 的 TLS 抢连接会 ESP_ERR_HTTP_CONNECT */

    while (1)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.state = BILI_LOADING;
        xSemaphoreGive(s_lock);

        bili_data_t tmp;
        memset(&tmp, 0, sizeof(tmp));
            bool gate_ok = net_gate_take(120000);   /* ★ 串行化 HTTPS：TLS 握手 30KB+，并发必 -0x3000 */
    ESP_LOGI(TAG, "heap before: int=%u (min %u) psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool ok = fetch_once(&tmp);
    ESP_LOGI(TAG, "heap after: int=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    net_gate_give();
    if (!gate_ok)
    {
        ESP_LOGW(TAG, "net gate timeout (proceeded)");
    }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok)
        {
            time_t now = 0;
            struct tm tm_now;
            time(&now);
            localtime_r(&now, &tm_now);
            strftime(tmp.updated, sizeof(tmp.updated), "%m-%d %H:%M", &tm_now);
            tmp.gen = s_snap.gen + 1;
            tmp.state = BILI_OK;
            s_snap = tmp;
        }
        else
        {
            s_snap.state = BILI_FAIL;
        }
        xSemaphoreGive(s_lock);

        int wait_ms = ok ? BILI_PERIOD_MS : BILI_FAIL_MS;
        /* ★ 锁外、界面更新前写缓存 */
        if (ok)
        {
            data_cache_save("bili", &tmp, sizeof(tmp));
        }
        s_force = false;
        for (int w = 0; w < wait_ms; w += 1000)
        {
            if (s_force)
            {
                break;
            }
            if ((w % 60000) == 0 && !wifi_sta_connected())
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void bili_start(void)
{
    if (s_lock != NULL)
    {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = BILI_INIT;
    /* ★ 先读上次缓存 */
    if (data_cache_load("bili", &s_snap, sizeof(s_snap)) == ESP_OK)
    {
        s_snap.state = BILI_OK;
        ESP_LOGI(TAG, "cache hit: %d items (updated %s)", s_snap.count, s_snap.updated);
    }

    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);   /* 临时：定位 bilibili TLS 失败 */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
    if (xTaskCreate(bili_task, "bili_hot", 8192, NULL, 3, NULL) == pdPASS)
    {
        ESP_LOGI(TAG, "bili_hot task started (30min)");
    }
    else
    {
        ESP_LOGE(TAG, "task create failed");
    }
}

void bili_refresh_now(void)
{
    s_force = true;
}

void bili_get(bili_data_t *out)
{
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->state = BILI_INIT;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}
