/**
 * @file    bsp_led.c
 * @brief   板载 WS2812 彩灯驱动（IO37）
 *
 * WS2812 = 单线归零码（0.35us/0.8us bit 时序），ESP-IDF 官方 led_strip 组件
 * 走 RMT 外设生成波形（本地化自厂商例程 managed_components）。
 *
 * ★ 坑位备忘：WS2812 数据字节序是 GRB（不是 RGB），组件内部已处理，
 *   调 set_pixel(r,g,b) 传常规顺序即可。
 *
 * ★ 伽马校正（11-20 实测）：人眼亮度感知 ≈ 幂律（^2.2~3.0），滑块线性值
 *   直写 PWM 时"最低档还是很亮"（用户实测 val=3 仍刺眼）。γ=2.8 查表校正后
 *   滑块低段贴地、中段细腻、100% 不变。
 */
#include "bsp_led.h"
#include "esp_log.h"
#include "led_strip.h"

#include <math.h>

static const char *TAG = "bsp_led";

#define LED_GPIO   37          /* LED_CTR（模组丝印，非背光 IO38） */
#define LED_NUM    4           /* 串级按 4 颗发（实际几颗亮几颗，多余无害） */
#define GAMMA      2.8f        /* 亮度感知校正（2.2=sRGB，WS2812 经验 2.8） */

static led_strip_handle_t s_strip = NULL;
static uint8_t s_gamma_lut[256];       /* 感知线性 → PWM 占空 */

void bsp_led_init(void)
{
    if (s_strip != NULL)
    {
        return;
    }

    for (int i = 0; i < 256; i++)
    {
        float v = powf((float)i / 255.0f, GAMMA);
        s_gamma_lut[i] = (uint8_t)(v * 255.0f + 0.5f);
    }

    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = LED_NUM,
        .led_model      = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src           = 0,   /* 0 = 组件默认（10MHz 分辨率） */
        .resolution_hz     = 0,
        .mem_block_symbols = 0,   /* 0 = 默认大小 */
        .flags.with_dma    = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK)
    {
        s_strip = NULL;
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);   /* 上电全灭（不抢戏） */
    ESP_LOGI(TAG, "WS2812 ready: GPIO%d x%d (gamma %.1f)", LED_GPIO, LED_NUM, (double)GAMMA);
}

void bsp_led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    if (s_strip == NULL)
    {
        bsp_led_init();
        if (s_strip == NULL)
        {
            return;
        }
    }

    /* HSV → RGB（复刻组件公式，为的是在这里插 gamma 校正） */
    uint32_t rgb_max = val;
    uint32_t rgb_min = rgb_max * (255 - sat) / 255;
    uint32_t i    = hue / 60;
    uint32_t diff = hue % 60;
    uint32_t adj  = (rgb_max - rgb_min) * diff / 60;

    uint32_t r = 0, g = 0, b = 0;
    switch (i)
    {
    case 0: r = rgb_max; g = rgb_min + adj; b = rgb_min; break;
    case 1: r = rgb_max - adj; g = rgb_max; b = rgb_min; break;
    case 2: r = rgb_min; g = rgb_max; b = rgb_min + adj; break;
    case 3: r = rgb_min; g = rgb_max - adj; b = rgb_max; break;
    case 4: r = rgb_min + adj; g = rgb_min; b = rgb_max; break;
    default: r = rgb_max; g = rgb_min; b = rgb_max - adj; break;
    }

    /* ★ gamma 校正：滑块感知线性 → PWM 实际占空 */
    r = s_gamma_lut[r];
    g = s_gamma_lut[g];
    b = s_gamma_lut[b];

    for (int k = 0; k < LED_NUM; k++)
    {
        led_strip_set_pixel(s_strip, (uint32_t)k, r, g, b);
    }
    led_strip_refresh(s_strip);
}

void bsp_led_off(void)
{
    if (s_strip != NULL)
    {
        led_strip_clear(s_strip);
    }
}
