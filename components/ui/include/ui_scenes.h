/**
 * @file    ui_scenes.h
 * @brief   测试画面注册表
 *
 * 一个场景有两种运行方式：
 *   静态（frames = 0）：画一次，停 hold_ms
 *   动画（frames > 0）：画 frames 次，每次间隔 hold_ms（逐帧，不阻塞调度）
 *
 * ★ 动画绝不能在一个 draw() 里循环刷完所有帧 —— 那样触摸事件和场景切换
 *   都会被堵在里面，表现为"到了这一页点屏幕没反应"。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const char *name;
    void (*draw)(uint16_t *fb, int frame);   /* frame: 从 0 开始的帧序号 */
    int  hold_ms;      /* 静态：停留时长；动画：每帧间隔 */
    int  frames;       /* 0 = 静态画面；>0 = 动画帧数；<0 = 一直运行（靠触摸退出） */
    bool show_badge;   /* 是否由 manager 统一画左上角编号 */
    bool self_managed; /* true: 场景自己管帧缓冲并提交（如 LVGL），manager 不碰 */
} ui_scene_t;

extern const ui_scene_t g_scenes[];
extern const int        g_scene_count;

/* 场景索引（ui_goto_scene 用）
 * ★ 11-20：天气页成为 HOME（开机主页面），原四卡片首页降级为 APPS 页。 */
enum
{
    UI_SCENE_HOME    = 0,   /* 天气主页面 */
    UI_SCENE_MONITOR = 1,
    UI_SCENE_AUDIO   = 2,
    UI_SCENE_SYSTEM  = 3,
    UI_SCENE_APPS    = 4,   /* 原四卡片应用选择页 */
    UI_SCENE_HF      = 5,   /* HF 模型热榜监视器（终端风） */
    UI_SCENE_AIRANK  = 6,   /* 国内 AI 产品榜（AICPB） */
    UI_SCENE_BILI    = 7,   /* B 站热搜榜 */
    UI_SCENE_CLOCK   = 8,   /* 屏保：SD 卡壁纸 + 大字时钟 */
};

/* 每个 scene 只负责画内容，不负责切换（badge 由 manager 画，除非 show_badge = false） */
void scene_solid(uint16_t *fb, int frame);
void scene_grayscale(uint16_t *fb, int frame);
void scene_rgb_ramp(uint16_t *fb, int frame);
void scene_checker(uint16_t *fb, int frame);
void scene_stripe(uint16_t *fb, int frame);
void scene_alignment(uint16_t *fb, int frame);
void scene_motion(uint16_t *fb, int frame);
void scene_info(uint16_t *fb, int frame);
void scene_ui(uint16_t *fb, int frame);
void scene_lvgl(uint16_t *fb, int frame);
void scene_monitor(uint16_t *fb, int frame);
void scene_home(uint16_t *fb, int frame);      /* 应用选择页（原首页，现 UI_SCENE_APPS） */
void scene_weather(uint16_t *fb, int frame);   /* 天气主页面（现 UI_SCENE_HOME） */
void scene_hf(uint16_t *fb, int frame);        /* HF 模型热榜（终端风） */
void scene_ai_rank(uint16_t *fb, int frame);   /* 国内 AI 产品榜（AICPB） */
void scene_bili(uint16_t *fb, int frame);      /* B 站热搜榜 */
void scene_audio(uint16_t *fb, int frame);     /* 音频播放器：音量滑块 + 测试音 */
void scene_system(uint16_t *fb, int frame);
void scene_clock(uint16_t *fb, int frame);   /* 屏保：SD 壁纸 + 96px 时间 */    /* 系统信息：芯片/内存/WiFi/日志 + 断连开关 */

/* 场景跳转（LVGL 按钮里调用；ui_manager 每帧检查） */
void ui_goto_scene(int idx);

/* 顶部状态栏（WiFi 扇形图标），建在 lv_layer_top，全界面共用；只调一次 */
void ui_statusbar_init(void);
void ui_statusbar_refresh(void);   /* 主题切换后重刷状态栏颜色（ui_theme.h） */

/* 最近日志环形缓冲（拦截 esp_log 输出，供系统信息页显示） */
void ui_logbuf_init(void);                 /* 在 ui_manager_run 开头调一次 */
int  ui_logbuf_copy(char dst[][96], int max_lines);   /* 返回行数，最新的在最后 */

/* 由 main 调用：开机初始化画面 → 主屏选择界面 */
void ui_manager_run(void);

/* 开机初始化画面（阻塞到 WiFi 连上且拿到第一帧数据，或超时） */
void ui_splash_run(void);
