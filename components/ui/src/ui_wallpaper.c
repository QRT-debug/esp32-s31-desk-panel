/**
 * @file    ui_wallpaper.c
 * @brief   壁纸背景实现
 *
 * 设备端：/sdcard/wallpaper 里随机挑一张 800×480 JPEG → 硬件解码 → PSRAM RGB565 → lv_image
 * 模拟器：PC 上没硬件 JPEG 解码器，直接返回 false（调用方用纯色背景）
 *
 * 关键约束（全部来自 scene_clock.c 的实战教训）：
 *   1. 输入缓冲必须 jpeg_alloc_decoder_mem 分配 —— 2D-DMA 要求 16 字节对齐，
 *      普通 heap_caps_malloc 不保证 ⇒ process 直接 ESP_ERR_INVALID_ARG
 *   2. 解码后**不要**做 cache msync —— 硬件保证一致性，手动 invalidate 768KB
 *      会和 LVGL 的 cache writeback 在双核上死锁（"点 CLOCK 卡死"的真凶）
 *   3. 尺寸必须正好 800×480，否则跳过（避免把任意图片拉伸变形）
 */
#include "ui_wallpaper.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#endif

#define WP_DIR  "/sdcard/wallpaper"
#define WP_W    800
#define WP_H    480
#define WP_MAX  12
#define TAG     "wallpaper"

#ifdef ESP_PLATFORM

static uint8_t       *s_pix;      /* PSRAM: 800*480*2，复用以避免每次重分配 */
static lv_image_dsc_t s_dsc;
static jpeg_decoder_handle_t s_dec;   /* 解码引擎只建一次，跨场景复用 */

/** 从目录随机挑一张 .jpg；没有返回 false */
static bool pick(char *out, size_t outsz)
{
    DIR *d = opendir(WP_DIR);
    if (d == NULL)
    {
        return false;
    }
    char names[WP_MAX][40];
    int  n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < WP_MAX)
    {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".jpg") == 0)
        {
            snprintf(names[n], sizeof(names[n]), "%s", e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n == 0)
    {
        return false;
    }
    /* 用硬件随机数：rand() 每次进页面都从同一 seed 起，连续两次会抽到同一张 */
    snprintf(out, outsz, "%s", names[(unsigned)(esp_random() % (unsigned)n)]);
    return true;
}

bool wallpaper_apply_bg(lv_obj_t *parent)
{
    char name[40];
    if (!pick(name, sizeof(name)))
    {
        ESP_LOGW(TAG, "没有 .jpg in " WP_DIR);
        return false;
    }

    char path[96];
    snprintf(path, sizeof(path), WP_DIR "/%s", name);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (512 * 1024)) { fclose(f); return false; }
    rewind(f);

    jpeg_decode_memory_alloc_cfg_t icfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t jalloc = 0;
    uint8_t *jpeg = jpeg_alloc_decoder_mem((size_t)sz, &icfg, &jalloc);
    if (jpeg == NULL) { fclose(f); return false; }

    size_t rd = fread(jpeg, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { heap_caps_free(jpeg); return false; }

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpeg, (uint32_t)sz, &info) != ESP_OK)
    {
        heap_caps_free(jpeg);
        return false;
    }
    if (info.width != WP_W || info.height != WP_H)
    {
        ESP_LOGW(TAG, "%s: %ux%u，不是 800x480，跳过", name,
                 (unsigned)info.width, (unsigned)info.height);
        heap_caps_free(jpeg);
        return false;
    }

    if (s_pix == NULL)
    {
        jpeg_decode_memory_alloc_cfg_t ocfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        size_t oalloc = 0;
        s_pix = jpeg_alloc_decoder_mem((size_t)WP_W * WP_H * 2, &ocfg, &oalloc);
        if (s_pix == NULL) { heap_caps_free(jpeg); return false; }
    }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* 和 LCD 的 RGB565 一致 */
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    /* 引擎只建一次（跨场景复用）。参数照 scene_clock.c 的实测值。 */
    if (s_dec == NULL)
    {
        jpeg_decode_engine_cfg_t ecfg = {
            .intr_priority = 0,
            .timeout_ms    = 2000,
        };
        if (jpeg_new_decoder_engine(&ecfg, &s_dec) != ESP_OK)
        {
            ESP_LOGE(TAG, "jpeg engine 初始化失败");
            s_dec = NULL;
            heap_caps_free(jpeg);
            return false;
        }
    }

    uint32_t out_sz = 0;
    esp_err_t err = jpeg_decoder_process(s_dec, &dcfg, jpeg, (uint32_t)sz,
                                        s_pix, (uint32_t)WP_W * WP_H * 2, &out_sz);
    heap_caps_free(jpeg);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "解码失败 %s: %s", name, esp_err_to_name(err));
        return false;
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w      = WP_W;
    s_dsc.header.h      = WP_H;
    s_dsc.data          = s_pix;
    s_dsc.data_size     = out_sz;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &s_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* 压到最底层，图标/文字盖在上面 */
    lv_obj_move_background(img);

    ESP_LOGI(TAG, "壁纸: %s (%u B)", name, (unsigned)out_sz);
    return true;
}

#else   /* ---------------- PC 模拟器 ---------------- */

bool wallpaper_apply_bg(lv_obj_t *parent)
{
    (void)parent;   /* PC 上没有硬件 JPEG 解码器，调用方回退到纯色背景 */
    return false;
}

#endif
