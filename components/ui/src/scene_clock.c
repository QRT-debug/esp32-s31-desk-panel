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
 *    ★ cache 一致性由 2D-DMA 硬件保证（官方示例解码后不做任何 msync）；
 *      软件再对大块做 INVALIDATE 反而会与 LVGL 渲染的 writeback 死锁（实测）。
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
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_theme.h"

#define WP_DIR   "/sdcard/wallpaper"
#define WP_W     800
#define WP_H     480
#define WP_MAX   12          /* 最多认这么多张，多了也不影响 */

static const char *TAG = "clock";

static lv_obj_t       *s_lbl_time;
static lv_obj_t       *s_lbl_date;
static uint8_t        *s_pix;                 /* PSRAM: 800*480*2 */
static lv_image_dsc_t  s_dsc;
static jpeg_decoder_handle_t s_dec;
static int             s_last_min = -1;
static volatile int    s_wp_ready = -1;   /* -1 解码中 / 0 失败 / 1 成功 */

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
        /* ★ 必须排除 desktop.jpg（2026-11-25 修）：它是 APPS 页专用的固定图，
         *   不排除的话时钟页会以 1/7 的概率"抽到桌面壁纸"，看起来像串页。
         *   注意这里与 ui_wallpaper.c 的 pick() 是两套独立实现，
         *   分流规则改动时**两边都要改** —— 当初就是只改了一边才漏的。 */
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".jpg") == 0 &&
            strcasecmp(e->d_name, "desktop.jpg") != 0)
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
    /* ★ 用硬件真随机，不用 rand()：srand(time(NULL)) 每次进场景重置序列，
     *   而 newlib 的 rand 对连续 seed 的第一个输出相关性很高 ⇒ 实测两次都抽到同一张。 */
    snprintf(out, outsz, "%s", names[(unsigned)(esp_random() % (unsigned)n)]);
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

    /* ★★★ 必须用 jpeg_alloc_decoder_mem，不能用普通 heap_caps_malloc：
     *   2D-DMA 要求缓冲 **16 字节对齐**（且 size 也要对齐），
     *   普通 malloc 不保证 ⇒ process 直接返回 ESP_ERR_INVALID_ARG（实测踩到）。
     *   这个函数会按 cache/2D-DMA 对齐取最大值，并分配在 PSRAM。 */
    jpeg_decode_memory_alloc_cfg_t icfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t jpeg_alloc = 0;
    uint8_t *jpeg = jpeg_alloc_decoder_mem((size_t)sz, &icfg, &jpeg_alloc);
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
        jpeg_decode_memory_alloc_cfg_t ocfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t oalloc = 0;
        s_pix = jpeg_alloc_decoder_mem((size_t)WP_W * WP_H * 2, &ocfg, &oalloc);
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

    /* ★ 官方示例（examples/peripherals/jpeg/jpeg_decode）解码后【不做】cache msync
     *   —— S31 的 2D-DMA 写 PSRAM 时硬件保证一致性。
     *   实测教训：在这里对 768KB 做 INVALIDATE 会与 LVGL 渲染任务的 cache writeback
     *   在双核上死锁（三次"点 CLOCK 卡死"的共同点，删掉即愈）。 */

    ESP_LOGI(TAG, "wallpaper: %s (%u bytes)", name, (unsigned)out_sz);
    return true;
}

/* ---------------- 场景 ---------------- */

/* ---- 解码放独立任务：★★ 绝不能在 LVGL 渲染循环里做 ----
 *   读 SD 卡 + 硬件解码 + 768KB cache invalidate 全在渲染线程同步执行的话，
 *   LVGL 主循环被冻住 → 看门狗复位（实测"点 CLOCK 界面卡死"就是这个）。
 *   正确姿势：任务里干重活，只写 volatile 标志；LVGL 定时器轮询、在主上下文更新界面。 */
static void wp_poll_cb(lv_timer_t *t)
{
    /* ★ LVGL 不是线程安全的：解码任务里绝不能直接碰任何 LVGL API
     *   （lv_async_call 也不行，它一样会进 LVGL 内部队列，从别的线程调就是死锁，
     *    实测"点 CLOCK 卡死在 System"就是它）。
     *   正确模式：任务只写 volatile 标志，LVGL 定时器在这里轮询、在主上下文更新。 */
    if (s_wp_ready < 0)
    {
        return;                       /* 还在解码 */
    }
    lv_timer_del(t);                  /* 只处理一次 */
    t = NULL;

    if (s_wp_ready == 0 || s_pix == NULL)
    {
        ESP_LOGW(TAG, "wallpaper unavailable, keep plain bg");
        return;
    }

    s_dsc.header.magic   = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf      = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w       = WP_W;
    s_dsc.header.h       = WP_H;
    s_dsc.header.stride  = WP_W * 2;
    s_dsc.data_size      = (uint32_t)WP_W * WP_H * 2;
    s_dsc.data           = s_pix;

    lv_obj_t *img = lv_image_create(lv_scr_act());
    lv_image_set_src(img, &s_dsc);
    lv_obj_set_pos(img, 0, 0);
    /* ★ 层级必须精确：img 是异步创建的，天然在最顶层会**盖住时间胶囊**；
     *   move_background 又会压到不透明 bg 的下面（壁纸整个被盖住，只剩灰屏）。
     *   正确位置 = index 1：bg(0) 之上、胶囊/时间之下。 */
    lv_obj_move_to_index(img, 1);
    ESP_LOGI(TAG, "wallpaper applied");
}

static void decode_task(void *arg)
{
    (void)arg;
    bool ok = load_wallpaper();
    s_wp_ready = ok ? 1 : 0;          /* ★ 只写标志，LVGL 侧轮询；绝不在任务里碰 LVGL */
    vTaskDelete(NULL);
}

static void clock_tap_cb(lv_event_t *e)
{
    (void)e;
    ui_statusbar_show(true);          /* 离开壁纸页恢复状态栏 */
    ui_goto_scene(g_clock_return_scene);
}

static void update_clock(bool force)
{
    time_t    now = 0;
    struct tm tm_now;
    time(&now);
    localtime_r(&now, &tm_now);

    /* ★ 临时心跳：每 5 秒打一条。用于定位"屏幕冻结但芯片活着"时 ui 循环死没死——
     *   如果日志停了 = ui_manager 卡死在场景代码里；一直打 = 循环活着，是渲染/flush 的问题。 */
    static int s_beat = 0;
    if (++s_beat % 5 == 0)
    {
        ESP_LOGI(TAG, "alive t=%02d:%02d:%02d frame_ok", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }

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
        /* ★★★ 本项目的场景切换惯例（ui_manager 不清对象，各场景自理）：
         *   frame==0 第一件事就是 lv_obj_clean 清掉上一页的所有对象。
         *   漏了这行的症状：新页对象全被旧页盖住 —— 看起来"卡死在旧页、点哪都没反应"，
         *   实际芯片/渲染一切正常（本场景第一次上线时栽在这）。 */
        lv_obj_clean(lv_scr_act());
        ui_statusbar_show(false);         /* ★ 壁纸页隐藏状态栏：时钟和它重复 */

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

        /* ---- 背景：先铺纯色兜底，壁纸异步解码成功后再叠上来 ---- */
        lv_obj_t *bg = lv_obj_create(lv_scr_act());
        lv_obj_set_size(bg, WP_W, WP_H);
        lv_obj_set_pos(bg, 0, 0);
        lv_obj_set_style_bg_color(bg, lv_color_hex(0x0B1020), 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_radius(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);

        s_wp_ready = -1;
        if (s_dec != NULL)
        {
            if (xTaskCreate(decode_task, "wpdec", 8192, NULL, 3, NULL) != pdPASS)
            {
                ESP_LOGW(TAG, "decode task create failed");
                s_wp_ready = 0;
            }
        }
        lv_timer_create(wp_poll_cb, 100, NULL);   /* 100ms 轮询解码结果 */

        /* ---- 时间背后的胶囊遮罩：亮壁纸上白字才看得清 ---- */
        lv_obj_t *cap = lv_obj_create(lv_scr_act());
        /* ★ 宽度自适应内容：96px 等宽 8 字符约 424px，固定 430 会把秒的个位裁掉一角；
         *   LV_SIZE_CONTENT 让胶囊跟着文本走，永不裁切。 */
        lv_obj_set_size(cap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_pos(cap, 20, 14);
        lv_obj_set_style_bg_color(cap, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_50, 0);
        lv_obj_set_style_border_width(cap, 0, 0);
        lv_obj_set_style_radius(cap, 18, 0);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cap, 10, 0);

        s_lbl_time = lv_label_create(cap);
        lv_obj_set_style_text_font(s_lbl_time, &ui_font_digit_48, 0);
        lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(s_lbl_time, "--:--:--");
        lv_obj_set_pos(s_lbl_time, 4, 2);

        s_lbl_date = lv_label_create(cap);
        lv_obj_set_style_text_font(s_lbl_date, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_date, lv_color_hex(0xD6E0F0), 0);
        lv_label_set_text(s_lbl_date, "");
        /* ★ 相对时间标签对齐：字体的 line_height 各字号不同，用魔法数字摆 y
         *   必然被某字号的时间行盖住（实测 48px 盖住日期）。align_to 永远贴对。 */
        lv_obj_align_to(s_lbl_date, s_lbl_time, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 2);



        /* ---- 全屏透明点击层：触摸任意位置返回来源场景 ----
         * ★ 必须最后创建（LVGL 后创建的在上层），否则会被时间/胶囊挡住点击。
         * ★ 事件用 PRESSED（按下即触发）。之前"轻点经常不灵"的真根因是本场景
         *   hold_ms=1000（触摸处理 1s 一跳），已改 20ms——与 hold 无关，见 ui_manager.c。 */
        lv_obj_t *tap = lv_obj_create(lv_scr_act());
        lv_obj_set_size(tap, WP_W, WP_H);
        lv_obj_set_pos(tap, 0, 0);
        lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tap, 0, 0);
        lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tap, clock_tap_cb, LV_EVENT_PRESSED, NULL);

        s_last_min = -1;
        update_clock(true);
    }

    update_clock(false);

    /* ★★★ 本项目的渲染驱动模式：每个场景在自己的 draw 里调 lvgl_port_task()
     *   （它跑 lv_timer_handler：定时器 + 渲染 + flush）。
     *   漏了这行 = LVGL 从不被驱动 ⇒ 屏幕永远停在上一帧（"ready"/旧页残影），
     *   而对象树、心跳、解码全部正常 —— 与实测症状完全吻合。 */
    lvgl_port_task();
}
