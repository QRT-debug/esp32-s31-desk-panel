/**
 * @file    led_service.c
 * @brief   WS2812 公共控制服务实现（底色 + 临时效果 + 自动恢复）
 *
 * 分层：本文件在 bsp_led（RMT 驱动）之上，只做「状态合成」，不碰硬件细节。
 * 刷新：一个 5ms tick 的任务驱动效果；无效果时不调用硬件（省 CPU/RMT）。
 *
 * ★ 平滑性（本文件的设计重点）：
 *   WS2812 每通道只有 8bit，而亮度上限压到 19% 后实际只剩 ~48 级可选，
 *   直接按整级输出时肉眼能看到一格一格的台阶。这里用三招解决：
 *     1) 统一在 HSV 域输出，不做 HSV->RGB->HSV 往返；
 *     2) 亮度用 Q8 定点（1/256 级精度）计算；
 *     3) 时间抖动（temporal dithering）：不足一个灰阶的余数累计到下一帧。
 *   配合 200Hz 更新率，等效精度远高于 8bit。
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

/* ★ 5ms = 200Hz。效果的平滑度直接取决于更新率：10ms(100Hz) 时
 *   每帧亮度要跨 4 个灰阶，抖动也来不及积分，看起来就是"一格一格"。 */
#define TICK_MS     5

/* ★ 全局亮度上限（0~255，**感知线性**，不是 PWM 占空）。
 *   ⚠ 这里踩过坑：bsp_led 内部有 gamma 2.8 校正，val 到 PWM 是幂律映射。
 *   之前设 48 以为"19%"，实际 PWM = (48/255)^2.8×255 ≈ 2 级 ⇒ 几乎全黑，
 *   色相过渡只剩两三个码值可表达，怎么调动画都"不丝滑"。
 *   现在 140 ⇒ PWM = (140/255)^2.8×255 ≈ 19%（约 48 级，配合抖动足够细腻）。
 *   想更亮/更暗改这一个数即可：180≈38%，110≈12%。 */
#define LED_MASTER_SCALE   140

/* 定点 Q8：把 0~255 的量放大 256 倍，让亮度有亚整数精度（抖动需要） */
#define Q8_ONE     256
#define Q8_MAX     65280

typedef enum
{
    FX_NONE = 0,
    FX_FLASH,
    FX_BREATH,
    FX_RAINBOW,
} fx_t;

static SemaphoreHandle_t s_mtx;
static bool              s_inited;

/* 底色（效果结束后恢复到这里）。存 HSV 而不是 RGB：
 * 输出层是 HSV，存 HSV 可以省掉每次恢复时的一次颜色空间转换。 */
static uint16_t s_base_h;
static uint8_t  s_base_s, s_base_v;

/* 当前效果 */
static fx_t      s_fx;
static uint16_t  s_fx_h;
static uint8_t   s_fx_s, s_fx_v;
static int       s_on_ms, s_off_ms, s_times;
static int       s_period_ms;
static int64_t   s_t_start;

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

/* HSV -> RGB，仅用于 led_get_base_rgb()（对外仍以 RGB 表达底色） */
static led_rgb_t hsv_to_rgb(uint16_t hue, uint8_t sat, uint8_t val)
{
    if (val == 0)
    {
        return 0;
    }
    if (sat == 0)
    {
        return ((led_rgb_t)val << 16) | ((led_rgb_t)val << 8) | val;
    }

    hue %= 360;
    uint8_t region    = (uint8_t)(hue / 60);
    uint8_t remainder = (uint8_t)(hue % 60);
    uint8_t p = (uint8_t)((uint32_t)val * (255 - sat) / 255);
    uint8_t q = (uint8_t)((uint32_t)val * (255 - (uint32_t)sat * remainder / 60) / 255);
    uint8_t t = (uint8_t)((uint32_t)val * (255 - (uint32_t)sat * (60 - remainder) / 60) / 255);

    uint8_t r, g, b;
    switch (region)
    {
    case 0:  r = val; g = t;   b = p;   break;
    case 1:  r = q;   g = val; b = p;   break;
    case 2:  r = p;   g = val; b = t;   break;
    case 3:  r = p;   g = q;   b = val; break;
    case 4:  r = t;   g = p;   b = val; break;
    default: r = val; g = p;   b = q;   break;
    }
    return ((led_rgb_t)r << 16) | ((led_rgb_t)g << 8) | b;
}

/* ---------------- 输出层（唯一出口） ---------------- */

/**
 * 按 HSV 输出，亮度用 Q8（0~Q8_MAX）。
 * 所有效果都走这里，这样"压亮度"只写一次，新增效果不会漏掉。
 *
 * ★ 这里**不做**抖动/去重：真正的量化点在 bsp_led 的 gamma 之后，
 *   抖动必须放在那儿才有效（见 bsp_led_set_rgb）。
 */
static void write_hsv_q8(uint16_t h, uint8_t s, int v_q8)
{
    if (v_q8 <= 0)
    {
        bsp_led_off();
        return;
    }
    if (v_q8 > Q8_MAX)
    {
        v_q8 = Q8_MAX;
    }

    /* 全局亮度上限：在 Q8 域做乘法，不丢小数 */
    v_q8 = v_q8 * LED_MASTER_SCALE / 255;

    int v8 = (v_q8 + 128) / 256;        /* Q8 -> 感知线性 8bit（四舍五入） */
    if (v8 <= 0)
    {
        bsp_led_off();
        return;
    }
    if (v8 > 255)
    {
        v8 = 255;
    }

    led_rgb_t c = hsv_to_rgb(h, s, (uint8_t)v8);
    bsp_led_set_rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}

/* 效果输出：effect 色按 scale（Q8）缩放亮度 */
static void write_fx_scaled_q8(int scale_q8)
{
    int v_q8 = (int)((int32_t)s_fx_v * scale_q8 / 255);
    write_hsv_q8(s_fx_h, s_fx_s, v_q8);
}

/* ---------------- 刷新任务 ---------------- */

/** 0 -> Q8_MAX -> 0 的平滑曲线（抛物线，无折角）。
 *  三角波在峰值/谷值有折角，加速度突变，看起来"顿"；抛物线端点更柔和。 */
static int ease_q8(int t_q8)
{
    if (t_q8 <= 0)
    {
        return 0;
    }
    if (t_q8 >= Q8_MAX)
    {
        return 0;
    }
    /* 4t(1-t) 形状（t 归一到 0~1），峰值在 t=0.5 */
    int64_t x = (int64_t)t_q8 * (Q8_MAX - t_q8);
    return (int)(x * 4 / Q8_MAX);
}

/** 线性三角波 Q8：0 -> Q8_MAX -> 0，一个来回 period_ms */
static int triangle_q8(int64_t el, int period_ms)
{
    if (period_ms <= 0)
    {
        return 0;
    }
    int half = period_ms / 2;
    if (half < 1)
    {
        half = 1;
    }
    int p = (int)(el % period_ms);
    return (p < half) ? (int)((int64_t)p * Q8_MAX / half)
                      : (int)((int64_t)(period_ms - p) * Q8_MAX / half);
}

static void fx_end_locked(void)
{
    s_fx = FX_NONE;
    write_hsv_q8(s_base_h, s_base_s, (int)s_base_v * Q8_ONE);
}

static void led_task(void *arg)
{
    (void)arg;

    while (1)
    {
        xSemaphoreTake(s_mtx, portMAX_DELAY);

        int64_t el = (esp_timer_get_time() / 1000) - s_t_start;

        switch (s_fx)
        {
        case FX_FLASH:
        {
            int cyc = s_on_ms + s_off_ms;
            if (cyc <= 0)
            {
                fx_end_locked();
                break;
            }
            int total = cyc * (s_times > 0 ? s_times : 1);
            if (el >= total)
            {
                fx_end_locked();
            }
            else if ((el % cyc) < s_on_ms)
            {
                write_fx_scaled_q8(Q8_MAX);     /* 亮 */
            }
            else
            {
                write_hsv_q8(0, 0, 0);          /* 灭 */
            }
            break;
        }

        case FX_BREATH:
            if (s_period_ms <= 0)
            {
                fx_end_locked();
                break;
            }
            write_fx_scaled_q8(ease_q8(triangle_q8(el, s_period_ms)));
            break;

        case FX_RAINBOW:
        {
            if (s_period_ms <= 0)
            {
                fx_end_locked();
                break;
            }

            /* 色相：一个周期走完 360°，视觉上颜色连续流转。
             * 360 级色相在 3.6s/圈下每 10ms 变 1°，人眼已分辨不出。 */
            int p = (int)(el % s_period_ms);
            uint16_t hue = (uint16_t)((int64_t)p * 360 / s_period_ms);

            /* ★ 亮度恒定，不做呼吸。
             *   之前每圈呼吸两次，谷底亮度经 gamma 后只剩 1~2 级 PWM
             *   ⇒ 用户看到的是"渐变中间灭了好几次"。颜色渐变本身已经
             *   足够有动感，不需要再叠一层明暗。 */
            int v_q8 = Q8_MAX;

            /* 轻度感知亮度补偿：纯色相环上黄(226)/绿(149) 比蓝(29) 亮得多，
             * 只压不抬（抬高蓝紫会超出亮度上限，物理上也做不到等亮度）。 */
            led_rgb_t probe = hsv_to_rgb(hue, 220, 255);
            int luma = (299 * (int)((probe >> 16) & 0xFF) +
                        587 * (int)((probe >> 8) & 0xFF) +
                        114 * (int)(probe & 0xFF)) / 1000;
            if (luma > 128)
            {
                v_q8 = v_q8 * (255 - (luma - 128) / 2) / 255;
            }

            write_hsv_q8(hue, 220, v_q8);
            break;
        }

        default:
            break;
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

static void set_base_hsv(uint16_t h, uint8_t s, uint8_t v)
{
    ensure_init();
    if (s_mtx == NULL)
    {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_base_h = h;
    s_base_s = s;
    s_base_v = v;
    if (s_fx == FX_NONE)
    {
        write_hsv_q8(h, s, (int)v * Q8_ONE);
    }
    xSemaphoreGive(s_mtx);
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t h;
    uint8_t  s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);
    set_base_hsv(h, s, v);
}

void led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    set_base_hsv(hue, sat, val);
}

void led_off(void)
{
    led_set_rgb(0, 0, 0);
}

void led_flash(uint8_t r, uint8_t g, uint8_t b, int on_ms, int off_ms, int times)
{
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

    ensure_init();
    if (s_mtx == NULL)
    {
        return;
    }

    uint16_t h;
    uint8_t  s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fx_h     = h;
    s_fx_s     = s;
    s_fx_v     = v;
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

    uint16_t h;
    uint8_t  s, v;
    rgb_to_hsv(r, g, b, &h, &s, &v);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fx_h      = h;
    s_fx_s      = s;
    s_fx_v      = v;
    s_period_ms = period_ms;
    s_t_start   = esp_timer_get_time() / 1000;
    s_fx        = FX_BREATH;
    xSemaphoreGive(s_mtx);
}

void led_rainbow(int period_ms)
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
    s_period_ms = period_ms;
    s_t_start   = esp_timer_get_time() / 1000;
    s_fx        = FX_RAINBOW;
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
    return hsv_to_rgb(s_base_h, s_base_s, s_base_v);
}

bool led_is_effect_running(void)
{
    return (s_fx != FX_NONE);
}
