/**
 * @file    pc_monitor.c
 * @brief   轮询电脑的 /api/stats，解析成快照
 */
#include "app_cfg.h"
#include "pc_monitor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_config.h"
#include "wifi_sta.h"

#define RX_BUF_SIZE 2048
#define POLL_MS     1000

static const char *TAG = "pcmon";

static char              s_url[128];
static pc_stats_t        s_snap;
static SemaphoreHandle_t s_lock = NULL;

static char s_rx[RX_BUF_SIZE];
static int  s_rx_len = 0;

/* ---------------- HTTP 收数据 ---------------- */

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0)
    {
        int room = RX_BUF_SIZE - s_rx_len - 1;
        int n    = (evt->data_len < room) ? evt->data_len : room;
        if (n > 0)
        {
            memcpy(s_rx + s_rx_len, evt->data, n);
            s_rx_len += n;
            s_rx[s_rx_len] = 0;
        }
    }
    return ESP_OK;
}

/* ---------------- key=value 解析 ----------------
 * ★ IDF 6.x 已不带 cJSON（components/json 不存在，全库搜不到 cJSON.h），
 *   所以电脑端额外提供一份 /api/stats.txt 纯文本，按行 "key=value" 解析。
 *   格式是我们自己定的、字段固定，不需要通用 JSON 解析器。
 */

static void copy_val(char *dst, size_t cap, const char *val)
{
    if (cap == 0)
    {
        return;
    }
    strncpy(dst, val, cap - 1);
    dst[cap - 1] = 0;

    /* 去掉行尾的 \r / \n / 空格 */
    size_t n = strlen(dst);
    while (n > 0 && (dst[n - 1] == '\r' || dst[n - 1] == '\n' || dst[n - 1] == ' '))
    {
        dst[--n] = 0;
    }
}

static void set_field(pc_stats_t *st, const char *key, const char *val)
{
    if (!strcmp(key, "host"))               copy_val(st->host, sizeof(st->host), val);
    else if (!strcmp(key, "uptime"))        st->uptime_s = atoi(val);
    else if (!strcmp(key, "cpu"))           st->cpu_pct = (float)atof(val);
    else if (!strcmp(key, "mhz"))           st->cpu_mhz = atoi(val);
    else if (!strcmp(key, "cores"))         st->cpu_cores = atoi(val);
    else if (!strcmp(key, "mem"))           st->mem_pct = (float)atof(val);
    else if (!strcmp(key, "mem_used"))      st->mem_used_gb = (float)atof(val);
    else if (!strcmp(key, "mem_total"))     st->mem_total_gb = (float)atof(val);
    else if (!strcmp(key, "net_up"))        st->net_up_kbps = (float)atof(val);
    else if (!strcmp(key, "net_down"))      st->net_down_kbps = (float)atof(val);
    else if (!strcmp(key, "gpu_ok"))        st->gpu_ok = atoi(val) != 0;
    else if (!strcmp(key, "gpu_name"))      copy_val(st->gpu_name, sizeof(st->gpu_name), val);
    else if (!strcmp(key, "gpu"))           st->gpu_pct = atoi(val);
    else if (!strcmp(key, "gpu_mem_pct"))   st->gpu_mem_pct = (float)atof(val);
    else if (!strcmp(key, "gpu_mem_used"))  st->gpu_mem_used_gb = (float)atof(val);
    else if (!strcmp(key, "gpu_mem_total")) st->gpu_mem_total_gb = (float)atof(val);
    else if (!strcmp(key, "gpu_temp"))      st->gpu_temp = atoi(val);
    else if (!strcmp(key, "gpu_power"))     st->gpu_power = (float)atof(val);
    else if (!strcmp(key, "disk_count"))
    {
        int n = atoi(val);
        if (n < 0)              n = 0;
        if (n > PC_MAX_DISKS)   n = PC_MAX_DISKS;
        st->disk_count = n;
    }
    else if (!strncmp(key, "disk", 4) && isdigit((unsigned char)key[4]))
    {
        int idx = key[4] - '0';
        if (idx < 0 || idx >= PC_MAX_DISKS)
        {
            return;
        }
        const char *f = key + 5;
        if (*f == '_')
        {
            f++;
        }
        pc_disk_t *d = &st->disks[idx];
        if (!strcmp(f, "name"))        copy_val(d->name, sizeof(d->name), val);
        else if (!strcmp(f, "pct"))    d->percent = (float)atof(val);
        else if (!strcmp(f, "free"))   d->free_gb = (float)atof(val);
        else if (!strcmp(f, "total"))  d->total_gb = (float)atof(val);
    }
}

static void parse_kv(pc_stats_t *st, const char *txt)
{
    const char *p = txt;
    char line[160];

    while (p && *p)
    {
        const char *nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);

        if (len > 0 && len < sizeof(line))
        {
            memcpy(line, p, len);
            line[len] = 0;

            char *eq = strchr(line, '=');
            if (eq && eq != line)
            {
                *eq = 0;
                set_field(st, line, eq + 1);
            }
        }

        if (nl == NULL)
        {
            break;
        }
        p = nl + 1;
    }
}

/* ---------------- 轮询任务 ---------------- */

static void monitor_task(void *arg)
{
    (void)arg;

    while (1)
    {
        if (!wifi_sta_connected())
        {
            /* ★ 11-20 修复：断开 WiFi 后 valid 停在 true，监控页永远 ONLINE。
             *   未连接即离线，立即清（拿锁改快照）。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snap.valid = false;
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_rx_len = 0;
        s_rx[0]  = 0;

        esp_http_client_config_t cfg = {
            .url               = s_url,
            .event_handler     = on_http,
            .timeout_ms        = 3000,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        esp_err_t err  = esp_http_client_perform(cli);
        int       code = esp_http_client_get_status_code(cli);

        if (err == ESP_OK && code == 200 && s_rx_len > 0)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            pc_stats_t *st = &s_snap;
            st->valid      = true;
            parse_kv(st, s_rx);
            st->fail_count = 0;
            st->last_ok_ms = esp_timer_get_time() / 1000;
            xSemaphoreGive(s_lock);
        }
        else
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_snap.fail_count++;
            /* ★ 连续 3 次（≈3s）拉不到 ⇒ 判离线（WiFi 在但 PC 端服务挂掉的场景） */
            if (s_snap.fail_count >= 3)
            {
                s_snap.valid = false;
            }
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG, "fetch failed: %s / http %d", esp_err_to_name(err), code);
        }

        esp_http_client_cleanup(cli);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* ---------------- 对外 ---------------- */

esp_err_t pc_monitor_start(void)
{
    if (s_lock != NULL)
    {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snap, 0, sizeof(s_snap));
    /* ★ 11-22：地址改为运行时配置（配网页可改），编译期宏只作默认值 */
    const app_cfg_t *cfg = app_cfg_get();
    snprintf(s_url, sizeof(s_url), "http://%s:%u/api/stats.txt",
             cfg->pc_host, (unsigned)cfg->pc_port);
    ESP_LOGI(TAG, "polling %s every %d ms", s_url, POLL_MS);

    BaseType_t ok = xTaskCreate(monitor_task, "pcmon", 8192, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void pc_monitor_get(pc_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }
    if (s_lock == NULL)
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}

const char *pc_monitor_url(void)
{
    return s_url;
}
