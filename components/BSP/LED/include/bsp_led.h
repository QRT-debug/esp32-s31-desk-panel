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

/**
 * 按**感知线性 RGB**（0~255）输出，内部做 gamma 校正 + 通道级时间抖动。
 *
 * ★ 为什么需要这个入口：gamma 之后 PWM 只剩 0~48 级（val=140 时），
 *   直接整级输出会有明显台阶。这里把不足一级的余数累计到下一帧
 *   （200Hz 下人眼积分成中间亮度），等效精度远超 8bit。
 *   需要做渐变/呼吸的调用方应走这个接口，而不是自己先转 RGB 再 gamma。
 */
void bsp_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* 全灭 */
void bsp_led_off(void);
