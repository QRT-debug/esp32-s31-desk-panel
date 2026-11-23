#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 语音服务（基础设施层）
 *
 * 定位：只负责"听 → 出一段文本"，不关心上层拿文本去干什么。
 *   ① 任务输入（发给 PC 执行）
 *   ② AI 对话（发给对话引擎）
 *   ③ 未来的 agent
 * 三者都通过 voice_subscribe() 订阅，互不影响；用 voice_set_mode() 决定当前归谁。
 *
 * 硬件链：ES8389 ADC → I2S RX 16kHz → 能量 VAD 断句 → WAV → HTTP POST /api/asr → 文本
 */

typedef enum
{
    VOICE_EV_SPEECH_START,   /* 检测到说话（电平超过阈值） */
    VOICE_EV_SPEECH_END,     /* 一句话结束（静音超时/达到上限），随后开始上传 */
    VOICE_EV_TEXT,           /* ASR 返回文本（evt->text 有效） */
    VOICE_EV_ERROR,          /* 上传/识别失败（evt->text 是原因） */
} voice_ev_t;

typedef struct
{
    voice_ev_t  type;
    const char *text;
    int         text_len;
} voice_evt_t;

/** 语音模式：决定一次识别结果归哪个订阅者有资格处理（避免多方抢） */
typedef enum
{
    VOICE_MODE_TASK = 0,   /* 任务下发 */
    VOICE_MODE_CHAT,       /* AI 对话 */
} voice_mode_t;

typedef void (*voice_cb_t)(const voice_evt_t *evt);

/** 初始化（开机调用一次）：建录音缓冲 + 启动识别任务 */
esp_err_t voice_init(void);

/** 订阅事件（可多个订阅者；模式过滤在事件里带 mode 由上层判断） */
int voice_subscribe(voice_cb_t cb);

/** 设置当前模式（配网页/UI 切换时调用） */
void voice_set_mode(voice_mode_t m);
voice_mode_t voice_get_mode(void);

/**
 * 触发一次"听一句话"（按钮/触摸/唤醒词都可以调）。
 * 已经在录音时重复调用会被忽略。返回 false = 设备忙（放歌中/已在录）。
 */
bool voice_listen(void);

/** 是否正在录音（UI 可用来显示状态） */
bool voice_is_listening(void);

#ifdef __cplusplus
}
#endif
