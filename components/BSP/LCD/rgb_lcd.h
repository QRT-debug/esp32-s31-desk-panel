/**
 ******************************************************************************
 * @file        rgb_lcd.h
 * @version     V1.0
 * @brief       rgblcd驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __RGB_LCD_H
#define __RGB_LCD_H

#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include <math.h>
#include <string.h>

/* RGB LCD时序参数(800*480)
 * ★ PCLK 可在 main/CMakeLists.txt 里用 LCD_PCLK_MHZ 覆盖，默认 25MHz（商家验证值）。
 *   25MHz -> 976x528 总周期 -> 48.5Hz
 *   30MHz -> 58.2Hz，33MHz -> 64.0Hz（均超商家验证值，可能出白条，用于探边界）
 */
#ifndef LCD_PCLK_MHZ
#define LCD_PCLK_MHZ            25
#endif
#define    LCD_PIXEL_CLOCK_HZ      (LCD_PCLK_MHZ * 1000 * 1000)
#define    LCD_H_RES               800                   /* 水平有效像素 */
#define    LCD_V_RES               480                   /* 垂直有效行数 */
#define    LCD_HSYNC_WIDTH         48                    /* 水平同步脉冲宽度(HSW) */
#define    LCD_HBP                 88                    /* 水平后廊(HBP) */
#define    LCD_HFP                 40                    /* 水平前廊(HFP) */
#define    LCD_VSYNC_WIDTH         3                     /* 垂直同步脉冲宽度(VSW) */
#define    LCD_VBP                 32                    /* 垂直后廊(VBP) */
#define    LCD_VFP                 13                    /* 垂直前廊(VFP) */

/* 帧缓冲区数量
 * ★ 保持 2：bounce buffer 模式实测更差（中断搬数跟不上，边缘更闪），已回退。
 *   LVGL 只用 buffer[0] 直写，buffer[1] 闲置。 */
#define    LCD_RGB_PANEL_NUM_FBS   2

/* RGB同步信号(IO MUX直连引脚) */
#define    LCD_PIN_NUM_HSYNC       GPIO_NUM_44           /* 行同步信号 */
#define    LCD_PIN_NUM_VSYNC       GPIO_NUM_45           /* 场同步信号 */
#define    LCD_PIN_NUM_DE          GPIO_NUM_43           /* 数据使能信号 */
#define    LCD_PIN_NUM_PCLK        GPIO_NUM_40           /* 像素时钟 */

/* RGB 16bit数据线
 * DATA[4:0]   = B[7:3]  (蓝色)
 * DATA[10:5]  = G[7:2]  (绿色)
 * DATA[15:11] = R[7:3]  (红色)
 */
#define    LCD_PIN_NUM_DATA0       GPIO_NUM_8           /* 蓝色B3 */
#define    LCD_PIN_NUM_DATA1       GPIO_NUM_9           /* 蓝色B4 */
#define    LCD_PIN_NUM_DATA2       GPIO_NUM_10          /* 蓝色B5 */
#define    LCD_PIN_NUM_DATA3       GPIO_NUM_11          /* 蓝色B6 */
#define    LCD_PIN_NUM_DATA4       GPIO_NUM_12          /* 蓝色B7 */
#define    LCD_PIN_NUM_DATA5       GPIO_NUM_13          /* 绿色G2 */
#define    LCD_PIN_NUM_DATA6       GPIO_NUM_14          /* 绿色G3 */
#define    LCD_PIN_NUM_DATA7       GPIO_NUM_15          /* 绿色G4 */
#define    LCD_PIN_NUM_DATA8       GPIO_NUM_16          /* 绿色G5 */
#define    LCD_PIN_NUM_DATA9       GPIO_NUM_17          /* 绿色G6 */
#define    LCD_PIN_NUM_DATA10      GPIO_NUM_18          /* 绿色G7 */
#define    LCD_PIN_NUM_DATA11      GPIO_NUM_19          /* 红色R3 */
#define    LCD_PIN_NUM_DATA12      GPIO_NUM_33          /* 红色R4 */
#define    LCD_PIN_NUM_DATA13      GPIO_NUM_34          /* 红色R5 */
#define    LCD_PIN_NUM_DATA14      GPIO_NUM_36          /* 红色R6 */
#define    LCD_PIN_NUM_DATA15      GPIO_NUM_35          /* 红色R7 */

/*函数声明*/
esp_lcd_panel_handle_t rgb_lcd_init(void);              /* rgb_lcd初始化 */

#endif
