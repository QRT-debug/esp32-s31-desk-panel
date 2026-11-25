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
#include "app_cfg.h"
#include "data_cache.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "weather.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "zlib.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "wifi_sta.h"
#include "net_gate.h"

static const char *TAG = "weather";

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

/* ---------------- 和风天气（QWeather）----------------
 * ★ 换源原因：wttr.in 免费接口观测数据滞后 ~1.5h（服务端缓存），且 observation_time
 *   是 UTC 不转时区。和风免费档 1000 次/天（30min 一刷只用 5%），obsTime 自带本地时区，
 *   描述直接中文（"晴"）。API KEY 硬编码（家庭设备个人 key），挪 app_cfg 再议。 */
#define QW_KEY       "800f00f999c047a8b94036a1562b30d5"
/* ★ 2025+ 注册的账号必须用控制台分配的专属 API Host（旧 devapi 域名返回空体）；
 *   地理接口路径也改为 /geo/v2/... */
#define QW_HOST      "https://pj2yw3mtxf.re.qweatherapi.com"
#define QW_NOW_FMT   QW_HOST "/v7/weather/now?location=%s&key=" QW_KEY "&lang=zh"
#define QW_3D_FMT    QW_HOST "/v7/weather/3d?location=%s&key=" QW_KEY "&lang=zh"
#define QW_24H_FMT   QW_HOST "/v7/weather/24h?location=%s&key=" QW_KEY "&lang=zh"
#define QW_GEO_FMT   QW_HOST "/geo/v2/city/lookup?location=%s&key=" QW_KEY "&lang=zh"

static void url_encode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++)
    {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
        {
            out[o++] = (char)c;
        }
        else
        {
            snprintf(out + o, n - o, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = 0;
}

/* 城市 LocationID 缓存（NVS ns=cache key=qloc）——GeoAPI 查一次永久复用 */
static bool qw_loc_cached(char *id, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("cache", NVS_READONLY, &h) != ESP_OK)
    {
        return false;
    }
    size_t len = n;
    esp_err_t e = nvs_get_str(h, "qloc", id, &len);
    nvs_close(h);
    return (e == ESP_OK && id[0] != 0);
}

static void qw_loc_save(const char *id)
{
    nvs_handle_t h;
    if (nvs_open("cache", NVS_READWRITE, &h) != ESP_OK)
    {
        return;
    }
    nvs_set_str(h, "qloc", id);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "location id cached: %s", id);
}

/* 通用 GET：复用 s_rx 缓冲与 on_http 事件回调；和风成功响应必含 "code":"200" */
static bool http_get(const char *url)
{
    ESP_LOGI(TAG, "GET %s", url);
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
        return false;
    }
    /* ★★ 和风服务器无条件返回 gzip 压缩响应，esp_http_client 不自动解压 ⇒
     *   s_rx 里是二进制，JSON 解析全失败。声明不压缩： */
    esp_http_client_set_header(cli, "Accept-Encoding", "gzip;q=0, identity;q=1");

    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    /* ★★ 和风 CDN 无视 Accept-Encoding 强制 gzip（实测 identity 也被压缩）⇒
     *   s_rx 里是 gzip 二进制，JSON 解析全失败。识别 gzip magic (1f 8b) 后用
     *   zlib 的 gzip 模式（inflateInit2 windowBits=15+16）解压回 s_rx。 */
    if (s_rx_len > 16 && (unsigned char)s_rx[0] == 0x1F && (unsigned char)s_rx[1] == 0x8B)
    {
        uint8_t *outbuf = heap_caps_malloc(WX_RX_MAX, MALLOC_CAP_SPIRAM);
        if (outbuf == NULL)
        {
            ESP_LOGE(TAG, "no mem for gzip inflate");
            return false;
        }
        z_stream d = { 0 };
        int zr = inflateInit2(&d, 15 + 16);      /* 15=window, +16=识别 gzip 头 */
        if (zr == Z_OK)
        {
            d.next_in  = (Bytef *)s_rx;
            d.avail_in = (uInt)s_rx_len;
            d.next_out = outbuf;
            d.avail_out = WX_RX_MAX;
            zr = inflate(&d, Z_FINISH);
            inflateEnd(&d);
            if (zr == Z_STREAM_END && d.total_out > 0)
            {
                ESP_LOGI(TAG, "gzip inflated: %d -> %d bytes", s_rx_len, (int)d.total_out);
                memcpy(s_rx, outbuf, d.total_out);
                s_rx[d.total_out] = 0;
                s_rx_len = (int)d.total_out;
            }
            else
            {
                ESP_LOGW(TAG, "inflate failed zr=%d", zr);
                s_rx_len = 0;
            }
        }
        else
        {
            inflateEnd(&d);
        }
        heap_caps_free(outbuf);
    }

    bool ok = (err == ESP_OK && code == 200 && s_rx_len > 50 &&
               strstr(s_rx, "\"code\":\"200\"") != NULL);
    /* ★ 诊断：HTTP 状态与响应开头——和风失败时常见 401(key 无效)/403(未配权限)/空体 */
    ESP_LOGI(TAG, "http %d rx %d head: %.120s", code, s_rx_len, s_rx);
    return ok;
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

    char url[256];
    char loc[16];

    /* ① 城市 LocationID：NVS 缓存优先，没有则 GeoAPI 按城市名查一次 */
    if (!qw_loc_cached(loc, sizeof(loc)))
    {
        char enc[96];
        url_encode(app_cfg_get()->city, enc, sizeof(enc));
        snprintf(url, sizeof(url), QW_GEO_FMT, enc);
        if (!http_get(url))
        {
            ESP_LOGW(TAG, "geo lookup failed");
            return false;
        }
        if (jfind_nth(s_rx, "\"id\"", 1, loc, sizeof(loc)))
        {
            qw_loc_save(loc);
        }
        else
        {
            ESP_LOGW(TAG, "geo: no id in response");
            return false;
        }
    }

    bool ok = false;

    /* ② 实况 now：temp/text/feelsLike/humidity/windSpeed/windDir/obsTime */
    snprintf(url, sizeof(url), QW_NOW_FMT, loc);
    if (http_get(url))
    {
        out->temp_c    = jfind_int(s_rx, "\"temp\"", 1, -99);
        out->feels_c   = jfind_int(s_rx, "\"feelsLike\"", 1, -99);
        out->humidity  = jfind_int(s_rx, "\"humidity\"", 1, -1);
        out->wind_kmph = jfind_int(s_rx, "\"windSpeed\"", 1, -1);

        char b[48];
        if (jfind_nth(s_rx, "\"text\"", 1, b, sizeof(b)))
        {
            snprintf(out->desc, sizeof(out->desc), "%.*s",
                     (int)(sizeof(out->desc) - 1), b);
        }
        if (jfind_nth(s_rx, "\"windDir\"", 1, b, sizeof(b)))
        {
            snprintf(out->wind_dir, sizeof(out->wind_dir), "%.*s",
                     (int)(sizeof(out->wind_dir) - 1), b);
        }
        /* obsTime "2026-11-23T14:00+08:00" → "14:00"（和风 obsTime 自带本地时区） */
        {
            const char *ot = strstr(s_rx, "\"obsTime\":\"");
            if (ot != NULL)
            {
                /* ★ 必须从 ot+11（跳过 "obsTime":" 这 11 个字节）开始找 T，
                 *   否则会命中 "obsTime" 里 Time 的那个 T ⇒ 解析出 'ime":' 垃圾
                 *   （实测：Updated 行显示 "Updated ime":"）。 */
                const char *t = strchr(ot + 11, 'T');
                if (t != NULL)
                {
                    snprintf(out->updated, sizeof(out->updated), "%.5s", t + 1);
                }
            }
        }
        snprintf(out->city, sizeof(out->city), "%s", app_cfg_get()->city);
        ok = (out->temp_c != -99);
    }
    if (!ok)
    {
        ESP_LOGW(TAG, "now parse failed");
        return false;
    }

    /* ③ 今明高低温（3d：tempMax/tempMin 第 1、2 次出现 = 今天/明天） */
    snprintf(url, sizeof(url), QW_3D_FMT, loc);
    if (http_get(url))
    {
        out->today_max = jfind_int(s_rx, "\"tempMax\"", 1, -99);
        out->today_min = jfind_int(s_rx, "\"tempMin\"", 1, -99);
        out->tmrw_max  = jfind_int(s_rx, "\"tempMax\"", 2, -99);
        out->tmrw_min  = jfind_int(s_rx, "\"tempMin\"", 2, -99);
    }

    /* ④ 逐小时温度（24h：hourly[].temp 前 8 个） */
    snprintf(url, sizeof(url), QW_24H_FMT, loc);
    if (http_get(url))
    {
        for (int hi = 0; hi < 8; hi++)
        {
            out->hourly_t[hi] = jfind_int(s_rx, "\"temp\"", hi + 1, -99);
        }
    }

    fetch_aqi(out);       /* 失败不影响天气主数据 */
    return true;
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

        /* ★★ 写缓存放在锁外 —— 但"此刻界面静止、写了看不出来"这个假设**是错的**
         *   （2026-11-25 实测更正，原注释就写在这里）。
         *
         *   机制没错：写 flash 会禁 i-cache ⇒ LVGL 任务 stall ⇒ flush 等 vsync 超时 ⇒ 掉帧/撕裂。
         *   错的是前提：以为"界面显示的还是上一份数据"就等于"画面静止"。
         *   实际天气页自己在重绘 —— 实测开机后每 5 秒 10~11 帧、每帧脏区 7~10 块、
         *   写回 881~1092 KB，根本没有"静止间隙"可挑。
         *   ⇒ 真正的解法是 data_cache_save() 里的"内容没变就不写"（不碰 flash 才是根治）；
         *     "锁外"只是必要条件，不是充分条件。
         */
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
