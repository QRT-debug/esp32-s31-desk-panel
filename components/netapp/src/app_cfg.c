/**
 * @file    app_cfg.c
 * @brief   运行时配置实现：整个结构体作为一个 NVS blob 存取
 *
 * 为什么整体存 blob 而不是逐项存：
 *   配置项会随功能增长（以后还要加相册间隔、时钟样式…），逐项存就要维护一堆 key
 *   和"缺项怎么办"的分支；blob + 版本号只需在字段变动时 +1，旧数据自动回落默认值。
 */
#include "app_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_config.h"   /* ★ 编译期默认值（首次上电 / 恢复出厂时用） */

static const char *TAG = "appcfg";

#define CFG_NS    "appcfg"
#define CFG_KEY   "cfg"

static app_cfg_t s_cfg;
static bool      s_ready = false;

/** 用编译期宏填默认值 —— 这样"不配置"时的行为和改造前完全一致 */
static void cfg_defaults(app_cfg_t *c)
{
    memset(c, 0, sizeof(*c));

    c->ver = APP_CFG_VER;
    snprintf(c->wifi_ssid, sizeof(c->wifi_ssid), "%s", WIFI_SSID);
    snprintf(c->wifi_pass, sizeof(c->wifi_pass), "%s", WIFI_PASS);
    c->city[0] = '\0';                                   /* 空 + 坐标 0 = 按 IP 定位 */
    c->lat       = 0.0f;
    c->lon       = 0.0f;
    snprintf(c->pc_host, sizeof(c->pc_host), "%s", PC_MONITOR_HOST);
    c->pc_port    = PC_MONITOR_PORT;
    c->backlight  = 75;
    c->tz         = 8;
    c->theme      = 0;

    /* 语音：默认走 PC 中转（key 不出 PC），云端直连留空 */
    c->asr_url[0]  = '\0';
    c->asr_key[0]  = '\0';
    c->vad_thresh  = 300;
    c->rec_max_sec = 10;
}

esp_err_t app_cfg_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(CFG_NS, NVS_READONLY, &h);

    if (err == ESP_OK)
    {
        size_t    len = sizeof(s_cfg);
        app_cfg_t tmp;
        err = nvs_get_blob(h, CFG_KEY, &tmp, &len);
        nvs_close(h);

        /* ★ 向后兼容：ver 相同但 blob 较旧（结构体追加过字段）时，
         *   先填默认值再按实际长度覆盖 —— 新字段保留默认，旧配置不丢。
         *   （旧逻辑要求 len == sizeof 完全相等，加字段就会整套回落默认值 = WiFi 要重配。） */
        if (err == ESP_OK && tmp.ver == APP_CFG_VER && len >= 8)
        {
            cfg_defaults(&s_cfg);
            memcpy(&s_cfg, &tmp, len < sizeof(s_cfg) ? len : sizeof(s_cfg));
            s_ready = true;
            ESP_LOGI(TAG, "loaded from NVS (blob %u B, struct %u B)",
                     (unsigned)len, (unsigned)sizeof(s_cfg));
            app_cfg_dump();
            return ESP_OK;
        }

        /* ★ 长度或版本不符 = 结构体改过了。不要试图逐字段迁移，
         *   直接回落默认值再存一遍（配置项都是"重填一次也不麻烦"的东西）。 */
        ESP_LOGW(TAG, "saved blob unusable (err=%s len=%u ver=%u), falling back to defaults",
                 esp_err_to_name(err), (unsigned)len,
                 (err == ESP_OK) ? (unsigned)tmp.ver : 0u);
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }

    cfg_defaults(&s_cfg);
    s_ready = true;
    ESP_LOGI(TAG, "first boot -> compile-time defaults");
    app_cfg_dump();
    return app_cfg_save();
}

const app_cfg_t *app_cfg_get(void)
{
    return &s_cfg;
}

app_cfg_t *app_cfg_mut(void)
{
    return &s_cfg;
}

esp_err_t app_cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open(rw) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(h, CFG_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_cfg_reset(void)
{
    cfg_defaults(&s_cfg);
    ESP_LOGW(TAG, "reset to defaults");
    return app_cfg_save();
}

void app_cfg_dump(void)
{
    /* 密码只报"有没有"，不进日志 —— 串口日志经常被贴到 issue 里 */
    ESP_LOGI(TAG, "ssid=\"%s\" pass=%s city=\"%s\" lat/lon=%.4f,%.4f pc=%s:%u bl=%u%% tz=%+d theme=%u",
             s_cfg.wifi_ssid,
             s_cfg.wifi_pass[0] ? "***" : "(empty)",
             s_cfg.city[0] ? s_cfg.city : "(auto by IP)",
             (double)s_cfg.lat, (double)s_cfg.lon,
             s_cfg.pc_host, (unsigned)s_cfg.pc_port,
             (unsigned)s_cfg.backlight, (int)s_cfg.tz, (unsigned)s_cfg.theme);
    ESP_LOGI(TAG, "voice: asr_url=\"%s\" key=%s vad=%u rec=%us",
             s_cfg.asr_url[0] ? s_cfg.asr_url : "(PC relay)",
             s_cfg.asr_key[0] ? "***" : "(empty)",
             (unsigned)s_cfg.vad_thresh, (unsigned)s_cfg.rec_max_sec);
}
