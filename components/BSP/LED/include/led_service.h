/**
 * @file    led_service.h
 * @brief   板载 WS2812 彩灯的**公共控制服务**（任何上层都能调用）
 *
 * 设计意图：原来 LED 只被 SYSTEM 页的调色滑条控制，属于"UI 私有功能"。
 * 现在抽成服务层，让多个上层按各自语义使用：
 *   - 语音：录音时呼吸（提示"正在听"）
 *   - 通知：新消息闪两下
 *   - 屏保：夜间呼吸灯
 *   - 系统：状态指示（WiFi 断开 = 红色慢闪）
 *
 * 语义约定（重要，避免互相打架）：
 *   - `led_set_rgb()` 设置**底色**，持续到下一次设置；
 *   - `led_flash()` / `led_breath()` 是**临时效果**，结束后**自动恢复底色**——
 *     上层不需要自己"记得恢复"，效果结束即回到常态；
 *   - 后发起的临时效果会覆盖前一个（最后调用者获胜），适合"高优先级提示打断低优先级"。
 *
 * ★ 线程安全：所有接口可从任意任务调用（内部互斥 + 独立刷新任务驱动，10ms tick）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/** 颜色用 0xRRGGBB 表示 */
typedef uint32_t led_rgb_t;

/** 服务初始化（惰性调用安全；建议 main 里显式调一次） */
void led_service_init(void);

/** 设置底色（常亮），持续到下次设置或 led_off() */
void led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/** 设置底色（HSV：hue 0~359，sat/val 0~255） */
void led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/** 关灯（不改变底色；随后的临时效果结束后会恢复到底色 → 仍为灭） */
void led_off(void);

/**
 * 闪烁提示：以 (r,g,b) 亮 on_ms、灭 off_ms，重复 times 次，结束后恢复底色。
 * times=0 视为 1 次。非阻塞，立即返回。
 */
void led_flash(uint8_t r, uint8_t g, uint8_t b, int on_ms, int off_ms, int times);

/**
 * 呼吸灯：亮度在 0~val 之间按 period_ms 一个来回。period_ms=0 停止呼吸并恢复底色。
 * 适合"正在监听""待机"这类持续性状态。
 */
void led_breath(uint8_t r, uint8_t g, uint8_t b, int period_ms);

/** 立即停止任何临时效果并恢复底色 */
void led_stop_effect(void);

/** 取当前底色（0xRRGGBB） */
led_rgb_t led_get_base_rgb(void);

/** 是否有临时效果在跑 */
bool led_is_effect_running(void);
