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
/* ★★★ 11-22 诊断结论：PCLK 10/16/20/25MHz 全试过——
 *   10MHz **更闪且失真**（低于 HX8264 最低 PCLK，且帧率低让闪更显眼），
 *   20/16MHz 无改善 ⇒ **与时钟频率无关**，不是时序余量/带宽问题。
 *   恢复商家验证值 25MHz。 */
#define LCD_PCLK_MHZ            25
#endif
#define    LCD_PIXEL_CLOCK_HZ      (LCD_PCLK_MHZ * 1000 * 1000)
#define    LCD_H_RES               800                   /* 水平有效像素 */
#define    LCD_V_RES               480                   /* 垂直有效行数 */
#define    LCD_HSYNC_WIDTH         48                    /* 水平同步脉冲宽度(HSW) */
/* ★★★ 11-22 用"加大 porch"把 40MHz 的帧率压回 62Hz —— 这是解开死结的关键一步。
 *
 *   死结：零抖动的 PCLK 只有 160/(2×PCLK) 为整数的档位 ⇒ 40 / 26.67 / 20 MHz，
 *         对应帧率 77.6 / 51.7 / 38.8Hz，中间没有 60~65Hz 的选项。
 *   绕法：**帧率 = PCLK / (H_total × V_total)**，帧率不必等于 PCLK 的极限。
 *         保持 PCLK 40MHz（零抖动），把总周期撑大到 1120×576 ⇒ 40e6/645120 = **62.0Hz**。
 *   附加收益：有效区只占 59.5%，每帧有 40% 的空闲，DMA 不再连轴转，CPU 的 PSRAM
 *         访问不会被持续仲裁延迟（40MHz+小porch 时 render 从 24ms 恶化到 49ms 正是这个原因）。
 *
 *   ★ 必须满足的约束：**H_total 是 32 的倍数**（行宽字节数 = H_total×2 要能被 DMA
 *     burst 64 整除）。1120×2 = 2240，2240/64 = 35 ✔
 *     ⇒ 之前"加大 porch 就失真"的真因就是 H_total 变成 1008，2016/64 = 31.5 不整除，
 *        DMA 突发跨行错位。不是 porch 本身不能加。
 *
 *   62.0Hz 仍高于人眼临界闪烁频率；40MHz 零抖动 ⇒ 波浪与闪烁应同时消失。
 *
 * ★★★ 11-22 终值修正 62.0 → **66.0Hz**：62Hz 下"又有频闪"。
 *   ★ 关键变量是**亮度**：临界闪烁频率 CFF 随亮度上升。背光从 75% 提到 100% 之后
 *     画面更亮 ⇒ CFF 抬高 ⇒ 62Hz 落在临界线上，主观感受时有时无。
 *     用户实测：100% 亮度下 64Hz = "几乎看不出来"，62Hz = "又有频闪"。
 *   ⇒ 取 **66.0Hz** 留 6% 余量：40e6 / (1120 × 541) = 66.02Hz。
 *   V_total 576→541（垂直消隐 96→61 行），有效区占比 59.5%→63.4%，
 *   DMA 空闲从 40% 降到 36.6%，仍然够（render 实测 24ms 不变）。 */
#define    LCD_HBP                 160                   /* 水平后廊(HBP) */
#define    LCD_HFP                 112                   /* 水平前廊(HFP) */
#define    LCD_VSYNC_WIDTH         3                     /* 垂直同步脉冲宽度(VSW) */
#define    LCD_VBP                 32                    /* 垂直后廊(VBP) */
#define    LCD_VFP                 26                    /* 垂直前廊(VFP) */

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
