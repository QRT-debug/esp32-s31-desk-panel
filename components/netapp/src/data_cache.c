/**
 * @file    data_cache.c
 * @brief   数据落盘缓存实现（NVS blob）
 */
#include "data_cache.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cache";

#define CACHE_NS   "cache"

esp_err_t data_cache_save(const char *key, const void *data, size_t len)
{
    if (key == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(CACHE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "open(rw) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ★★★ 2026-11-25：内容没变就**不写**。
     *
     * 为什么必须跳过：开机 5.5 s 四个源各自从 NVS 读出缓存，9~12 s WiFi 回来后
     * 四个 HTTP 请求陆续返回，紧接着各自 nvs_set_blob + nvs_commit。
     * 而 nvs_commit() 写 flash 期间会**禁用 instruction cache**，LVGL 的渲染代码
     * 就住在 flash 里 ⇒ 任务 stall ⇒ flush_cb 错过 vsync ⇒ **开机那几秒画面抖一下**。
     * （现象实测：烧录后首次启动有明显频闪，之后完全正常；这次热启动 vsync timeout = 0）
     *
     * 热启动时新拉到的数据通常和缓存一致，跳过写入即可完全不碰 flash。
     * 语义是安全的：先比长度，再逐字节比；只有完全相同才跳过。 */
    size_t old_len = 0;
    if (nvs_get_blob(h, key, NULL, &old_len) == ESP_OK && old_len == len)
    {
        void *old = malloc(len);
        if (old != NULL)
        {
            size_t got = len;
            bool   same = (nvs_get_blob(h, key, old, &got) == ESP_OK) &&
                          (got == len) && (memcmp(old, data, len) == 0);
            free(old);
            if (same)
            {
                nvs_close(h);
                ESP_LOGI(TAG, "unchanged \"%s\" (%u B), skip", key, (unsigned)len);
                return ESP_OK;      /* 一字未改，不产生 flash 写 */
            }
        }
    }

    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "save \"%s\" failed: %s", key, esp_err_to_name(err));
    }
    else
    {
        ESP_LOGD(TAG, "saved \"%s\" (%u B)", key, (unsigned)len);
    }
    return err;
}

esp_err_t data_cache_load(const char *key, void *data, size_t len)
{
    if (key == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(CACHE_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    size_t    got = len;
    esp_err_t r   = nvs_get_blob(h, key, data, &got);
    nvs_close(h);

    if (r == ESP_ERR_NVS_NOT_FOUND)
    {
        return r;                        /* 第一次开机，很正常，不报警 */
    }
    if (r != ESP_OK)
    {
        ESP_LOGW(TAG, "load \"%s\" failed: %s", key, esp_err_to_name(r));
        return r;
    }
    if (got != len)
    {
        /* 结构体改过了：当作没有缓存，调用方用默认值 */
        ESP_LOGW(TAG, "\"%s\" size mismatch (%u != %u), ignored",
                 key, (unsigned)got, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "loaded \"%s\" (%u B)", key, (unsigned)len);
    return ESP_OK;
}

esp_err_t data_cache_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(CACHE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
