/**
 * @file    backlight.c
 * @brief   背光 PWM：LEDC 5kHz / 10bit @ IO38 (BL_CTR)
 *
 * 设计取舍：
 *   - 频率 5 kHz：人眼无闪烁，也远高于音频基频（避免串进功放）
 *   - 10 bit（0~1023）：100 级够用，且分辨率不影响一致性
 *   - 上电默认 75%：用户定（100% 在米白浅色界面上偏刺眼），SYSTEM 页滑块可调
 *   - 下限保护：设为 0 时实际给 1%，防止完全黑屏后无法操作（可选）
 */
#include "backlight.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "backlight";

#define BL_GPIO       38
#define BL_FREQ_HZ    5000
#define BL_RES        LEDC_TIMER_10_BIT
#define BL_TIMER      LEDC_TIMER_2
#define BL_CHANNEL    LEDC_CHANNEL_2
#define BL_MODE       LEDC_LOW_SPEED_MODE
#define BL_FULL       1023

static int s_percent = 75;   /* 上电默认亮度 */

esp_err_t backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = BL_MODE,
        .duty_resolution = BL_RES,
        .timer_num       = BL_TIMER,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ccfg = {
        .gpio_num   = BL_GPIO,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,   /* ★ 11-20：init 只配 PWM 不点亮——首帧后由 lvgl_port 调 backlight_default() */
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_percent = 75;
    ESP_LOGI(TAG, "PWM ready: IO%d, %d Hz, 10bit (duty 0, first-frame lit)", BL_GPIO, BL_FREQ_HZ);
    return ESP_OK;
}

/* ★ 11-20：LVGL 首帧 flush 完成后由 lvgl_port 调用——此刻才点亮背光，
 *   消灭"复位→首帧"期间的白屏（屏已清黑 + 背光灭 = 全程黑屏过渡） */
/* ★★★ 11-22 定案：默认 **75%**（用户长期使用值）。
 *
 *   诊断期间曾临时改成 100%（为了排除"PWM 斩背光"这个嫌疑），并由此查实
 *   5kHz PWM 会让模组内升压恒流反复启停（75% 的实际亮度低于标称 75%）。
 *   但**闪/波浪/抖动/白竖线四项全部与背光无关**，都由显示时序解决（见 rgb_lcd.h）。
 *   ⇒ 亮度回 75%。用户实测 60%/75% 下都看不到 5kHz 与 66Hz 的拍频包络
 *     （我按线性响应推算的"17.5Hz、30% 深度"是高估：升压的电感+输出电容把 5kHz 平滑掉了）。
 *
 *   ★ 亮度越低 ⇒ CFF 越低 ⇒ 66Hz 的 flicker 余量越大。所以 75% 比 100% 更安全，
 *     不存在"亮度低反而更容易闪"的情况。
 *   ★ 滑条百分比**不是真实亮度百分比**（升压被斩时强非线性），看着舒服即可。 */
void backlight_set_boot(int percent)
{
    if (percent < 10)
    {
        percent = 10;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    s_percent = percent;
}

void backlight_default(void)
{
    /* ★ 用 s_percent 而不是硬编码 75：配网页存进 NVS 的亮度由 main 通过
     *   backlight_set_boot() 推进来，否则用户设了 80 也会被这里覆盖回 75。 */
    backlight_set(s_percent);
}

void backlight_set(int percent)
{
    if (percent < 0)
    {
        percent = 0;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    s_percent = percent;

    uint32_t duty = (uint32_t)((BL_FULL * percent) / 100);
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

int backlight_get(void)
{
    return s_percent;
}
