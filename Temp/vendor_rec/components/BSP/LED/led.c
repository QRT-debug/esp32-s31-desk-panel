/**
 ******************************************************************************
 * @file        led.c
 * @version     V1.0
 * @brief       WS2812B协议 LED驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "led.h"
#include "led_strip.h"

static led_strip_handle_t g_led_strip = NULL;
static uint8_t led_flag = 1;           /* 闪烁状态位: 1=亮 0=灭 */

/**
 * @brief   初始化WS2812B LED (RMT + DMA, 初始熄灭)
 * @param   无
 * @retval  无
 */
void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num          = LED_GPIO_PIN,
        .max_leds                = LED_NUM,
        .color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB,   /* WS2812B: GRB */
        .led_model               = LED_MODEL_WS2812,
        .flags.invert_out        = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = 10 * 1000 * 1000,     /* 10MHz */
        .mem_block_symbols  = 64,
        .flags.with_dma     = true,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &g_led_strip));
    led_off();
}

/**
 * @brief   设置LED颜色 (原始亮度, 不缩放)
 * @param   red    红色分量 (0~255)
 * @param   green  绿色分量 (0~255)
 * @param   blue   蓝色分量 (0~255)
 * @retval  无
 */
void led_set_color(uint32_t red, uint32_t green, uint32_t blue)
{
    led_strip_set_pixel(g_led_strip, 0, red, green, blue);
    led_strip_refresh(g_led_strip);
}

/**
 * @brief   按 LED_BRIGHTNESS 缩放后设置LED颜色
 * @param   rgb  颜色值 (0xRRGGBB)
 * @retval  无
 */
void led_set_rgb(uint32_t rgb)
{
    uint32_t r = ((rgb >> 16) & 0xFF) * LED_BRIGHTNESS / 255;
    uint32_t g = ((rgb >> 8)  & 0xFF) * LED_BRIGHTNESS / 255;
    uint32_t b = ( rgb        & 0xFF) * LED_BRIGHTNESS / 255;
    led_set_color(r, g, b);
}

/**
 * @brief   闪烁指定颜色 (每次调用翻转亮/灭)
 * @param   rgb  颜色值 (0xRRGGBB)
 * @retval  无
 */
void led_blink_color(uint32_t rgb)
{
    if (led_flag) led_set_rgb(rgb);
    else          led_set_rgb(LED_RGB_BLACK);
    led_flag = !led_flag;
}

/**
 * @brief   闪烁LED (默认绿色)
 * @param   无
 * @retval  无
 */
void led_blink(void)
{
    led_blink_color(LED_RGB_GREEN);
}

/**
 * @brief   关闭LED
 * @param   无
 * @retval  无
 */
void led_off(void)
{
    led_set_color(0, 0, 0);
}
