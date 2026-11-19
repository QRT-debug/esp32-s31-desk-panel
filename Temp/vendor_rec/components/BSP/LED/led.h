/**
 ******************************************************************************
 * @file        led.h
 * @version     V1.0
 * @brief       WS2812B协议 LED驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __LED_H
#define __LED_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*引脚定义*/
#define LED_GPIO_PIN             37                               /*WS2812B连接的GPIO*/
#define LED_NUM                  1                                /*LED数量*/

/*常用颜色定义*/
#define    LED_RGB_BLACK         0x000000                         /*黑色*/
#define    LED_RGB_RED           0xFF0000                         /*红色*/
#define    LED_RGB_GREEN         0x00FF00                         /*绿色*/
#define    LED_RGB_BLUE          0x0000FF                         /*蓝色*/
#define    LED_RGB_WHITE         0xFFFFFF                         /*白色*/
#define    LED_RGB_YELLOW        0xFFFF00                         /*黄色*/
#define    LED_RGB_CYAN          0x00FFFF                         /*青色*/
#define    LED_RGB_MAGENTA       0xFF00FF                         /*品红色*/

#define    LED_BRIGHTNESS        10                               /*设置LED亮度（0~255 数值越大亮度越高）*/

/*函数声明*/ 
void led_init(void);                                              /*初始化LED*/
void led_set_color(uint32_t red, uint32_t green, uint32_t blue);  /*设置LED颜色*/
void led_set_rgb(uint32_t rgb);                                   /*设置LED颜色*/
void led_blink(void);                                             /*闪烁LED (默认绿)*/
void led_blink_color(uint32_t rgb);                               /*闪烁LED (指定颜色)*/
void led_off(void);                                               /*关闭LED*/

#endif

