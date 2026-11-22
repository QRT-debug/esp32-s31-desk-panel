/**
 ******************************************************************************
 * @file        rgb_lcd.c
 * @version     V1.0
 * @brief       rgblcd驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "rgb_lcd.h"
#include "lcd.h"

static const char *rgb_lcd_tag = "rgb_lcd";

/**
 * @brief       填充RGB数据线引脚
 * @param       data_gpio_nums:数据线引脚数组
 * @retval      无
 */
static void rgb_lcd_fill_data_gpio_nums(gpio_num_t data_gpio_nums[16])
{
    data_gpio_nums[0]  = LCD_PIN_NUM_DATA0;
    data_gpio_nums[1]  = LCD_PIN_NUM_DATA1;
    data_gpio_nums[2]  = LCD_PIN_NUM_DATA2;
    data_gpio_nums[3]  = LCD_PIN_NUM_DATA3;
    data_gpio_nums[4]  = LCD_PIN_NUM_DATA4;
    data_gpio_nums[5]  = LCD_PIN_NUM_DATA5;
    data_gpio_nums[6]  = LCD_PIN_NUM_DATA6;
    data_gpio_nums[7]  = LCD_PIN_NUM_DATA7;
    data_gpio_nums[8]  = LCD_PIN_NUM_DATA8;
    data_gpio_nums[9]  = LCD_PIN_NUM_DATA9;
    data_gpio_nums[10] = LCD_PIN_NUM_DATA10;
    data_gpio_nums[11] = LCD_PIN_NUM_DATA11;
    data_gpio_nums[12] = LCD_PIN_NUM_DATA12;
    data_gpio_nums[13] = LCD_PIN_NUM_DATA13;
    data_gpio_nums[14] = LCD_PIN_NUM_DATA14;
    data_gpio_nums[15] = LCD_PIN_NUM_DATA15;
}

/**
 * @brief       新建rgblcd句柄,并配置参数
 * @param       panel_handle:RGB LCD屏句柄
 * @retval      ESP_OK:创建成功
 */
static esp_err_t rgb_lcd_new_panel(esp_lcd_panel_handle_t *panel_handle)
{
    ESP_RETURN_ON_FALSE(panel_handle, ESP_ERR_INVALID_ARG, rgb_lcd_tag, "invalid argument");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src               = LCD_CLK_SRC_PLL160M,     /* 时钟源 */
        /* ★★★ 11-21 实验记录（不要再动）：改 128 后屏幕"闪得更明显 + 小区域失真"。
         *   原因：每行 = 800 × 2 = **1600 字节**，1600/128 = 12.5 ⇒ 半个突发跨行，
         *   行尾数据错位；1600/64 = 25 正好整除。⇒ 64 是逐行扫描的最优值。 */
        .dma_burst_size        = 64,
        .data_width            = 16,                      /* RGB565并行16线 */
        .in_color_format       = LCD_COLOR_FMT_RGB565,    /* 颜色格式 */
        .num_fbs               = LCD_RGB_PANEL_NUM_FBS,   /* 帧缓冲区数量 */
        /* bounce buffer 实测更差（中断搬数跟不上 25 MHz 的扫描速度，边缘更闪），不启用 */
        .disp_gpio_num         = -1,                      /* -1:不使用显示使能引脚 */
        .pclk_gpio_num         = LCD_PIN_NUM_PCLK,        /* PCLK引脚 */
        .vsync_gpio_num        = LCD_PIN_NUM_VSYNC,       /* VSYNC引脚 */
        .hsync_gpio_num        = LCD_PIN_NUM_HSYNC,       /* HSYNC引脚 */
        .de_gpio_num           = LCD_PIN_NUM_DE,          /* DE引脚 */
        .timings = {
            .pclk_hz           = LCD_PIXEL_CLOCK_HZ,      /* 像素时钟 */
            .h_res             = LCD_H_RES,               /* 水平分辨率 */
            .v_res             = LCD_V_RES,               /* 垂直分辨率 */
            .hsync_pulse_width = LCD_HSYNC_WIDTH,         /* 水平同步宽度 */
            .hsync_back_porch  = LCD_HBP,                 /* 水平后廊 */
            .hsync_front_porch = LCD_HFP,                 /* 水平前廊 */
            .vsync_pulse_width = LCD_VSYNC_WIDTH,         /* 垂直同步宽度 */
            .vsync_back_porch  = LCD_VBP,                 /* 垂直后廊 */
            .vsync_front_porch = LCD_VFP,                 /* 垂直前廊 */
            .flags = {
                /* ★★★ 11-22：40MHz（零抖动档）下右侧出现 1~2px 白色竖线 = 行尾采不住数据，
                 *   是超规格压缩建立/保持裕量的典型症状。换采样沿把裕量挪到另一边试一次：
                 *   false = 在 PCLK 上升沿采样（面板侧），40MHz 下裕量不够；
                 *   true  = 挪到下降沿。
                 *   ★ 25MHz 时代测过 true/false"都正常显示"，当时与 flicker 无关、不能作参考。 */
                .pclk_active_neg = true,
            },
        },
        .flags.fb_in_psram = true,                        /* 帧缓冲分配在PSRAM */
    };

    rgb_lcd_fill_data_gpio_nums(panel_config.data_gpio_nums);   /* 填充数据线引脚 */

    return esp_lcd_new_rgb_panel(&panel_config, panel_handle);
}

/**
 * @brief       rgb_lcd初始化
 * @param       无
 * @retval      LCD控制句柄
 */
esp_lcd_panel_handle_t rgb_lcd_init(void)
{
    esp_lcd_panel_handle_t rgb_lcd_ctrl_panel;                  /* RGB LCD控制句柄 */

    ESP_ERROR_CHECK(rgb_lcd_new_panel(&rgb_lcd_ctrl_panel));    /* 新建RGB LCD面板 */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(rgb_lcd_ctrl_panel));   /* 复位RGB LCD屏 */
    ESP_ERROR_CHECK(esp_lcd_panel_init(rgb_lcd_ctrl_panel));    /* 初始化RGB LCD屏(启动DMA) */

    lcddev.id     = 0x8264;                                     /* HX8264屏幕(无ID寄存器,用作屏类型标识) */
    lcddev.width  = LCD_H_RES;                                  /* 宽度 */
    lcddev.height = LCD_V_RES;                                  /* 高度 */
    lcddev.dir    = 1;                                          /* 1,横屏 */
    ESP_LOGI(rgb_lcd_tag, "rgblcd_id:%#x ", lcddev.id);         /* 打印RGBLCD的ID */

    /* ★★★ 11-22 把这套"反直觉时序"打到日志里，改任何一项都能立刻算出新帧率，
     *   避免以后有人"为了保险"把 PCLK 调回 25MHz 又把 flicker 弄回来：
     *     PCLK 40MHz（160÷2÷2 整数分频，零抖动）+ H1120×V576 ⇒ 62.0fps + 采样沿 true。
     *   总周期 = 1120 × 576 = 645120，40e6 / 645120 = 62.00fps。 */
    {
        const int htot = LCD_H_RES + LCD_HSYNC_WIDTH + LCD_HBP + LCD_HFP;
        const int vtot = LCD_V_RES + LCD_VSYNC_WIDTH + LCD_VBP + LCD_VFP;
        ESP_LOGI(rgb_lcd_tag,
                 "PCLK %dMHz | total %dx%d | %.2ffps | pclk_active_neg=%d | 40MHz must keep pclk_neg=1",
                 (int)LCD_PCLK_MHZ, htot, vtot,
                 (double)LCD_PIXEL_CLOCK_HZ / ((double)htot * (double)vtot),
                 (int)1);
    }

    return rgb_lcd_ctrl_panel;
}
