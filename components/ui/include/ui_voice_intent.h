/**
 * @file    ui_voice_intent.h
 * @brief   语音文本 → 本地动作（关键词意图识别）
 *
 * 用法（必须在 LVGL 上下文执行 apply，避免跨线程碰 UI）：
 *     ui_vact_t act; int arg;
 *     if (ui_voice_intent(text, &act, &arg)) {
 *         ui_voice_intent_apply(act, arg);
 *     }
 */

#pragma once

#include <stdbool.h>

typedef enum
{
    UI_VACT_NONE = 0,
    UI_VACT_SCENE,        /**< 切场景，arg = UI_SCENE_xxx */
    UI_VACT_BRIGHT_UP,    /**< 背光调亮，arg = 增量 */
    UI_VACT_BRIGHT_DOWN,  /**< 背光调暗，arg = 负增量 */
} ui_vact_t;

/**
 * 解析语音文本，命中返回 true 并填 act/arg。
 * ★ 纯函数，无副作用，可在任意线程调用。
 */
bool ui_voice_intent(const char *text, ui_vact_t *act, int *arg);

/**
 * 执行动作。★ 会调用 LVGL/背光接口，请在 LVGL 上下文（场景 draw）里调用。
 */
void ui_voice_intent_apply(ui_vact_t act, int arg);
