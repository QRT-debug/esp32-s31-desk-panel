/**
 ******************************************************************************
 * @file        text.h
 * @version     V1.0
 * @brief       汉字显示 代码
 *              提供text_show_font和text_show_string两个函数,用于显示汉字
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __TEXT_H
#define __TEXT_H

#include "fonts.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "ff.h"
#include "lcd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* 函数声明 */
void text_show_font(uint16_t x, uint16_t y, uint8_t *font, uint8_t size, uint8_t mode, uint32_t color);                                 /* 显示汉字 */
void text_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, char *str, uint8_t size, uint8_t mode, uint32_t color);  /* 显示汉字字符串 */
void text_show_string_middle(uint16_t x, uint16_t y, char *str, uint8_t size, uint16_t width, uint32_t color);                          /* 居中显示汉字字符串 */

#endif
