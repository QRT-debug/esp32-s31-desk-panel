/**
 * @file    ui_manager.c
 * @brief   场景表 + 调度
 *
 * 现在的调度模型（2026-11-16 起改为手机式导航）：
 *   全部界面都是 LVGL（self_managed），导航只靠界面内按钮（ui_goto_scene）。
 *   manager 的职责退化成三件事：
 *     1. 每周期调 sc->draw()（内部跑 lv_timer_handler）
 *     2. 丢掉触摸事件队列（LVGL 用 touch_get_last 轮询状态，队列没用了）
 *     3. 检查 ui_goto_scene 设置的目标并切换
 *
 * 旧的"点左/右半屏切换 + 自动轮播"只保留给非 LVGL 测试场景（已关）。
 */
#include "ui_scenes.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"
#include "touch_service.h"
#include "ui_draw.h"

static const char *TAG = "ui";

/* ★ 1 = 保留 9 个硬件测试画面（测屏用）；0 = 只显示正式界面 */
#define ENABLE_TEST_SCENES 0

/* hold_ms 现在的含义（self_managed 场景）：manager 循环周期。
 *   20ms  → LVGL 50Hz：滑块拖动顺滑（音频页）
 *   100ms → 监控页数据本来就 1Hz，省 CPU */
const ui_scene_t g_scenes[] = {
    { "WEATHER",    scene_weather, 20, -1, false, true },
    { "PC MONITOR", scene_monitor, 100, -1, false, true },
    { "AUDIO",      scene_audio,   20, -1, false, true },
    { "SYSTEM",     scene_system,   20, -1, false, true },
    { "APPS",       scene_home,    20, -1, false, true },
    { "HF TREND",   scene_hf,      50, -1, false, true },
    { "AI RANK",    scene_ai_rank, 50, -1, false, true },
    { "BILI HOT",   scene_bili,    50, -1, false, true },
    /* ★★ hold_ms 必须与其他页同量级（20~50ms）：LVGL 的触摸事件处理只在每帧
     *   draw() 里跑（lv_timer_handler 在其中），hold_ms=1000 ⇒ 时钟页每 1 秒才
     *   处理一次触摸——轻点 100ms 落在空窗必丢、长按 2s 必中（实测"长按灵轻点不灵"）。
     *   时间标签内部自带秒变化判断，高频空跑无害；脏区不变时 refr 也几乎零开销。 */
    { "CLOCK",      scene_clock,   20, -1, false, true },
};
const int g_scene_count = (int)(sizeof(g_scenes) / sizeof(g_scenes[0]));

static volatile int s_goto = -1;
static volatile int s_cur  = UI_SCENE_HOME;
static int64_t     s_last_touch_ms = 0;   /* 最后触摸时刻（ms，esp_timer） */
#define SCRSAVER_AFTER_MS  (5LL * 60 * 1000)   /* 无触摸 5 分钟进屏保 */   /* 当前场景：给时钟页"返回"用 */

int ui_current_scene(void)
{
    return s_cur;
}

void ui_goto_scene(int idx)
{
    if (idx >= 0 && idx < g_scene_count)
    {
        s_goto = idx;
    }
}

/* ---- 旧的半屏触摸切换逻辑：只服务非 LVGL 测试场景 ---- */

static bool poll_events(int *idx, int64_t *last_touch)
{
    touch_evt_t e;
    bool switched = false;

    while (touch_get_event(&e))
    {
        if (e.type != TOUCH_EV_DOWN)
        {
            continue;
        }

        *last_touch = esp_timer_get_time();

        g_touch_last_x = e.x;
        g_touch_last_y = e.y;

        if (e.x > UI_W / 2)
        {
            *idx = (*idx + 1) % g_scene_count;
        }
        else
        {
            *idx = (*idx + g_scene_count - 1) % g_scene_count;
        }

        ESP_LOGI(TAG, "tap (%u,%u) -> scene %d", e.x, e.y, *idx + 1);
        switched = true;
    }

    return switched;
}

static bool wait_with_events(int duration_ms, int *idx, int64_t *last_touch)
{
    int64_t t0 = esp_timer_get_time();
    int64_t d  = (int64_t)duration_ms * 1000;

    while ((esp_timer_get_time() - t0) < d)
    {
        if (poll_events(idx, last_touch))
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* ---- 主循环 ---- */

void ui_manager_run(void)
{
    /* 日志环形缓冲：接管 esp_log，给"系统信息"页显示用（只调一次） */
    ui_logbuf_init();

    /* ★ 状态栏最先建：这样开机就能看到"未连接=全灰 → 连上逐格点亮"的过程，
     *   而不是等 splash（最长 15 秒）结束才第一次出现。 */
    ui_statusbar_init();

    /* 开机初始化画面：等 WiFi 连上 + 拿到第一帧 PC 数据（或超时 15 秒） */
    ui_splash_run();

    /* 丢掉切换前的残留事件 */
    {
        touch_evt_t e;
        while (touch_get_event(&e)) { }
    }

    int idx   = UI_SCENE_HOME;
    int frame = 0;

    while (1)
    {
        const ui_scene_t *sc = &g_scenes[idx];
        s_cur = idx;

        if (sc->self_managed)
        {
            /* ---- LVGL 场景：按钮导航 ---- */
            sc->draw(NULL, frame++);

            /* 队列丢弃：LVGL 的 indev 走 touch_get_last 轮询，队列积压无意义，
             * 且不清会挡住"半屏 tap 切换"（对 LVGL 场景反而是干扰） */
            touch_evt_t e;
            while (touch_get_event(&e)) { }

            /* ---- 屏保：无触摸 5 分钟自动进时钟页 ----
             * ★ 只记"当前按住"（短按在 DOWN 一瞬为 true），click/长按语义无关紧要。
             * ★ 时钟页自身不触发（否则进去 5 分钟又被切走）；触摸返回后重新计时。 */
            {
                uint16_t tx, ty;
                bool     tp;
                touch_get_last(&tx, &ty, &tp);
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (tp)
                {
                    s_last_touch_ms = now_ms;
                }
                if (s_last_touch_ms == 0)
                {
                    s_last_touch_ms = now_ms;    /* 首次：以启动时刻起算 */
                }
                if (idx != UI_SCENE_CLOCK &&
                    (now_ms - s_last_touch_ms) >= SCRSAVER_AFTER_MS)
                {
                    g_clock_return_scene = idx;
                    s_goto = UI_SCENE_CLOCK;
                    ESP_LOGI(TAG, "screensaver: idle 5min -> clock");
                }
            }

            if (s_goto >= 0)
            {
                ESP_LOGI(TAG, "scene %s -> %s", sc->name, g_scenes[s_goto].name);
                idx   = s_goto;
                s_goto = -1;
                frame  = 0;
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(sc->hold_ms));
            continue;
        }

        /* ---- 旧自绘路径（仅测试场景，当前关闭） ---- */
        int64_t last_touch = esp_timer_get_time();

        if (sc->frames != 0)
        {
            for (int f = 0; sc->frames < 0 || f < sc->frames; f++)
            {
                uint16_t *fb = ui_fb_begin();
                sc->draw(fb, f);
                if (sc->show_badge)
                {
                    ui_badge(fb, idx + 1, sc->name);
                }
                ui_fb_commit();

                if (wait_with_events(sc->hold_ms, &idx, &last_touch))
                {
                    break;
                }
            }
        }
        else
        {
            uint16_t *fb = ui_fb_begin();
            sc->draw(fb, 0);
            if (sc->show_badge)
            {
                ui_badge(fb, idx + 1, sc->name);
            }
            ui_fb_commit();

            wait_with_events(sc->hold_ms, &idx, &last_touch);
        }
    }
}
