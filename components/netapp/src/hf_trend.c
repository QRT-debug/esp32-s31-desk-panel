/**
 * @file    hf_trend.c
 * @brief   HuggingFace Trending Top 10 抓取 + 手写 JSON 解析（零依赖）
 *
 * 解析策略（数组而非固定对象）：
 *   以 "id":" 作为每条目的锚点，条目范围 = 本锚点到下一个锚点之间；
 *   在范围内分别取 pipeline_tag / likes / downloads / createdAt。
 *   这样与字段顺序无关，且不受 tags 数组里的 {} 影响。
 */
#include "data_cache.h"
#include "hf_trend.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "wifi_sta.h"
#include "net_gate.h"

static const char *TAG = "hf_trend";

#define HF_URL            "https://hf-mirror.com/api/models?sort=trendingScore&direction=-1&limit=10"
#define HF_RX_MAX         (12 * 1024)       /* 实测响应约 5KB，留足余量 */
#define HF_OK_PERIOD_MS   (60 * 60 * 1000)  /* 成功：每小时 */
#define HF_FAIL_PERIOD_MS (5 * 60 * 1000)
#define HF_TIMEOUT_MS     12000

static SemaphoreHandle_t s_lock;
static hf_data_t  s_snap;
static char      *s_rx;
static int        s_rx_len;
static volatile bool s_force;

/* ---------------- HTTP 收包 ---------------- */

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA)
    {
        int room = HF_RX_MAX - 1 - s_rx_len;
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

/* ---------------- 解析小工具 ---------------- */

/* 在 [begin,end) 范围内找 key 后的字符串值（"key":"value"） */
static bool range_str(const char *begin, const char *end,
                      const char *key, char *out, size_t osz)
{
    size_t klen = strlen(key);
    for (const char *p = begin; p + klen < end; p++)
    {
        if (memcmp(p, key, klen) == 0)
        {
            p += klen;
            while (p < end && (*p == ' ' || *p == ':')) { p++; }
            if (p < end && *p == '"')
            {
                p++;
                const char *e = p;
                while (e < end && *e != '"') { e++; }
                size_t n = (size_t)(e - p);
                if (n >= osz) { n = osz - 1; }
                memcpy(out, p, n);
                out[n] = 0;
                return n > 0;
            }
            return false;
        }
    }
    return false;
}

/* 范围内找 key 后的整数值（"key":123） */
static int range_int(const char *begin, const char *end, const char *key, int def)
{
    size_t klen = strlen(key);
    for (const char *p = begin; p + klen < end; p++)
    {
        if (memcmp(p, key, klen) == 0)
        {
            p += klen;
            while (p < end && (*p == ' ' || *p == ':')) { p++; }
            return atoi(p);
        }
    }
    return def;
}

/* "2026-11-20T19:22:11.000Z" → "11-20 19:22" */
static void fmt_created(const char *iso, char *out, size_t osz)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) >= 5)
    {
        snprintf(out, osz, "%02d-%02d %02d:%02d", mo, d, hh, mm);
    }
    else
    {
        out[0] = 0;
    }
}

/* ---------------- 一次拉取 ---------------- */

static bool fetch_once(hf_data_t *out)
{
    if (s_rx == NULL)
    {
        s_rx = heap_caps_malloc(HF_RX_MAX, MALLOC_CAP_SPIRAM);
        if (s_rx == NULL)
        {
            s_rx = heap_caps_malloc(HF_RX_MAX, MALLOC_CAP_DEFAULT);
        }
    }
    if (s_rx == NULL)
    {
        return false;
    }

    ESP_LOGI(TAG, "fetching %s", HF_URL);
    s_rx_len = 0;
    s_rx[0]  = 0;

    esp_http_client_config_t cfg = {
        .url               = HF_URL,
        .event_handler     = on_http,
        .timeout_ms        = HF_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = "s31-hf-trend/1.0",
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
    if (err != ESP_OK || code != 200 || s_rx_len < 100 || s_rx[0] != '[')
    {
        ESP_LOGW(TAG, "fetch failed: %s http %d rx %d", esp_err_to_name(err), code, s_rx_len);
        return false;
    }

    /* ---- 逐条目解析 ---- */
    int cnt = 0;
    const char *p = s_rx;
    while (cnt < HF_TOP_N)
    {
        const char *anchor = strstr(p, "\"id\":\"");
        if (anchor == NULL)
        {
            break;
        }
        const char *body = anchor;
        const char *next = strstr(anchor + 6, "\"id\":\"");
        const char *end  = (next != NULL) ? next : (s_rx + s_rx_len);

        hf_item_t *it = &out->item[cnt];
        memset(it, 0, sizeof(*it));

        /* id（模型名）：anchor 后到下一个引号 */
        {
            const char *v = anchor + 6;
            const char *e = v;
            while (e < end && *e != '"') { e++; }
            size_t n = (size_t)(e - v);
            if (n >= sizeof(it->id)) { n = sizeof(it->id) - 1; }
            memcpy(it->id, v, n);
            it->id[n] = 0;
        }
        if (!range_str(body, end, "\"pipeline_tag\"", it->task, sizeof(it->task)))
        {
            snprintf(it->task, sizeof(it->task), "%s", "--");
        }
        it->likes     = range_int(body, end, "\"likes\"", 0);
        it->downloads = range_int(body, end, "\"downloads\"", 0);

        char iso[40];
        if (range_str(body, end, "\"createdAt\"", iso, sizeof(iso)))
        {
            fmt_created(iso, it->created, sizeof(it->created));
        }
        else
        {
            snprintf(it->created, sizeof(it->created), "%s", "--");
        }

        cnt++;
        p = (next != NULL) ? next : (s_rx + s_rx_len);
        if (next == NULL)
        {
            break;
        }
    }

    out->count = cnt;
    if (cnt == 0)
    {
        ESP_LOGW(TAG, "parse: 0 items (rx %d)", s_rx_len);
        return false;
    }

    ESP_LOGI(TAG, "OK %d items, first: %s", cnt, out->item[0].id);
    return true;
}

/* ---------------- 刷新任务 ---------------- */

static void hf_task(void *arg)
{
    (void)arg;

    while (!wifi_sta_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(2500));   /* 与 weather 错开，避免同时抢 TLS */

    while (1)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.state = HF_STATE_LOADING;
        xSemaphoreGive(s_lock);

        hf_data_t tmp;
        memset(&tmp, 0, sizeof(tmp));
            bool gate_ok = net_gate_take(120000);   /* ★ 串行化 HTTPS：TLS 握手 30KB+，并发必 -0x3000 */
    bool ok = fetch_once(&tmp);
    net_gate_give();
    if (!gate_ok)
    {
        ESP_LOGW(TAG, "net gate timeout (proceeded)");
    }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok)
        {
            /* 与上次榜单对比：不在旧榜 = 新进（首次拉取全部按在榜处理，避免满屏 NEW） */
            bool had_prev = (s_snap.state == HF_STATE_OK) && (s_snap.count > 0);
            for (int i = 0; i < tmp.count; i++)
            {
                tmp.item[i].is_new = false;
                if (had_prev)
                {
                    bool found = false;
                    for (int j = 0; j < s_snap.count; j++)
                    {
                        if (strcmp(tmp.item[i].id, s_snap.item[j].id) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                    tmp.item[i].is_new = !found;
                }
            }
            tmp.state = HF_STATE_OK;
            tmp.gen = s_snap.gen + 1;          /* ★ 代号自增：顺序变化也能被 UI 感知 */
            /* 记录拉取时刻 */
            time_t now = 0;
            struct tm tm_now;
            time(&now);
            localtime_r(&now, &tm_now);
            snprintf(tmp.updated, sizeof(tmp.updated), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
            s_snap = tmp;
        }
        else
        {
            s_snap.state = HF_STATE_FAIL;
        }
        xSemaphoreGive(s_lock);

        int wait_ms = ok ? HF_OK_PERIOD_MS : HF_FAIL_PERIOD_MS;
        /* ★ 锁外、界面更新前写缓存（写 flash 会禁 i-cache，此刻画面静止） */
        if (ok)
        {
            data_cache_save("hf", &tmp, sizeof(tmp));
        }
        s_force = false;
        for (int waited = 0; waited < wait_ms; waited += 1000)
        {
            if (s_force)
            {
                break;
            }
            if ((waited % 30000) == 0 && !wifi_sta_connected())
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------------- 对外 ---------------- */

void hf_start(void)
{
    if (s_lock != NULL)
    {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = HF_STATE_INIT;
    /* ★ 先读上次缓存：断网/刚开机也能立刻显示榜单 */
    if (data_cache_load("hf", &s_snap, sizeof(s_snap)) == ESP_OK)
    {
        s_snap.state = HF_STATE_OK;
        ESP_LOGI(TAG, "cache hit: %d items (updated %s)", s_snap.count, s_snap.updated);
    }

    if (xTaskCreate(hf_task, "hf_trend", 10240, NULL, 3, NULL) == pdPASS)
    {
        ESP_LOGI(TAG, "hf task started (hf-mirror)");
    }
    else
    {
        ESP_LOGE(TAG, "task create failed");
    }
}

void hf_refresh_now(void)
{
    s_force = true;
}

void hf_get(hf_data_t *out)
{
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->state = HF_STATE_INIT;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}
