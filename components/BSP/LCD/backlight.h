/**
 * @file    backlight.h
 * @brief   LCD 背光 PWM 调光
 *
 * ★ 硬件（原理图实测坐标）：LCD 连接器 pin34 = `BL_CTR` = 模组 pin49 = **IO38**。
 *   （注意：`LED_CTR`(IO37) 不是背光，它经 Q3 P-MOS 控制模组上的 WS2812 彩灯。）
 *
 * 频率选 5 kHz：低于 1 kHz 会看到闪烁，也可能串进音频；
 * 占空比直接控制平均亮度（背光由模组内部恒流驱动，峰值电流不变）。
 */
#pragma once

#include "esp_err.h"

/** 初始化 PWM（★ 只配置不点亮，duty=0；首帧后调 backlight_default 点亮） */
esp_err_t backlight_init(void);

/** ★ LVGL 首帧 flush 完成后调用：按默认亮度（75%）点亮背光 */
void backlight_default(void);

/** 设置亮度 0~100（%）。0 = 熄灭（注意：全灭后屏幕内容看不见） */
void backlight_set(int percent);

/** 当前亮度（%） */
int backlight_get(void);
