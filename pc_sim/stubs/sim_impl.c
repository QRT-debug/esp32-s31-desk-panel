/**
 * @file    sim_impl.c
 * @brief   PC 模拟器：把板级模块（音频/网络/SD/移植层）换成假实现
 *
 * 目的：让 components/ui 下的界面代码**一行不改**地在 PC 上跑起来，
 *      调像素/布局不用烧板子。
 *
 * 假数据都是"会动"的（RSSI 缓慢波动、CPU 上下跳），这样能同时看出
 * 图标分级、数值刷新等动态行为。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio_player.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "pc_monitor.h"
#include "sd_card.h"
#include "ui_scenes.h"
#include "wifi_sta.h"

/* ---------------- ESP-IDF 基础 ---------------- */

static uint32_t s_start_ms;

const char *esp_err_to_name(esp_err_t code)
{
    switch (code)
    {
    case ESP_OK:                 return "ESP_OK";
    case ESP_FAIL:               return "ESP_FAIL";
    case ESP_ERR_NO_MEM:         return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:    return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:  return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:      return "ESP_ERR_NOT_FOUND";
    default:                     return "ESP_ERR_?";
    }
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)(xTaskGetTickCount()) * 1000;
}

void esp_chip_info(esp_chip_info_t *out)
{
    out->model    = 0;
    out->features = CHIP_FEATURE_WIFI_BGN;
    out->revision = 1;
    out->cores    = 2;
}

size_t heap_caps_get_total_size(uint32_t caps)          { (void)caps; return 320 * 1024; }
size_t heap_caps_get_free_size(uint32_t caps)           { (void)caps; return 210 * 1024; }
size_t heap_caps_get_minimum_free_size(uint32_t caps)   { (void)caps; return 160 * 1024; }

const char *esp_get_idf_version(void) { return "v6.1-sim"; }

void vTaskDelay(TickType_t ticks) { extern void sim_delay_ms(uint32_t); sim_delay_ms((uint32_t)ticks); }
UBaseType_t uxTaskGetNumberOfTasks(void) { return 12; }

/* ---------------- LVGL 移植层（PC 版） ---------------- */

bool lvgl_port_ready(void) { return true; }

void lvgl_port_task(void)
{
    lv_timer_handler();
}

esp_err_t lvgl_port_init(void) { return ESP_OK; }
void lvgl_port_start_benchmark(void) { }

/* ---------------- 场景切换 ---------------- */

static volatile int s_scene = UI_SCENE_HOME;
static volatile int s_rebuild = 0;   /* ★ 同场景重入请求（主题切换后重建本页） */

void ui_goto_scene(int idx)
{
    if (idx >= 0 && idx <= UI_SCENE_SYSTEM)
    {
        /* 板端 ui_manager 对同场景 goto 也会 frame=0 重建；
         * PC 侧等价语义 = 置重建标志，main_sim 每帧检查消费。 */
        s_rebuild = 1;
        s_scene = idx;
    }
}

int sim_scene_get(void) { return s_scene; }
void sim_scene_set(int idx) { ui_goto_scene(idx); }

/* 取走重建请求（1=本帧 frame 清零重建；读后自动清除） */
int sim_rebuild_take(void)
{
    int v = s_rebuild;
    s_rebuild = 0;
    return v;
}

/* ---------------- 音频（假播放） ---------------- */

static audio_player_state_t s_state = AUD_STATE_IDLE;
static int   s_vol = 40;
static int   s_diag;
static char  s_cur[64] = "Inferno.wav";
static uint32_t s_play_start_ms;

esp_err_t audio_player_init(void)   { return ESP_OK; }
esp_err_t audio_player_mount(void)  { return ESP_OK; }
audio_player_state_t audio_player_state(void) { return s_state; }
bool audio_player_is_playing(void)  { return s_state == AUD_STATE_PLAYING; }

esp_err_t audio_player_play_sd_by_name(const char *name)
{
    snprintf(s_cur, sizeof(s_cur), "%s", name);
    s_state = AUD_STATE_PLAYING;
    s_play_start_ms = xTaskGetTickCount();
    printf("[sim] play %s\n", name);
    return ESP_OK;
}

esp_err_t audio_player_play_sd(void) { return audio_player_play_sd_by_name("Inferno.wav"); }
esp_err_t audio_player_play(void)    { return audio_player_play_sd(); }
esp_err_t audio_player_play_file(const char *name) { return audio_player_play_sd_by_name(name); }

esp_err_t audio_player_stop(void)
{
    s_state = AUD_STATE_IDLE;
    return ESP_OK;
}

/* 模拟播放进度：按真实时间递增，到 3:53 循环（方便看进度条动） */
uint32_t audio_player_duration_ms(void) { return 233000u; }

/* 模拟麦克风电平：正弦慢摆（便于在模拟器里看条动） */
int audio_player_mic_level(void)
{
    double t = (double)xTaskGetTickCount() / 1000.0;
    int v = (int)(35 + 30 * sin(t * 2.2));
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}
uint32_t audio_player_position_ms(void)
{
    if (s_state != AUD_STATE_PLAYING)
    {
        return 0;
    }
    return (xTaskGetTickCount() - s_play_start_ms) % 233000u;
}

const char *audio_player_current_file(void) { return s_cur; }
esp_err_t audio_player_diag_set(int mode) { s_diag = mode; return ESP_OK; }
int       audio_player_diag_get(void)     { return s_diag; }
esp_err_t audio_player_set_volume(int vol) { s_vol = vol; return ESP_OK; }
int       audio_player_get_volume(void)    { return s_vol; }

/* ---------------- 电脑状态（假数据，缓慢变化） ---------------- */

static pc_stats_t s_stats;

esp_err_t pc_monitor_start(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.valid       = true;
    s_stats.cpu_cores   = 16;
    s_stats.cpu_mhz     = 4200;
    s_stats.mem_total_gb = 32.0f;
    s_stats.gpu_ok      = true;
    snprintf(s_stats.gpu_name, sizeof(s_stats.gpu_name), "RTX 4070");
    s_stats.gpu_mem_total_gb = 12.0f;
    s_stats.disk_count  = 2;
    snprintf(s_stats.disks[0].name, sizeof(s_stats.disks[0].name), "C:");
    s_stats.disks[0].total_gb = 1000.0f;
    snprintf(s_stats.disks[1].name, sizeof(s_stats.disks[1].name), "D:");
    s_stats.disks[1].total_gb = 2000.0f;
    s_stats.uptime_s    = 98765;
    return ESP_OK;
}

void pc_monitor_get(pc_stats_t *out)
{
    double t = (double)xTaskGetTickCount() / 1000.0;
    s_stats.cpu_pct      = 35.0f + 25.0f * (float)sin(t * 0.7);
    s_stats.mem_pct      = 62.0f;
    s_stats.mem_used_gb  = 19.8f;
    s_stats.gpu_pct      = 45.0f + 30.0f * (float)sin(t * 0.45 + 1.0);
    s_stats.gpu_mem_pct  = 55.0f;
    s_stats.gpu_mem_used_gb = 6.6f;
    s_stats.gpu_temp     = 58 + (int)(6.0 * sin(t * 0.3));
    s_stats.gpu_power    = 120.0f + 20.0f * (float)sin(t * 0.5);
    s_stats.disks[0].percent = 68.0f;
    s_stats.disks[0].free_gb = 320.0f;
    s_stats.disks[1].percent = 41.0f;
    s_stats.disks[1].free_gb = 1180.0f;
    s_stats.uptime_s    += 1;
    *out = s_stats;
}

const char *pc_monitor_url(void) { return "http://sim.local/api/stats.txt"; }

/* ---------------- WiFi（假 RSSI，缓慢波动） ---------------- */

static bool s_wifi_on = true;

esp_err_t wifi_sta_init(void) { return ESP_OK; }
bool wifi_sta_connected(void) { return s_wifi_on; }

bool wifi_sta_wait_connected(int timeout_ms) { (void)timeout_ms; return s_wifi_on; }

void wifi_sta_get_ip(char *buf, size_t len)
{
    snprintf(buf, len, "%s", s_wifi_on ? "192.168.1.3" : "0.0.0.0");
}

int wifi_sta_rssi(void)
{
    if (!s_wifi_on)
    {
        return 0;
    }
    /* 在 -45 ~ -88 之间缓慢摆动，能看到 3/2/1 档切换 */
    double t = (double)xTaskGetTickCount() / 1000.0;
    return (int)(-66.0 + 22.0 * sin(t * 0.08));
}

esp_err_t wifi_sta_disconnect_user(void) { s_wifi_on = false; return ESP_OK; }
esp_err_t wifi_sta_reconnect_user(void)  { s_wifi_on = true;  return ESP_OK; }

/* ---------------- SD 卡（假曲目） ---------------- */

static const char *k_fake_wav[] = { "Inferno.wav", "SawanoHiroyuki[nZk] - REMEMBER.wav", "TheDOGS.wav" };

static bool s_sd_ready = true;

esp_err_t sd_card_mount(void) { s_sd_ready = true; return ESP_OK; }
bool sd_card_ready(void) { return s_sd_ready; }

/* 板上会在这里做真实探测；模拟器直接返回当前状态 */
bool sd_card_poll(void) { return s_sd_ready; }

/* ---- WS2812 彩灯（PC 无硬件，色块预览在 LVGL 侧仍工作） ---- */
void bsp_led_init(void) { }
void bsp_led_set_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    (void)hue; (void)sat; (void)val;
}
void bsp_led_off(void) { }

/* 模拟器专用：按键 S 切换"插卡/拔卡"，用来验证状态栏图标 */
void sim_sd_toggle(void)
{
    s_sd_ready = !s_sd_ready;
    printf("[sim] SD card %s\n", s_sd_ready ? "inserted" : "removed");
}
uint64_t sd_card_capacity(void) { return 32ull * 1024 * 1024 * 1024; }

int sd_card_scan_wav(char names[][64], int max)
{
    int n = (int)(sizeof(k_fake_wav) / sizeof(k_fake_wav[0]));
    if (n > max)
    {
        n = max;
    }
    for (int i = 0; i < n; i++)
    {
        snprintf(names[i], 64, "%s", k_fake_wav[i]);
    }
    return n;
}

bool sd_card_find_first_wav(char *name, size_t len)
{
    snprintf(name, len, "%s", k_fake_wav[0]);
    return true;
}

/* ---------------- 背光（PC 上无操作） ---------------- */
static int s_bl = 80;
esp_err_t backlight_init(void) { return ESP_OK; }
void backlight_set(int pct) { s_bl = (pct < 0) ? 0 : (pct > 100 ? 100 : pct); }
int  backlight_get(void) { return s_bl; }
void audio_player_mic_debug_log(void) {}
