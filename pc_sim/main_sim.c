/**
 * @file    main_sim.c
 * @brief   PC 端 LVGL 模拟器：SDL 窗口 800x480，鼠标当触摸，数字键切页面
 *
 * 用法：
 *   cmake -S . -B build -G Ninja && cmake --build build
 *   ./build/pc_sim.exe
 *
 * 按键：
 *   1 = HOME   2 = PC MONITOR   3 = AUDIO   4 = SYSTEM   Esc = 退出
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_player.h"
#include "lv_demos.h"
#include "lvgl.h"
#include "pc_monitor.h"
#include "sd_card.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "wifi_sta.h"

#define SCR_W 800
#define SCR_H 480

static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;
static int           s_mx, s_my;
static bool          s_down;

/* 由 stubs/sim_impl.c 提供 */
int  sim_scene_get(void);
void sim_scene_set(int idx);
void sim_sd_toggle(void);
int  sim_rebuild_take(void);   /* 同场景重入请求（主题切换） */
void weather_start(void);      /* 天气（PC 为固定假数据） */
void hf_start(void);           /* HF 热榜（PC 为固定假榜单） */

/* ---- 给 stub 层用的时间/延时（它们只依赖这两个） ---- */
uint32_t xTaskGetTickCount(void) { return SDL_GetTicks(); }
void     sim_delay_ms(uint32_t ms) { SDL_Delay(ms); }

static uint32_t tick_cb(void) { return SDL_GetTicks(); }

/* LVGL 渲染完一块 → 更新 SDL 纹理对应区域 → 呈现 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    SDL_Rect r = { area->x1, area->y1, w, h };
    SDL_UpdateTexture(s_tex, &r, px_map, w * 2);

    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);

    lv_display_flush_ready(disp);
}

/* 把当前渲染器内容存 BMP（SDL 自带，零依赖；供 --shot 自动截图） */
static void save_shot(const char *path)
{
    SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormat(0, SCR_W, SCR_H, 32,
                                                     SDL_PIXELFORMAT_ABGR8888);
    if (ss == NULL)
    {
        printf("shot: surface failed\n");
        return;
    }
    if (SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                             ss->pixels, ss->pitch) != 0)
    {
        printf("shot: read pixels failed: %s\n", SDL_GetError());
        SDL_FreeSurface(ss);
        return;
    }
    /* 转 RGB24 再存；部分 GL 驱动的 ReadPixels 输出 BGR，手动交换 R/B 保证截图颜色正确 */
    SDL_Surface *cv = SDL_ConvertSurfaceFormat(ss, SDL_PIXELFORMAT_RGB24, 0);
    SDL_FreeSurface(ss);
    if (cv != NULL)
    {
        uint8_t *px = (uint8_t *)cv->pixels;
        int     np = cv->w * cv->h;
        for (int i = 0; i < np; i++)
        {
            uint8_t t = px[i * 3];
            px[i * 3]     = px[i * 3 + 2];
            px[i * 3 + 2] = t;
        }
    }
    if (cv == NULL || SDL_SaveBMP(cv, path) != 0)
    {
        printf("shot: save failed: %s\n", SDL_GetError());
    }
    else
    {
        printf("shot saved: %s\n", path);
    }
    if (cv != NULL)
    {
        SDL_FreeSurface(cv);
    }
}

static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_mx;
    data->point.y = s_my;
    data->state   = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("ESP32-S31 UI Simulator  [1-4 切页面, Esc 退出]",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCR_W, SCR_H, 0);
    s_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SCR_W, SCR_H);

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    /* 部分渲染模式：60 行一块，双缓冲，内存占用小、刷新也够快 */
    static uint8_t buf1[SCR_W * 60 * 2];
    static uint8_t buf2[SCR_W * 60 * 2];
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mouse_read_cb);

    /* 各模块的假实现 */
    audio_player_init();
    pc_monitor_start();
    wifi_sta_init();
    sd_card_mount();

    /* ★ 板子上这两步在 ui_manager_run() 里；模拟器不编 ui_manager.c，
     *   必须自己调，否则状态栏（时间/WiFi）和日志缓冲都不会出现。 */
    ui_logbuf_init();
    ui_statusbar_init();
    ui_statusbar_refresh();   /* 应用当前色板（--dark 时需要立即刷） */

    bool demo_music = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--demo-music") == 0)
        {
            demo_music = true;
        }
    }

    int  scene = UI_SCENE_HOME;
    const char *shot_path = NULL;
    int  shot_after = 0;

    weather_start();   /* PC 模拟：装载固定假天气数据（sim_impl.c） */
    hf_start();        /* PC 模拟：装载固定假 HF 榜单 */

    /* 可选：--scene N 直接打开指定页面（0=WEATHER 1=MONITOR 2=AUDIO 3=SYSTEM 4=APPS） */
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
        {
            sim_scene_set(atoi(argv[i + 1]));
        }
        if (strcmp(argv[i], "--dark") == 0)
        {
            ui_theme_set(true);   /* 深色主题启动（验证深色配色用） */
        }
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
        {
            shot_path  = argv[i + 1];
            shot_after = 90;   /* 渲染 90 帧（约 1.5s）后截图 */
        }
        if (strcmp(argv[i], "--nosd") == 0)
        {
            sim_sd_toggle();   /* 启动即"无卡"状态，用于截图对比 */
        }
    }
    ui_statusbar_refresh();   /* --dark 在状态栏 init 之后才解析，这里按最终色板再刷一次 */

    int  last_scene = -1;
    int  frame = 0;
    bool running = true;

    if (demo_music)
    {
        printf("running LVGL lv_demo_music (official reference UI)\n");
        lv_demo_music();
        while (running)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_QUIT) running = false;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (e.type == SDL_MOUSEBUTTONDOWN) { s_down = true; s_mx = e.button.x; s_my = e.button.y; }
                if (e.type == SDL_MOUSEBUTTONUP)   { s_down = false; }
                if (e.type == SDL_MOUSEMOTION)     { s_mx = e.motion.x; s_my = e.motion.y; }
            }
            lv_timer_handler();
            SDL_Delay(5);
        }
        return 0;
    }

    printf("simulator started: HOME (press 1-4 to switch, Esc to quit)\n");

    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_MOUSEMOTION:
                s_mx = e.motion.x;
                s_my = e.motion.y;
                break;
            case SDL_MOUSEBUTTONDOWN:
                s_down = true;
                s_mx = e.button.x;
                s_my = e.button.y;
                break;
            case SDL_MOUSEBUTTONUP:
                s_down = false;
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym)
                {
                case SDLK_1: ui_goto_scene(UI_SCENE_HOME);    break;
                case SDLK_2: ui_goto_scene(UI_SCENE_MONITOR); break;
                case SDLK_3: ui_goto_scene(UI_SCENE_AUDIO);   break;
                case SDLK_4: ui_goto_scene(UI_SCENE_SYSTEM);  break;
                case SDLK_s: sim_sd_toggle();                 break;
                case SDLK_ESCAPE: running = false;            break;
                default: break;
                }
                break;
            default:
                break;
            }
        }

        scene = sim_scene_get();
        if (sim_rebuild_take())     /* ★ 同场景重入（ui_goto_scene 调用即重建） */
        {
            frame = 0;
        }
        if (scene != last_scene)
        {
            frame = 0;
            last_scene = scene;
        }

        switch (scene)
        {
        case UI_SCENE_HOME:    scene_weather(NULL, frame++); break;
        case UI_SCENE_MONITOR: scene_monitor(NULL, frame++); break;
        case UI_SCENE_AUDIO:   scene_audio(NULL, frame++);   break;
        case UI_SCENE_SYSTEM:  scene_system(NULL, frame++);  break;
        case UI_SCENE_APPS:    scene_home(NULL, frame++);    break;
        case UI_SCENE_HF:      scene_hf(NULL, frame++);      break;
        default: break;
        }

        if (shot_path != NULL && frame >= shot_after)
        {
            SDL_RenderPresent(s_ren);   /* 确保最后一帧已呈现 */
            save_shot(shot_path);
            running = false;
        }

        SDL_Delay(16);   /* ~60Hz */
    }

    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
