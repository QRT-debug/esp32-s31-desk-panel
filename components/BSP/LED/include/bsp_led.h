/**
 * @file    bsp_led.h
 * @brief   板载 WS2812 彩灯驱动（IO37，单线串行，RMT 外设）
 *
 * 板上灯珠数量未确认（原理图待核）：驱动按 4 颗串级发同色，
 * 实际几颗亮几颗，多余数据对不存在的灯珠无害。
 *
 * 11-20：先做系统页调色测试（HUE 滑块），后续按用途做成状态机。
 */
#pragma once

#include <stdint.h>

/* 初始化 RMT + 通道（重复调用安全） */
void bsp_led_init(void);

/* HSV 调色：hue 0~359，sat/val 0~255（val=0 即灭）——全部灯珠同色 */
void bsp_led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/* 全灭 */
void bsp_led_off(void);
