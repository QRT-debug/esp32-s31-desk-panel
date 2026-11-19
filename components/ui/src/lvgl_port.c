/**
 * @file    lvgl_port.c
 * @brief   LVGL 9.6 移植层实现
 */
#include "lvgl_port.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
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
extern uint8_t  refresh_done_flag;

static bool          s_ready = false;
static lv_display_t *s_disp  = NULL;

/* ---------------- 显示 ---------------- */

/**
 * flush 回调（双缓冲 + vsync 切换）
 *
 * ★ 之前试过"单缓冲直写"（什么都不做，让 LVGL 直接写面板正在扫描的那块），
 *   结果就是边缘频闪 —— 写 20 KB 虽然只要 0.1 ms，但面板刚好扫到就吃到半更新的数据。
 *   纯黑 8 秒时边缘完全静止，证明闪是"写入"引起的，不是面板本身。
 *
 * 现在的做法：双缓冲轮换，LVGL 永远写"当前没在显示"的那块，
 * 画完把 panel 切过去。代价是每帧等一次 vsync（~20 ms），
 * 对每秒只变几个数字的监控界面完全够用。
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;

    static uint32_t cnt = 0;
    cnt++;

    long w = (long)(area->x2 - area->x1 + 1);
    long h = (long)(area->y2 - area->y1 + 1);
    if (cnt <= 8 || (cnt % 30) == 0)
    {
        ESP_LOGI(TAG, "frame #%lu  %ldx%ld  fb=%s",
                 (unsigned long)cnt, w, h,
                 (px_map == (uint8_t *)lcd_buffer[0]) ? "0" : "1");
    }

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

    /* ★ 11-20：首帧 flush 完成（等过一次 vsync，画面已在屏上）才点亮背光，
     *   消灭复位后到首帧之间的白屏期（lcd_init 已改清黑 + 背光 init 不点亮） */
    {
        static bool s_bl_on = false;
        if (!s_bl_on)
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

    /* 双缓冲 + FULL：LVGL 每帧把整屏渲染到"不在显示"的那块，画完由 flush_cb 切过去。
     * ★ 必须 FULL 而不是 DIRECT：DIRECT 只重绘脏区，轮换到另一块时那边是上一轮的旧内容，
     *   会出现"新旧交替"的缺损。 */
    lv_display_set_buffers(s_disp,
                           lcd_buffer[0],
                           lcd_buffer[1],
                           (uint32_t)LCD_H_RES * LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);

    s_ready = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d ready  %dx%d  full/double-buffer  %u KB x2",
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