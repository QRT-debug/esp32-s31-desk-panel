/**
 * @file    scene_clock.c
 * @brief   屏保/大字时钟：SD 卡壁纸 + 96px 时间
 *
 * 数据流：/sdcard/wallpaper 目录里的 .jpg → 硬件 JPEG 解码 → PSRAM(RGB565 800×480)
 *        → lv_image_dsc_t → LVGL 显示。解码在 CPU 之外完成，几乎不占 MIPS。
 *
 * ★★ 为什么不用 LVGL 自带的解码器：LVGL 的 TJPGD/LODEPNG 全没开（开了要占 flash
 *    且是软件解码，800×480 要几百 ms）。S31 有硬件 JPEG（SOC_JPEG_DECODE_SUPPORTED=1），
 *    解码同样一张图只要几十 ms，CPU 还能去跑 LVGL。
 *
 * ★★ PSRAM cache：硬件解码器把像素直接写进 PSRAM，CPU 随后要读它
 *    ⇒ 必须 esp_cache_msync(INVALIDATE) 让 CPU 重新从 PSRAM 取，否则读到脏数据
 *    （表现为壁纸花屏/撕裂/上一张图的残留）。
 */
#include "ui_scenes.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "ui_fonts.h"
#include "ui_theme.h"

#define WP_DIR   "/sdcard/wallpaper"
#define WP_W     800
#define WP_H     480
#define WP_MAX   12          /* 最多认这么多张，多了也不影响 */

static const char *TAG = "clock";

static lv_obj_t       *s_lbl_time;
static lv_obj_t       *s_lbl_date;
static lv_obj_t       *s_lbl_hint;
static uint8_t        *s_pix;                 /* PSRAM: 800*480*2 */
static lv_image_dsc_t  s_dsc;
static jpeg_decoder_handle_t s_dec;
static int             s_last_min = -1;

/* ---------------- 壁纸 ---------------- */

/** 从目录里随机挑一张 jpg；没有返回 false */
static bool pick_wallpaper(char *out, size_t outsz)
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
    snprintf(out, outsz, "%s", names[(unsigned)rand() % (unsigned)n]);
    return true;
}

/** 读文件 → 硬件解码 → PSRAM。任何一步失败都返回 false（调用方用纯色背景） */
static bool load_wallpaper(void)
{
    char name[40];
    if (!pick_wallpaper(name, sizeof(name)))
    {
        ESP_LOGW(TAG, "no .jpg in " WP_DIR);
        return false;
    }

    char path[72];
    snprintf(path, sizeof(path), WP_DIR "/%s", name);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > (512 * 1024))
    {
        fclose(f);
        return false;
    }
    rewind(f);

    uint8_t *jpeg = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg == NULL)
    {
        fclose(f);
        return false;
    }
    size_t rd = fread(jpeg, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz)
    {
        heap_caps_free(jpeg);
        return false;
    }

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpeg, (uint32_t)sz, &info) != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: header parse failed", name);
        heap_caps_free(jpeg);
        return false;
    }
    if (info.width != WP_W || info.height != WP_H)
    {
        ESP_LOGW(TAG, "%s: %ux%u != 800x480, skipped", name,
                 (unsigned)info.width, (unsigned)info.height);
        heap_caps_free(jpeg);
        return false;
    }

    if (s_pix == NULL)
    {
        s_pix = heap_caps_malloc((size_t)WP_W * WP_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pix == NULL)
        {
            heap_caps_free(jpeg);
            return false;
        }
    }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* 与 LCD 的 RGB565 一致 */
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_sz = 0;
    esp_err_t err = jpeg_decoder_process(s_dec, &dcfg, jpeg, (uint32_t)sz,
                                        s_pix, (uint32_t)WP_W * WP_H * 2, &out_sz);
    heap_caps_free(jpeg);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "decode failed: %s", esp_err_to_name(err));
        return false;
    }

    /* ★ 硬件写进 PSRAM ⇒ CPU 侧 cache 是旧的，必须作废，否则读到脏数据 */
    esp_cache_msync(s_pix, (size_t)WP_W * WP_H * 2, ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    ESP_LOGI(TAG, "wallpaper: %s (%u bytes)", name, (unsigned)out_sz);
    return true;
}

/* ---------------- 场景 ---------------- */

static void update_clock(bool force)
{
    time_t    now = 0;
    struct tm tm_now;
    time(&now);
    localtime_r(&now, &tm_now);

    if (!force && tm_now.tm_min == s_last_min)
    {
        /* 分钟没变就只刷秒 */
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        lv_label_set_text(s_lbl_time, b);
        return;
    }

    s_last_min = tm_now.tm_min;

    char b[24];
    snprintf(b, sizeof(b), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    lv_label_set_text(s_lbl_time, b);

    static const char *wk[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    snprintf(b, sizeof(b), "%d月%d日 %s", tm_now.tm_mon + 1, tm_now.tm_mday, wk[tm_now.tm_wday % 7]);
    lv_label_set_text(s_lbl_date, b);
}

void scene_clock(uint16_t *fb, int frame)
{
    (void)fb;

    if (frame == 0)
    {
        /* 解码引擎只建一次，跨场景复用 */
        if (s_dec == NULL)
        {
            jpeg_decode_engine_cfg_t ecfg = {
                .intr_priority = 0,
                .timeout_ms    = 2000,
            };
            if (jpeg_new_decoder_engine(&ecfg, &s_dec) != ESP_OK)
            {
                ESP_LOGE(TAG, "jpeg engine init failed");
                s_dec = NULL;
            }
        }

        srand((unsigned)time(NULL));
        bool ok = (s_dec != NULL) && load_wallpaper();

        /* ---- 背景 ---- */
        if (ok)
        {
            s_dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
            s_dsc.header.w          = WP_W;
            s_dsc.header.h          = WP_H;
            s_dsc.header.stride     = WP_W * 2;
            s_dsc.data_size         = (uint32_t)WP_W * WP_H * 2;
            s_dsc.data              = s_pix;

            lv_obj_t *img = lv_image_create(lv_scr_act());
            lv_image_set_src(img, &s_dsc);
            lv_obj_set_pos(img, 0, 0);
        }
        else
        {
            lv_obj_t *bg = lv_obj_create(lv_scr_act());
            lv_obj_set_size(bg, WP_W, WP_H);
            lv_obj_set_pos(bg, 0, 0);
            lv_obj_set_style_bg_color(bg, lv_color_hex(0x0B1020), 0);
            lv_obj_set_style_border_width(bg, 0, 0);
            lv_obj_set_style_radius(bg, 0, 0);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
        }

        /* ---- 时间背后的胶囊遮罩：亮壁纸上白字才看得清 ---- */
        lv_obj_t *cap = lv_obj_create(lv_scr_act());
        lv_obj_set_size(cap, 430, 150);
        lv_obj_set_pos(cap, 20, 14);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_50, 0);
        lv_obj_set_style_border_width(cap, 0, 0);
        lv_obj_set_style_radius(cap, 18, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

        s_lbl_time = lv_label_create(cap);
        lv_obj_set_style_text_font(s_lbl_time, &ui_font_digit_96, 0);
        lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(s_lbl_time, "--:--:--");
        lv_obj_set_pos(s_lbl_time, 14, 8);

        s_lbl_date = lv_label_create(cap);
        lv_obj_set_style_text_font(s_lbl_date, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_date, lv_color_hex(0xD6E0F0), 0);
        lv_label_set_text(s_lbl_date, "");
        lv_obj_set_pos(s_lbl_date, 18, 112);

        s_lbl_hint = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_font(s_lbl_hint, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_opa(s_lbl_hint, LV_OPA_50, 0);
        lv_label_set_text(s_lbl_hint, "轻触屏幕返回");
        lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

        s_last_min = -1;
        update_clock(true);
    }

    update_clock(false);
}
