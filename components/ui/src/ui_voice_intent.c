/**
 * @file    ui_voice_intent.c
 * @brief   语音文本 → 本地动作（关键词意图识别）
 *
 * 定位：语音链路的最上一层。voice 组件只负责「声音 → 文本」（不依赖 UI），
 * 这里负责「文本 → 动作」，因此放在 UI 层（可引用 ui_scenes.h / backlight.h）。
 *
 * ★ 为什么不起 LLM：本地关键词匹配零延迟、零成本、断网可用，覆盖 90% 的固定指令。
 *   将来接 AI（自然语言理解）时，把这个函数作为 LLM 失败后的兜底即可。
 */

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "ui_voice_intent.h"
#include "ui_scenes.h"
#include "app_cfg.h"
#include "backlight.h"

static const char *TAG = "vintent";

/* 关键词表：按顺序匹配，**长词/具体词必须排在前面**
 * （例："打开屏保" 命中"屏保"，不会被别的词抢走） */
typedef struct
{
    const char *kw;
    int         scene;      /* 场景索引，-1 = 不是场景动作 */
    int         bright;     /* 亮度增量，0 = 不是亮度动作 */
} vintent_rule_t;

static const vintent_rule_t s_rules[] = {
    /* ---- 场景（词越具体越靠前）---- */
    { "屏保",     UI_SCENE_CLOCK,   0 },
    { "壁纸",     UI_SCENE_CLOCK,   0 },   /* 实测用户会说"进入壁纸" */
    { "时钟",     UI_SCENE_CLOCK,   0 },
    { "待机",     UI_SCENE_CLOCK,   0 },
    { "天气",     UI_SCENE_HOME,    0 },
    { "主页",     UI_SCENE_HOME,    0 },
    { "电脑",     UI_SCENE_MONITOR, 0 },
    { "监控",     UI_SCENE_MONITOR, 0 },
    { "性能",     UI_SCENE_MONITOR, 0 },
    { "音乐",     UI_SCENE_AUDIO,   0 },
    { "播放",     UI_SCENE_AUDIO,   0 },
    { "音频",     UI_SCENE_AUDIO,   0 },
    { "系统",     UI_SCENE_SYSTEM,  0 },
    { "设置",     UI_SCENE_SYSTEM,  0 },
    { "应用",     UI_SCENE_APPS,    0 },
    { "程序",     UI_SCENE_APPS,    0 },
    { "热榜",     UI_SCENE_HF,      0 },
    { "模型",     UI_SCENE_HF,      0 },
    { "排行",     UI_SCENE_AIRANK,  0 },
    { "榜单",     UI_SCENE_AIRANK,  0 },
    { "B站",      UI_SCENE_BILI,    0 },
    { "热搜",     UI_SCENE_BILI,    0 },

    /* ---- 亮度（"亮"单独的词太容易误命中，这里只在明确语境下用）---- */
    { "亮一点",   -1,  10 },
    { "亮一些",   -1,  10 },
    { "调亮",     -1,  10 },
    { "太暗",     -1,  10 },
    { "暗一点",   -1, -10 },
    { "暗一些",   -1, -10 },
    { "调暗",     -1, -10 },
    { "太亮",     -1, -10 },
};

#define RULE_N  (int)(sizeof(s_rules) / sizeof(s_rules[0]))

bool ui_voice_intent(const char *text, ui_vact_t *act, int *arg)
{
    if (text == NULL || act == NULL || arg == NULL || text[0] == '\0')
    {
        return false;
    }

    for (int i = 0; i < RULE_N; i++)
    {
        if (strstr(text, s_rules[i].kw) != NULL)
        {
            if (s_rules[i].scene >= 0)
            {
                *act = UI_VACT_SCENE;
                *arg = s_rules[i].scene;
            }
            else
            {
                *act = (s_rules[i].bright > 0) ? UI_VACT_BRIGHT_UP : UI_VACT_BRIGHT_DOWN;
                *arg = s_rules[i].bright;
            }
            ESP_LOGI(TAG, "intent: \"%s\" -> kw \"%s\"", text, s_rules[i].kw);
            return true;
        }
    }

    ESP_LOGI(TAG, "intent: \"%s\" -> no match", text);
    return false;
}

void ui_voice_intent_apply(ui_vact_t act, int arg)
{
    switch (act)
    {
    case UI_VACT_SCENE:
        ESP_LOGI(TAG, "action: goto scene %d", arg);
        ui_goto_scene(arg);
        break;

    case UI_VACT_BRIGHT_UP:
    case UI_VACT_BRIGHT_DOWN:
    {
        int cur = backlight_get();
        int tgt = cur + arg;
        if (tgt < 10)  { tgt = 10; }    /* 不压到 0：全灭看不见屏，用户会以为死机 */
        if (tgt > 100) { tgt = 100; }
        backlight_set(tgt);
        /* 持久化：下次上电沿用（配网页滑条读同一字段）
         * ★ 必须用 app_cfg_mut()：app_cfg_get() 返回 const，直接改会触发
         *   -Werror=discarded-qualifiers。 */
        app_cfg_t *c = app_cfg_mut();
        if (c != NULL)
        {
            c->backlight = (uint8_t)tgt;
            app_cfg_save();
        }
        ESP_LOGI(TAG, "action: brightness %d%% -> %d%%", cur, tgt);
        break;
    }

    default:
        break;
    }
}
