/**
 * @file    lvgl_port.c
 * @brief   LVGL 9.6 移植层实现
 */
#include "lvgl_port.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "backlight.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "demos/benchmark/lv_demo_benchmark.h"
#include "touch_service.h"

static const char *TAG = "lvgl_port";

/* lcd.c 提供的双帧缓冲与 vsync 标志 */
extern void    *lcd_buffer[2];
extern uint8_t  buffer_sw;
extern volatile uint8_t refresh_done_flag;   /* ISR 写、任务读，必须 volatile */

static bool          s_ready = false;
static lv_display_t *s_disp  = NULL;

/* ---------------- 显示 ---------------- */

/* ================= 脏区记账（cache 同步用） =================
 *
 * ★★★ 2026-11-22 找到"四周随机区域频闪"的真凶：**LVGL 自己的双缓冲同步拷贝没做 cache 写回**。
 *
 *   DIRECT + 双缓冲时，LVGL 每帧先调 refr_sync_areas()，用 **纯 CPU memcpy**
 *   (lv_draw_buf_copy) 把"上一帧的脏区"从正在扫描的那块 fb 拷到即将渲染的那块 fb
 *   （见 lv_refr.c: refr_sync_areas → lv_draw_buf_copy）。
 *   这个 memcpy 写的是 PSRAM（帧缓冲在 PSRAM 且 cacheable），而 LCD 的 DMA **绕过 cache**
 *   直读 PSRAM ⇒ 那块被拷过的区域，DMA 读到的是**两帧以前的旧内容**，
 *   下一帧再被覆盖 —— 肉眼看到的就是"某片区域闪一下"，位置随脏区跑。
 *
 *   而 flush_cb 原来只 sync 本帧的 area，**拷过来的区域根本不经过 flush 回调**，
 *   所以永远 sync 不到。⇒ 必须把"上一帧的脏区"也一起 sync。
 *
 *   为什么只在刷新时看得见：界面静止时 LVGL 不刷新，src_sync 不执行也不会切 fb。 */
#define MAX_DIRTY 16

static lv_area_t s_dir_cur[MAX_DIRTY];
static int       s_dir_cur_n   = 0;
static bool      s_dir_cur_ovf = false;     /* 超出 16 块 ⇒ 退化整屏 sync */
static lv_area_t s_dir_prev[MAX_DIRTY];
static int       s_dir_prev_n  = 0;
static bool      s_dir_prev_ovf = false;

/** 把一块区域的**整行**从 CPU cache 写回 PSRAM（行步长 1600B 是 64B cache line 的整数倍） */
static void cache_sync_rows(const uint8_t *base, int32_t y1, int32_t y2)
{
    const size_t line_bytes = (size_t)LCD_H_RES * 2;

    if (y1 < 0)
    {
        y1 = 0;
    }
    if (y2 > LCD_V_RES - 1)
    {
        y2 = LCD_V_RES - 1;
    }
    if (y2 < y1)
    {
        return;
    }

    uint8_t *p     = (uint8_t *)base + (size_t)y1 * line_bytes;
    size_t   bytes = (size_t)(y2 - y1 + 1) * line_bytes;

    esp_err_t err = esp_cache_msync(p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (err != ESP_OK)
    {
        static int once = 0;
        if (once++ == 0)
        {
            ESP_LOGW(TAG, "cache msync: %s", esp_err_to_name(err));
        }
    }
}

/**
 * flush 回调（双缓冲 + vsync 切换）
 *
 * ★ 之前试过"单缓冲直写"（什么都不做，让 LVGL 直接写面板正在扫描的那块），
 *   结果就是边缘频闪 —— 写 20 KB 虽然只要 0.1 ms，但面板刚好扫到就吃到半更新的数据。
 *
 * 现在的做法：双缓冲轮换，LVGL 永远写"当前没在显示"的那块，
 * 画完把 panel 切过去。代价是每帧等一次 vsync（~20 ms）。
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 记账本帧脏区（is_last 时统一 sync） */
    if (s_dir_cur_n < MAX_DIRTY)
    {
        s_dir_cur[s_dir_cur_n++] = *area;
    }
    else
    {
        s_dir_cur_ovf = true;
    }

    /* ★★ DIRECT 模式下 LVGL 会把一次刷新拆成**多个脏区**，每块调一次本回调。
     *   只有最后一块才需要"切帧缓冲 + 等 vsync"；中间的块若也等，
     *   实测 flush 会被拉到 175ms（8 块 × 20ms），界面卡顿、CPU 空转 43%。
     *   中间块直接 flush_ready —— 它们已经画进 buf_act，最后一块切过去时一起显示。 */
    if (lv_display_flush_is_last(disp))
    {
        /* ★ 临时诊断：节流到每 2 秒一条，看 refr/flush 是否在跑 */
        {
            static int64_t s_last_log = 0;
            int64_t now = esp_timer_get_time();
            if (now - s_last_log > 2000000)
            {
                s_last_log = now;
                ESP_LOGI("flush", "last flush at %lldms", now / 1000);
            }
        }
        /* ★★★ 见文件开头 MAX_DIRTY 处的说明：必须同时 sync
         *   ① 本帧新画的脏区  ② **上一帧的脏区**（被 refr_sync_areas 用 CPU memcpy 拷过、
         *   不经过本回调，不 sync 就会让 DMA 读到旧像素 = 局部闪）。
         *   多 sync 几个区域没有副作用（只多写回几条 cache line），少 sync 一定花屏。 */
        if (s_dir_cur_ovf || s_dir_prev_ovf)
        {
            cache_sync_rows(px_map, 0, LCD_V_RES - 1);
        }
        else
        {
            for (int i = 0; i < s_dir_cur_n; i++)
            {
                cache_sync_rows(px_map, s_dir_cur[i].y1, s_dir_cur[i].y2);
            }
            for (int i = 0; i < s_dir_prev_n; i++)
            {
                cache_sync_rows(px_map, s_dir_prev[i].y1, s_dir_prev[i].y2);
            }
        }

        /* 本帧 → 上帧，为下一次刷新记账 */
        memcpy(s_dir_prev, s_dir_cur, sizeof(lv_area_t) * (size_t)s_dir_cur_n);
        s_dir_prev_n   = s_dir_cur_n;
        s_dir_prev_ovf = s_dir_cur_ovf;
        s_dir_cur_n    = 0;
        s_dir_cur_ovf  = false;

        /* 先清标志再提交：提交后到来的 vsync 才不会被误清 */
        refresh_done_flag = 0;
        esp_lcd_panel_draw_bitmap(lcddev.lcd_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, px_map);

        int64_t t0 = esp_timer_get_time();
        while (!refresh_done_flag)
        {
            vTaskDelay(1);
            __sync_synchronize();
            if (esp_timer_get_time() - t0 > 200000)     /* 200 ms 超时保护 */
            {
                ESP_LOGW(TAG, "vsync timeout");
                break;
            }
        }
    }

    /* ★ 11-20：首帧 flush 完成（等过一次 vsync，画面已在屏上）才点亮背光，
     *   消灭复位后到首帧之间的白屏期（lcd_init 已改清黑 + 背光 init 不点亮） */
    {
        static bool s_bl_on = false;
        if (!s_bl_on && lv_display_flush_is_last(disp))
        {
            s_bl_on = true;
            backlight_default();
            ESP_LOGI(TAG, "backlight on (first frame done)");
        }
    }

    lv_display_flush_ready(disp);
}

/* ---------------- 输入 ---------------- */

/**
 * 直接读 touch_service 维护的"最近状态"，不消费事件队列：
 * 队列要留给 ui_manager 做"点左/右半屏切场景"。
 */
static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static uint16_t last_x = 0;
    static uint16_t last_y = 0;

    uint16_t x = 0, y = 0;
    bool     pressed = false;
    touch_get_last(&x, &y, &pressed);

    if (pressed)
    {
        last_x = x;
        last_y = y;
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---------------- tick ---------------- */

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t lvgl_port_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* top / sys 两个覆盖层默认可能带背景，会在每次重绘时露出边缘一圈 */
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(lv_layer_sys(), LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lv_layer_top(), 0, 0);
    lv_obj_set_style_border_width(lv_layer_sys(), 0, 0);
    lv_obj_set_style_pad_all(lv_layer_top(), 0, 0);
    lv_obj_set_style_pad_all(lv_layer_sys(), 0, 0);
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_SCROLLABLE);

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (s_disp == NULL)
    {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* ★★★ 11-22 定案：用 DIRECT（只渲染脏区）。
     *   昨晚之所以回退到 FULL，是误以为"DIRECT 会导致闪"——实际闪是**热复位**造成的
     *   （RGB 屏无 RST，esptool 硬复位只复位芯片不复位面板），与渲染模式无关。
     *   FULL 模式每帧整屏渲染（实测 render 60ms、写 768KB），DIRECT 只写脏区
     *   （实测 7.5KB/帧），流畅度差一个数量级。
     *   双缓冲一致性由 LVGL 9.6 的 refr_sync_areas() 自动同步（auto_sync）。 */
    lv_display_set_buffers(s_disp,
                           lcd_buffer[0],
                           lcd_buffer[1],
                           (uint32_t)LCD_H_RES * LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    lv_indev_t *indev = lv_indev_create();
    /* ★★ 11-21 教训：全局调低手势阈值会让"拖动滑条"被误判为手势，
     *   System 页拖 LED 滑条时崩溃（乱屏+重启）。这里保持 LVGL 默认值；
     *   需要更灵敏翻页的页面（HF）自行用 PRESSED/RELEASED 算位移，不走手势引擎。 */
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);

    s_ready = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d ready  %dx%d  DIRECT/double-buffer  %u KB x2",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             LCD_H_RES, LCD_V_RES,
             (unsigned)((uint32_t)LCD_H_RES * LCD_V_RES * 2 / 1024));
    return ESP_OK;
}

bool lvgl_port_ready(void)
{
    return s_ready;
}

void lvgl_port_task(void)
{
    if (!s_ready)
    {
        return;
    }
    lv_timer_handler();
}

/**
 * benchmark 跑完的回调：把每个场景的数据打到串口，同时保留屏幕上的汇总表。
 *
 * benchmark 自己只做两件事：屏幕画表 + LV_LOG(TRACE) 输出 CSV。
 * 我们开的日志级别是 USER，TRACE 被过滤，所以必须自己挂回调才拿得到数据。
 */
static void bench_end_cb(const lv_demo_benchmark_summary_t *s)
{
    ESP_LOGI(TAG, "================ LVGL BENCHMARK RESULT ================");

    if (s->valid_scene_cnt > 0)
    {
        ESP_LOGI(TAG, "AVG(%ld scenes)  cpu %ld%%  fps %ld  render %ld ms  flush %ld ms",
                 (long)s->valid_scene_cnt,
                 (long)(s->total_avg_cpu / s->valid_scene_cnt),
                 (long)(s->total_avg_fps / s->valid_scene_cnt),
                 (long)(s->total_avg_render_time / s->valid_scene_cnt),
                 (long)(s->total_avg_flush_time / s->valid_scene_cnt));
    }

    for (const lv_demo_benchmark_scene_dsc_t *sc = s->scenes;
         sc->create_cb != NULL; sc++)
    {
        if (sc->measurement_cnt == 0)
        {
            continue;
        }
        /* ★ cpu_avg_usage / fps_avg / render_avg_time 都是**累加值**，
         *   必须除以 measurement_cnt 才是平均。直接打印会出现 cpu 724% 这种鬼数。 */
        const uint32_t n = sc->measurement_cnt;
        ESP_LOGI(TAG, "%-26s cpu %3lu%%  fps %3lu  render %3lu ms  flush %3lu ms  (n=%lu)",
                 sc->name,
                 (unsigned long)(sc->cpu_avg_usage / n),
                 (unsigned long)(sc->fps_avg / n),
                 (unsigned long)(sc->render_avg_time / n),
                 (unsigned long)(sc->flush_avg_time / n),
                 (unsigned long)n);
    }
    ESP_LOGI(TAG, "=======================================================");

    /* 屏幕上照常显示汇总表，方便不开串口时直接看 */
    lv_demo_benchmark_summary_display(s);
}

void lvgl_port_start_benchmark(void)
{
    if (!s_ready)
    {
        return;
    }
    ESP_LOGI(TAG, "starting lv_demo_benchmark");
    lv_demo_benchmark_set_end_cb(bench_end_cb);
    lv_demo_benchmark();
}