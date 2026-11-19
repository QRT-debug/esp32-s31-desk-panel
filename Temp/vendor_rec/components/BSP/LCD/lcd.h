/**
 ******************************************************************************
 * @file        lcd.h
 * @version     V1.0
 * @brief       lcd驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */
#ifndef __LCD_H
#define __LCD_H

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "rgb_lcd.h"

/*定义背光引脚*/
#define   LCD_BL_PIN     GPIO_NUM_38

/*LCD_BL*/
#define  LCD_BL(x)       do{ x ?                             \
                             gpio_set_level(LCD_BL_PIN, 1):  \
                             gpio_set_level(LCD_BL_PIN, 0);  \
                         }while(0)

/*常用颜色值*/
#define     WHITE           0xFFFF      /*白色*/
#define     BLACK           0x0000      /*黑色*/
#define     RED             0xF800      /*红色*/
#define     GREEN           0x07E0      /*绿色*/
#define     BLUE            0x001F      /*蓝色*/
#define     MAGENTA         0xF81F      /*品红色*/
#define     YELLOW          0xFFE0      /*黄色*/
#define     CYAN            0x07FF      /*青色*/

/* 非常用颜色 */
#define     BROWN           0XBC40      /* 棕色 */
#define     BRRED           0XFC07      /* 棕红色 */
#define     GRAY            0X8430      /* 灰色 */
#define     DARKBLUE        0X01CF      /* 深蓝色 */
#define     LIGHTBLUE       0X7D7C      /* 浅蓝色 */
#define     GRAYBLUE        0X5458      /* 灰蓝色 */
#define     LIGHTGREEN      0X841F      /* 浅绿色 */
#define     LGRAY           0XC618      /* 浅灰色 */
#define     LGRAYBLUE       0XA651      /* 浅灰蓝色 */
#define     LBBLUE          0X2B12      /* 浅棕蓝色 */

/*LCD重要参数集*/
typedef struct
{
    uint32_t id;                             /*LCD设备ID*/
    uint32_t width;                          /*面板宽度（单位：像素）*/
    uint32_t height;                         /*面板高度（单位：像素）*/
    uint32_t dir;                            /*显示方向*/
    uint32_t color_byte;                     /*颜色格式*/
    esp_lcd_panel_handle_t lcd_panel_handle; /*LCD控制句柄*/
    struct lcd
    {
        int lcd_rst;                         /* 复位引脚(RGB屏无复位引脚,固定为-1) */
        int lcd_bl;                          /* 背光引脚 */
    }ctrl;   
}_lcd_dev;

/* LCD参数 */
extern _lcd_dev lcddev; /* 管理LCD重要参数 */
extern uint32_t g_back_color; /* 背景色(文本非叠加模式用它填充) */

/* 函数声明 */
void lcd_init(void);                                                                                 /* lcd初始化函数 */
void lcd_clear(uint16_t color);                                                                      /* 清屏函数 */
void lcd_draw_point(uint16_t x, uint16_t y, uint16_t color);                                         /* 画点函数 */
void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);                   /* 填充函数 */
void lcd_color_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t *color);            /* 填充颜色块函数 */
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);              /* 画线函数 */
void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);                           /* 画水平线函数 */
void lcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,uint16_t color);          /* 画矩形函数 */
void lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);                           /* 画圆函数 */
void lcd_fill_circle(uint16_t center_x, uint16_t center_y, uint16_t radius, uint16_t color);         /* 画实心圆函数 */
void lcd_show_char(uint16_t x, uint16_t y, char chr, uint8_t size, uint8_t mode, uint16_t color);    /* 显示字符函数 */
void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color);  /* 显示数字函数 */
void lcd_show_xnum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode, uint16_t color);      /* 扩展显示len个数字 */
void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color);   /* 显示字符串函数 */

#endif