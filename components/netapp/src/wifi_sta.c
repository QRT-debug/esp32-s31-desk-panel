/**
 * @file    wifi_sta.c
 * @brief   WiFi STA 连接实现
 */
#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"

#include <stdlib.h>
#include <time.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "wifi_config.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_eg   = NULL;
static esp_netif_t       *s_netif = NULL;
static const int          BIT_IP = BIT0;
static volatile bool       s_user_off;   /* true = 用户主动断开，不自动重连 */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            xEventGroupClearBits(s_eg, BIT_IP);
            /* 自动重连：不改这里的话，路由器重启后要手动复位板子 */
            if (s_user_off)
            {
                ESP_LOGW(TAG, "disconnected by user (reason %d), stay offline", e->reason);
            }
            else
            {
                ESP_LOGW(TAG, "disconnected (reason %d), reconnecting...", e->reason);
                esp_wifi_connect();
            }
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, BIT_IP);

        /* 首次联网后启动 SNTP（东八区），供状态栏显示日期/时间 */
        static bool sntp_started;
        if (!sntp_started)
        {
            sntp_started = true;
            setenv("TZ", "CST-8", 1);
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com, TZ=CST-8)");
        }
    }
}

esp_err_t wifi_sta_init(void)
{
    s_eg = xEventGroupCreate();
    if (s_eg == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ★★ 11-21 实测：**不要把 WiFi 功率调低**。
     *   降到 8.5dBm 后屏幕闪烁**更严重**——因为信号弱→重传增多→WiFi DMA 更频繁，
     *   与 RGB LCD 抢 PSRAM 带宽更凶，LCD 扫描丢像素 → 四周随机闪。
     *   结论：闪的根因是 PSRAM 带宽仲裁，不是电源纹波。保持默认 20dBm。 */

    ESP_LOGI(TAG, "connecting to \"%s\" ...", WIFI_SSID);
    return ESP_OK;
}

bool wifi_sta_connected(void)
{
    if (s_eg == NULL)
    {
        return false;
    }
    return (xEventGroupGetBits(s_eg) & BIT_IP) != 0;
}

bool wifi_sta_wait_connected(int timeout_ms)
{
    if (s_eg == NULL)
    {
        return false;
    }
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_IP, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_IP) != 0;
}

void wifi_sta_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }
    buf[0] = 0;

    if (s_netif == NULL || !wifi_sta_connected())
    {
        strncpy(buf, "0.0.0.0", len - 1);
        return;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK)
    {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    }
}

int wifi_sta_rssi(void)
{
    if (!wifi_sta_connected())
    {
        return 0;
    }
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK)
    {
        return 0;
    }
    return rssi;
}

esp_err_t wifi_sta_disconnect_user(void)
{
    s_user_off = true;
    esp_err_t err = esp_wifi_disconnect();
    ESP_LOGW(TAG, "user requested disconnect (%s)", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_sta_reconnect_user(void)
{
    s_user_off = false;
    esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "user requested reconnect (%s)", esp_err_to_name(err));
    return err;
}
