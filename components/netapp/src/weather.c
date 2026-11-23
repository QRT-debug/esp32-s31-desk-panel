/**
 * @file    weather.c
 * @brief   天气数据层：wttr.in（免 key）+ IP 自动定位 + 手写 JSON 字段提取
 *
 * ★ 服务：https://wttr.in/?format=j1
 *   - 免注册；IP 自动定位城市；lang=zh 让 weatherDesc 返回中文；
 *   - 响应 JSON 较大（15~25KB）→ 收包缓冲从 PSRAM 分配（内部 RAM 不够）；
 *   - 频率限制：每 30 分钟一次，够客气。
 *
 * ★ 解析路径（j1 格式，字段都是字符串）：
 *   current_condition[0]: temp_C / FeelsLikeC / humidity / windspeedKmph
 *                         / winddir16Point / weatherDesc[0].value
 *   nearest_area[0]:      areaName[0].value / region[0].value
 *   weather[0]/[1]:       maxtempC / mintempC（今天/明天）
 */
#include "data_cache.h"
#include "weather.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "wifi_sta.h"
#include "net_gate.h"

static const char *TAG = "weather";

#define WX_URL          "https://wttr.in/?format=j1"
#define WX_RX_MAX       (48 * 1024)     /* j1 响应上限（实测 28KB+，留足余量） */
#define WX_OK_PERIOD_MS (30 * 60 * 1000)
#define WX_FAIL_PERIOD_MS (5 * 60 * 1000)
#define WX_TIMEOUT_MS   10000

static SemaphoreHandle_t s_lock;
static wx_data_t  s_snap;
static char      *s_rx;          /* PSRAM 收包缓冲（懒分配） */
static int        s_rx_len;
static bool       s_force;       /* 手动刷新标志 */

/* ---------------- HTTP 收包 ---------------- */

static esp_err_t on_http(esp_http_client_event_t *e)
{
    /* 11-20 诊断：打事件阶段（定位卡在 DNS / TCP / TLS / 数据） */
    switch (e->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:    ESP_LOGI(TAG, "http: connected"); break;
    case HTTP_EVENT_ON_HEADER:       ESP_LOGI(TAG, "http: hdr %s: %.40s", e->header_key, e->header_value); break;
    case HTTP_EVENT_ON_FINISH:       ESP_LOGI(TAG, "http: finish"); break;
    case HTTP_EVENT_DISCONNECTED:    ESP_LOGI(TAG, "http: disconnected"); break;
    case HTTP_EVENT_ERROR:           ESP_LOGW(TAG, "http: error"); break;
    default: break;
    }
    if (e->event_id == HTTP_EVENT_ON_DATA)
    {
        int room = WX_RX_MAX - 1 - s_rx_len;
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

/* ---------------- 手写 JSON 字段提取（零依赖） ----------------
 * wttr.in j1 响应结构固定，按"第 n 次出现"取值即可，比引 cJSON 更省心
 * （IDF 6.1 已移除 cJSON 组件）。字符串值按 UTF-8 原样（lang=zh）。 */

/* 找第 nth 次出现 key 后的 "值"（数字或字符串均可；字符串去引号） */
static bool jfind_nth(const char *json, const char *key, int nth,
                      char *out, size_t osz)
{
    const char *p = json;
    int cnt = 0;
    size_t klen = strlen(key);
    while ((p = strstr(p, key)) != NULL)
    {
        p += klen;
        if (++cnt < nth)
        {
            continue;
        }
        /* 跳到冒号后的值起点 */
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r')
        {
            p++;
        }
        if (*p == '"')
        {
            p++;
            const char *e = strchr(p, '"');
            if (e == NULL)
            {
                return false;
            }
            size_t n = (size_t)(e - p);
            if (n >= osz)
            {
                n = osz - 1;
            }
            memcpy(out, p, n);
            out[n] = 0;
            return true;
        }
        /* 数字/裸值：读到逗号或 } 为止 */
        const char *e = p;
        while (*e && *e != ',' && *e != '}' && *e != '\n')
        {
            e++;
        }
        size_t n = (size_t)(e - p);
        if (n >= osz)
        {
            n = osz - 1;
        }
        memcpy(out, p, n);
        out[n] = 0;
        /* 去尾部空白 */
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r'))
        {
            out[--n] = 0;
        }
        return true;
    }
    return false;
}

static int jfind_int(const char *json, const char *key, int nth, int def)
{
    char b[24];
    if (!jfind_nth(json, key, nth, b, sizeof(b)))
    {
        return def;
    }
    return atoi(b);
}

/* anchor 之后第一个 key 的字符串值（如 "weatherDesc" 后的第一个 "value"） */
static bool jafter(const char *json, const char *anchor, const char *key,
                   char *out, size_t osz)
{
    const char *a = strstr(json, anchor);
    if (a == NULL)
    {
        return false;
    }
    return jfind_nth(a, key, 1, out, osz);
}

/* ---------------- 一次拉取 ---------------- */

/* 城市 → 坐标（wttr.in 给英文城市名；未匹配一律按北京） */
static const struct
{
    const char *city;
    float lat, lon;
} s_geo[] = {
    { "Beijing",   39.9042f, 116.4074f },
    { "Shanghai",  31.2304f, 121.4737f },
    { "Shenzhen",  22.5431f, 114.0579f },
    { "Guangzhou", 23.1291f, 113.2644f },
    { "Hangzhou",  30.2741f, 120.1551f },
    { "Chengdu",   30.5728f, 104.0668f },
    { "Wuhan",     30.5928f, 114.3055f },
    { "Xi'an",     34.3416f, 108.9398f },
    { "Xian",      34.3416f, 108.9398f },
    { "Nanjing",   32.0603f, 118.7969f },
    { "Chongqing", 29.5630f, 106.5516f },
    { "Tianjin",   39.3434f, 117.3616f },
    { "Suzhou",    31.2989f, 120.5853f },
};

/* 抓空气质量（open-meteo，免 key，实测 200/1.9s）。失败只影响 AQI，不动天气主数据 */
static void fetch_aqi(wx_data_t *out)
{
    out->aqi      = -1;
    out->pm25_x10 = -1;

    float lat = 39.9042f, lon = 116.4074f;
    for (size_t i = 0; i < sizeof(s_geo) / sizeof(s_geo[0]); i++)
    {
        if (strcasecmp(out->city, s_geo[i].city) == 0)
        {
            lat = s_geo[i].lat;
            lon = s_geo[i].lon;
            break;
        }
    }

    char url[192];
    snprintf(url, sizeof(url),
             "https://air-quality-api.open-meteo.com/v1/air-quality"
             "?latitude=%.4f&longitude=%.4f&current=pm2_5,us_aqi", (double)lat, (double)lon);

    s_rx_len = 0;
    s_rx[0]  = 0;

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = on_http,
        .timeout_ms        = WX_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = "s31-weather/1.0",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL)
    {
        return;
    }
    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || code != 200 || s_rx_len < 40)
    {
        ESP_LOGW(TAG, "aqi fetch failed: %s http %d", esp_err_to_name(err), code);
        return;
    }

    /* 响应里 us_aqi / pm2_5 各出现两次：current_units（字符串）在前、current（数字）在后 */
    const char *k = strstr(s_rx, "\"us_aqi\":");
    if (k != NULL)
    {
        k = strstr(k + 1, "\"us_aqi\":");
    }
    out->aqi = (k != NULL) ? atoi(k + 9) : -1;
    if (out->aqi <= 0)
    {
        out->aqi = -1;
    }

    const char *m = strstr(s_rx, "\"pm2_5\":");
    if (m != NULL)
    {
        m = strstr(m + 1, "\"pm2_5\":");
    }
    out->pm25_x10 = (m != NULL) ? (int)(atof(m + 8) * 10.0f + 0.5f) : -1;

    ESP_LOGI(TAG, "AQI %d, PM2.5 %d.%d (%.3f,%.3f)", out->aqi,
             out->pm25_x10 / 10, out->pm25_x10 % 10, (double)lat, (double)lon);
}

static bool fetch_once(wx_data_t *out)
{
    if (s_rx == NULL)
    {
        s_rx = heap_caps_malloc(WX_RX_MAX, MALLOC_CAP_SPIRAM);
        if (s_rx == NULL)
        {
            s_rx = heap_caps_malloc(WX_RX_MAX, MALLOC_CAP_DEFAULT);
        }
    }
    if (s_rx == NULL)
    {
        ESP_LOGE(TAG, "no memory for rx buffer");
        return false;
    }

    ESP_LOGI(TAG, "fetching %s ...", WX_URL);
    s_rx_len = 0;
    s_rx[0]  = 0;

    esp_http_client_config_t cfg = {
        .url               = WX_URL,
        .event_handler     = on_http,
        .timeout_ms        = WX_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent        = "s31-weather/1.0",
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
    if (err != ESP_OK || code != 200 || s_rx_len < 100)
    {
        ESP_LOGW(TAG, "fetch failed: %s http %d (rx %d)",
                 esp_err_to_name(err), code, s_rx_len);
        return false;
    }

    ESP_LOGI(TAG, "rx %d bytes, head: %.180s", s_rx_len, s_rx);

    /* ---- 手写字段提取（顺序：current → nearest_area → weather[0]/[1]） ---- */
    bool ok = false;
    {
        char b[48];

        if (jfind_int(s_rx, "\"temp_C\"", 1, -99) != -99)
        {
            out->temp_c   = jfind_int(s_rx, "\"temp_C\"", 1, -99);
            out->feels_c  = jfind_int(s_rx, "\"FeelsLikeC\"", 1, -99);
            out->humidity = jfind_int(s_rx, "\"humidity\"", 1, -1);
            out->wind_kmph = jfind_int(s_rx, "\"windspeedKmph\"", 1, -1);

            if (jfind_nth(s_rx, "\"winddir16Point\"", 1, b, sizeof(b)))
            {
                snprintf(out->wind_dir, sizeof(out->wind_dir), "%.*s", (int)(sizeof(out->wind_dir) - 1), b);
            }
            /* 中文描述：weatherDesc 后第一个 value */
            if (jafter(s_rx, "\"weatherDesc\"", "\"value\"", b, sizeof(b)))
            {
                snprintf(out->desc, sizeof(out->desc), "%.*s", (int)(sizeof(out->desc) - 1), b);
            }

            /* observation_time "04:21 AM"（UTC）→ 转用户时区（UTC+8）显示。
             *   ★ 必须 +8：wttr.in 是英国服务，观测时间给 UTC——不转换用户会把它
             *   读成本地时间，"凌晨 04:21"与当前差 9 小时的困惑即源于此。 */
            if (jfind_nth(s_rx, "\"observation_time\"", 1, b, sizeof(b)))
            {
                int hh = 0, mm = 0;
                char ap[4] = {0};
                if (sscanf(b, "%d:%d %2s", &hh, &mm, ap) == 3)
                {
                    if ((ap[0] == 'P' || ap[0] == 'p') && hh < 12) { hh += 12; }
                    if ((ap[0] == 'A' || ap[0] == 'a') && hh == 12) { hh = 0; }
                    hh += 8;                      /* UTC → CST-8 */
                    if (hh >= 24) { hh -= 24; }
                    char tmp[24];   /* GCC 的 format-truncation 只看 int 理论宽度；
                                     * hh/mm 已是 %02d 语义（0-23/0-59），24B 装得下任何 int 对 */
                    snprintf(tmp, sizeof(tmp), "%02d:%02d", hh, mm);
                    strlcpy(out->updated, tmp, sizeof(out->updated));
                }
            }

            /* 城市/省份：nearest_area 后第一个 areaName / region 的 value */
            if (jafter(s_rx, "\"nearest_area\"", "\"areaName\"", b, sizeof(b)) ||
                jfind_nth(s_rx, "\"areaName\"", 1, b, sizeof(b)))
            {
                /* areaName 是对象数组，需再取一层 value */
                const char *an = strstr(s_rx, "\"areaName\"");
                if (an != NULL && jfind_nth(an, "\"value\"", 1, b, sizeof(b)))
                {
                    snprintf(out->city, sizeof(out->city), "%.*s", (int)(sizeof(out->city) - 1), b);
                }
            }
            /* region 同理 */
            {
                const char *rg = strstr(s_rx, "\"region\"");
                if (rg != NULL && jfind_nth(rg, "\"value\"", 1, b, sizeof(b)))
                {
                    snprintf(out->region, sizeof(out->region), "%.*s", (int)(sizeof(out->region) - 1), b);
                }
            }

            /* 今天 / 明天高低温：maxtempC 第 1、2 次出现；mintempC 同理 */
            out->today_max = jfind_int(s_rx, "\"maxtempC\"", 1, -99);
            out->today_min = jfind_int(s_rx, "\"mintempC\"", 1, -99);
            out->tmrw_max  = jfind_int(s_rx, "\"maxtempC\"", 2, -99);
            out->tmrw_min  = jfind_int(s_rx, "\"mintempC\"", 2, -99);

            /* 今天逐 3 小时温度：hourly[].tempC 的前 8 个
             * ★ 注意搜 "tempC"（带前引号）——current 的 key 是 temp_C（下划线），
             *   "maxtempC" 前引号位置不同也不会误匹配。 */
            for (int hi = 0; hi < 8; hi++)
            {
                out->hourly_t[hi] = jfind_int(s_rx, "\"tempC\"", hi + 1, -99);
            }

            ok = true;
        }
        if (!ok)
        {
            ESP_LOGW(TAG, "parse: temp_C not found");
        }
    }

    if (ok)
    {
        fetch_aqi(out);       /* 失败不影响天气主数据 */
    }
    return ok;
}

/* ---------------- 刷新任务 ---------------- */

static void weather_task(void *arg)
{
    (void)arg;

    /* 等 WiFi 首次连上（最多等 60 秒再试，失败会重试） */
    while (!wifi_sta_connected())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));   /* 让 SNTP/DNS 就绪 */

    while (1)
    {
        /* 置"拉取中"（保留旧数据字段，UI 可继续显示旧值 + 转圈） */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap.state = WX_STATE_LOADING;
        xSemaphoreGive(s_lock);

        wx_data_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        snprintf(tmp.city, sizeof(tmp.city), "%.*s", (int)sizeof(tmp.city) - 1, s_snap.city);   /* 失败时保留城市名 */
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
            tmp.state = WX_STATE_OK;
            s_snap = tmp;
        }
        else
        {
            s_snap.state = WX_STATE_FAIL;   /* 其余字段保留上次成功值 */
        }
        xSemaphoreGive(s_lock);

        /* ★★ 写缓存必须在锁外、且在"界面更新之前"这个间隙：
         *   写 flash 会禁 i-cache ⇒ LVGL 任务 stall ⇒ flush 等 vsync 超时 ⇒ 撕裂。
         *   此刻界面显示的还是上一份数据（静止的），写它看不出来。 */
        if (ok)
        {
            data_cache_save("wx", &tmp, sizeof(tmp));
        }

        int wait_ms = ok ? WX_OK_PERIOD_MS : WX_FAIL_PERIOD_MS;
        /* 切成 1 秒一片地等，便于手动刷新插队 */
        s_force = false;
        for (int waited = 0; waited < wait_ms; waited += 1000)
        {
            if (s_force)
            {
                break;
            }
            /* WiFi 掉线时不必空等满周期：每 30 秒探一次 */
            if ((waited % 30000) == 0 && !wifi_sta_connected())
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

/* ---------------- 对外 ---------------- */

void weather_start(void)
{
    if (s_lock != NULL)
    {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = WX_STATE_INIT;
    snprintf(s_snap.city, sizeof(s_snap.city), "定位中...");

    /* ★★ 先读上次缓存：断网、或刚开机 WiFi 还没连上时，界面立刻就有内容，
     *   不用空等第一次 HTTPS。拉到新数据后自然覆盖。 */
    if (data_cache_load("wx", &s_snap, sizeof(s_snap)) == ESP_OK)
    {
        /* 缓存里的 state 可能是 FAIL/LOADING，强制当"有数据"用，
         * 否则 UI 会显示"加载中"而不是上次的温度。 */
        s_snap.state = WX_STATE_OK;
        ESP_LOGI(TAG, "cache hit: %dC %s (updated %s)", s_snap.temp_c, s_snap.desc, s_snap.updated);
    }

    BaseType_t ok = xTaskCreate(weather_task, "weather", 10240, NULL, 4, NULL);
    if (ok == pdPASS)
    {
        ESP_LOGI(TAG, "weather task started (wttr.in, ip locate)");
    }
    else
    {
        ESP_LOGE(TAG, "task create failed");
    }
}

void weather_refresh_now(void)
{
    s_force = true;
}

void weather_get(wx_data_t *out)
{
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->state = WX_STATE_INIT;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}
