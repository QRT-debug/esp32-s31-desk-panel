/**
 * @file    sd_card.c
 * @brief   TF 卡挂载（SDMMC 4-bit）+ 根目录扫描日志
 *
 * 引脚来自原理图（high-res 核对）：D0=IO20 D1=IO21 D2=IO22 D3=IO23 CLK=IO24 CMD=IO25。
 * ★ 早期文档误记为 IO27~IO32（那是模组引脚号），已更正。
 */
#include "sd_card.h"

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_PIN_CLK 24
#define SD_PIN_CMD 25
#define SD_PIN_D0  20
#define SD_PIN_D1  21
#define SD_PIN_D2  22
#define SD_PIN_D3  23

static sdmmc_card_t *s_card;
static bool          s_ready;
static uint64_t      s_capacity;

static bool s_quiet;   /* true = 不打印挂载日志（轮询重试时用） */

static void sd_recovery_task(void *arg);
static void sd_start_recovery_task(void);

static esp_err_t sd_card_mount_inner(bool quiet)
{
    if (s_ready)
    {
        return ESP_OK;
    }
    s_quiet = quiet;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20MHz：先求稳，验证通过后再试 HIGHSPEED */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.d1    = SD_PIN_D1;
    slot.d2    = SD_PIN_D2;
    slot.d3    = SD_PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mcfg, &s_card);
    if (ret != ESP_OK)
    {
        if (!quiet)
        {
            ESP_LOGE(TAG, "mount failed (%s) — 卡没插好 / 格式不支持 / 引脚不对",
                     esp_err_to_name(ret));
        }
        s_quiet = false;
        return ret;
    }
    s_ready = true;
    sd_start_recovery_task();

    uint64_t total = 0, freemb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freemb) == ESP_OK)
    {
        s_capacity = total;
        if (!quiet)
        ESP_LOGI(TAG, "mounted: %.2f GB total, %.2f GB free  (SDMMC 4-bit, 20MHz)",
                 (double)total / (1024.0 * 1024 * 1024),
                 (double)freemb / (1024.0 * 1024 * 1024));
    }
    else
    {
        ESP_LOGW(TAG, "mounted but info unavailable");
    }

    s_quiet = false;
    if (quiet)
    {
        return ESP_OK;   /* 轮询重试成功：不打印、不扫描 */
    }

    /* 根目录扫描：确认 FATFS 能读到文件（最多列 20 个） */
    DIR *d = opendir("/sdcard");
    if (d == NULL)
    {
        ESP_LOGW(TAG, "opendir(/sdcard) failed");
        return ESP_OK;
    }

    struct dirent *e;
    int n = 0, shown = 0;
    while ((e = readdir(d)) != NULL)
    {
        n++;
        if (shown < 20 && e->d_name[0] != '.')
        {
            ESP_LOGI(TAG, "  [%d] %s", shown, e->d_name);
            shown++;
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "root entries: %d", n);

    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    sd_start_recovery_task();   /* 开机就启动：无卡时也会持续尝试 */
    return sd_card_mount_inner(false);
}

bool sd_card_ready(void)
{
    return s_ready;
}

/* 卡恢复任务：只在"未挂载"时尝试重挂。
 * ★ 放在独立任务里（低优先级）：挂载会阻塞数百 ms，若跑在 LVGL 定时器里会卡界面。
 *   这样可以把重试间隔压到 1.5 秒，插回卡后很快变绿。 */
static void sd_recovery_task(void *arg)
{
    (void)arg;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!s_ready)
        {
            sd_card_mount_inner(true);   /* 静默：失败不打印 */
        }
    }
}

static void sd_start_recovery_task(void)
{
    static bool started;
    if (!started)
    {
        started = true;
        xTaskCreate(sd_recovery_task, "sdrec", 4096, NULL, 1, NULL);
    }
}

bool sd_card_poll(void)
{
    if (s_ready && s_card != NULL)
    {
        /* 已挂载：读一次卡状态。卡被拔掉时会失败 ⇒ 自动卸载 */
        if (sdmmc_get_status(s_card) != ESP_OK)
        {
            ESP_LOGW(TAG, "card removed");
            esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            s_ready    = false;
            s_card     = NULL;
            s_capacity = 0;
        }
    }
    /* 未挂载时由 sd_recovery_task 负责重试（每 1.5 秒），这里不阻塞 */
    return s_ready;
}

uint64_t sd_card_capacity(void)
{
    return s_capacity;
}

bool sd_card_find_first_wav(char *name, size_t len)
{
    if (!s_ready || name == NULL || len == 0)
    {
        return false;
    }

    DIR *d = opendir("/sdcard");
    if (d == NULL)
    {
        return false;
    }

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        size_t n = strlen(e->d_name);
        if (n > 4 && n < len &&
            (e->d_name[n - 4] == '.') &&
            (e->d_name[n - 3] == 'w' || e->d_name[n - 3] == 'W') &&
            (e->d_name[n - 2] == 'a' || e->d_name[n - 2] == 'A') &&
            (e->d_name[n - 1] == 'v' || e->d_name[n - 1] == 'V'))
        {
            snprintf(name, len, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);

    if (found)
    {
        ESP_LOGI(TAG, "found wav: %s", name);
    }
    else
    {
        ESP_LOGW(TAG, "no .wav in /sdcard root");
    }
    return found;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int sd_card_scan_wav(char names[][64], int max)
{
    if (!s_ready || names == NULL || max <= 0)
    {
        return 0;
    }

    DIR *d = opendir("/sdcard");
    if (d == NULL)
    {
        return 0;
    }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max)
    {
        size_t len = strlen(e->d_name);
        if (len > 4 && len < 64 &&
            e->d_name[len - 4] == '.' &&
            (e->d_name[len - 3] == 'w' || e->d_name[len - 3] == 'W') &&
            (e->d_name[len - 2] == 'a' || e->d_name[len - 2] == 'A') &&
            (e->d_name[len - 1] == 'v' || e->d_name[len - 1] == 'V'))
        {
            snprintf(names[n], 64, "%s", e->d_name);
            n++;
        }
    }
    closedir(d);

    if (n > 1)
    {
        qsort(names, (size_t)n, 64, cmp_str);
    }
    ESP_LOGI(TAG, "scan: %d wav file(s)", n);
    return n;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_ready)
    {
        return ESP_OK;
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    if (ret == ESP_OK)
    {
        s_ready = false;
        s_card  = NULL;
        ESP_LOGW(TAG, "unmounted (card released)");
    }
    else
    {
        ESP_LOGE(TAG, "unmount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void *sd_card_handle(void)
{
    return s_card;
}
