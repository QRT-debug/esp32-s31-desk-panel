/**
 * @file    data_cache.c
 * @brief   数据落盘缓存实现（NVS blob）
 */
#include "data_cache.h"

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
