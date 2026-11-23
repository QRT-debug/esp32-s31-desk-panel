/**
 * @file    led_service.c
 * @brief   WS2812 公共控制服务实现（底色 + 临时效果 + 自动恢复）
 *
 * 分层：本文件在 bsp_led（RMT 驱动）之上，只做「状态合成」，不碰硬件细节。
 * 刷新：一个 10ms tick 的任务驱动效果；无效果时不调用硬件（省 CPU/RMT）。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "bsp_led.h"
#include "led_service.h"

static const char *TAG = "ledsvc";

#define TICK_MS     10

typedef enum
{
    FX_NONE = 0,
    FX_FLASH,
    FX_BREATH,
} fx_t;

static SemaphoreHandle_t s_mtx;
static bool              s_inited;

/* 底色（效果结束后恢复到这里） */
static led_rgb_t s_base = 0x000000;

/* 当前效果 */
static fx_t      s_fx;
static led_rgb_t s_fx_rgb;
static int       s_on_ms, s_off_ms, s_times;
static int       s_period_ms;
static int64_t   s_t_start;
static led_rgb_t s_last_out = 0xFFFFFFFF;   /* 上次真正写给硬件的颜色（去重） */

/* ---------------- 颜色转换 ---------------- */

static void rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b,
                       uint16_t *h, uint8_t *s, uint8_t *v)
{
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    uint8_t d  = (uint8_t)(mx - mn);

    *v = mx;
    *s = (mx == 0) ? 0 : (uint8_t)((uint32_t)d * 255u / mx);

    if (d == 0)
    {
        *h = 0;
        return;
    }
    int hv;
    if (mx == r)
    {
        hv = 60 * ((int)g - (int)b) / (int)d;
    }
    else if (mx == g)
    {
        hv = 120 + 60 * ((int)b - (int)r) / (int)d;
    }
    else
    {
        hv = 240 + 60 * ((int)r - (int)g) / (int)d;
    }
    if (hv < 0)
    {
        hv += 360;
    }
    *h = (uint16_t)(hv % 360);
}

static void write_rgb(led_rgb_t rgb)
{
    if (rgb == s_last_out)
    {
        return;     /* 去重：颜色没变就不碰 RMT，省总线时间 */
    }
    s_last_out = rgb;

    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;

    if (r == 0 && g == 0 && b == 0)
    {
        bsp_led_off();
        return;
    }
    uint16_t h;
    uint8_t  s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    bsp_led_set_hsv(h, s, v);
}

/* 按缩放系数输出效果色（0~255） */
static void write_fx_scaled(int scale)
{
    uint8_t r = (s_fx_rgb >> 16) & 0xFF;
    uint8_t g = (s_fx_rgb >> 8) & 0xFF;
    uint8_t b = s_fx_rgb & 0xFF;
    if (scale <= 0)
    {
        write_rgb(0);
        return;
    }
    if (scale > 255)
    {
        scale = 255;
    }
    write_rgb(((led_rgb_t)(r * scale / 255) << 16) |
              ((led_rgb_t)(g * scale / 255) << 8) |
              (led_rgb_t)(b * scale / 255));
}

/* ---------------- 刷新任务 ---------------- */

static void fx_end_locked(void)
{
    s_fx = FX_NONE;
    write_rgb(s_base);     /* 效果结束自动回到底色 */
}

static void led_task(void *arg)
{
    (void)arg;

    while (1)
    {
        xSemaphoreTake(s_mtx, portMAX_DELAY);

        if (s_fx == FX_FLASH)
        {
            int64_t el  = (esp_timer_get_time() / 1000) - s_t_start;
            int     cyc = s_on_ms + s_off_ms;
            if (cyc <= 0)
            {
                fx_end_locked();
            }
            else
            {
                int total = cyc * (s_times > 0 ? s_times : 1);
                if (el >= total)
                {
                    fx_end_locked();
                }
                else if ((el % cyc) < s_on_ms)
                {
                    write_fx_scaled(255);      /* 亮 */
                }
                else
                {
                    write_rgb(0);              /* 灭 */
                }
            }
        }
        else if (s_fx == FX_BREATH && s_period_ms > 0)
        {
            int64_t el = (esp_timer_get_time() / 1000) - s_t_start;
            /* 三角波：0 -> 255 -> 0，一个来回 period_ms */
            int half = s_period_ms / 2;
            if (half < 1)
            {
                half = 1;
            }
            int p = (int)(el % s_period_ms);
            int scale = (p < half) ? (p * 255 / half) : ((s_period_ms - p) * 255 / half);
            write_fx_scaled(scale);
        }

        xSemaphoreGive(s_mtx);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

/* ---------------- 对外接口 ---------------- */

void led_service_init(void)
{
    if (s_inited)
    {
        return;
    }
    if (s_mtx == NULL)
    {
        s_mtx = xSemaphoreCreateMutex();
    }
    if (s_mtx == NULL)
    {
        ESP_LOGE(TAG, "mutex create failed");
        return;
    }

    bsp_led_init();

    if (xTaskCreate(led_task, "ledsvc", 3072, NULL, 3, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    s_inited = true;
    ESP_LOGI(TAG, "led service ready");
}

/* 惰性初始化：任何上层不必关心"有没有 init 过" */
static void ensure_init(void)
{
    if (!s_inited)
    {
        led_service_init();
    }
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ensure_init();
    if (s_mtx == NULL)
    {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_base = ((led_rgb_t)r << 16) | ((led_rgb_t)g << 8) | b;
    if (s_fx == FX_NONE)
    {
        write_rgb(s_base);
    }
    xSemaphoreGive(s_mtx);
}

void led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    /* 复用 LVGL 的转换会引入 UI 依赖，这里手算，保持 BSP 层干净 */
    uint8_t v = val;
    uint8_t s = sat;
    uint8_t r = 0, g = 0, b = 0;

    if (v == 0)
    {
        led_set_rgb(0, 0, 0);
        return;
    }
    uint8_t region    = (uint8_t)(hue / 60);
    uint8_t remainder = (uint8_t)(hue % 60);
    uint8_t p = (uint8_t)((uint32_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint32_t)v * (255 - (uint32_t)s * remainder / 60) / 255);
    uint8_t t = (uint8_t)((uint32_t)v * (255 - (uint32_t)s * (60 - remainder) / 60) / 255);

    switch (region)
    {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    led_set_rgb(r, g, b);
}

void led_off(void)
{
    led_set_rgb(0, 0, 0);
}

void led_flash(uint8_t r, uint8_t g, uint8_t b, int on_ms, int off_ms, int times)
{
    ensure_init();
    if (s_mtx == NULL)
    {
        return;
    }
    if (on_ms <= 0)
    {
        on_ms = 100;
    }
    if (off_ms < 0)
    {
        off_ms = 0;
    }
    if (times <= 0)
    {
        times = 1;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fx_rgb   = ((led_rgb_t)r << 16) | ((led_rgb_t)g << 8) | b;
    s_on_ms    = on_ms;
    s_off_ms   = off_ms;
    s_times    = times;
    s_t_start  = esp_timer_get_time() / 1000;
    s_fx       = FX_FLASH;
    xSemaphoreGive(s_mtx);
}

void led_breath(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    if (period_ms <= 0)
    {
        led_stop_effect();
        return;
    }
    ensure_init();
    if (s_mtx == NULL)
    {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fx_rgb    = ((led_rgb_t)r << 16) | ((led_rgb_t)g << 8) | b;
    s_period_ms = period_ms;
    s_t_start   = esp_timer_get_time() / 1000;
    s_fx        = FX_BREATH;
    xSemaphoreGive(s_mtx);
}

void led_stop_effect(void)
{
    if (s_mtx == NULL)
    {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_fx != FX_NONE)
    {
        fx_end_locked();
    }
    xSemaphoreGive(s_mtx);
}

led_rgb_t led_get_base_rgb(void)
{
    return s_base;
}

bool led_is_effect_running(void)
{
    return (s_fx != FX_NONE);
}
