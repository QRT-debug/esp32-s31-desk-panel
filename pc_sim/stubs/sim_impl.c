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
    if (idx >= 0)   /* 不设上界：新场景只在 switch 里兜底，避免每次扩枚举都要改这里 */
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

/* ---- 天气（PC 模拟：固定假数据，便于调 UI） ---- */
#include "weather.h"

static wx_data_t s_wx_fake;
static bool      s_wx_inited;

void weather_start(void)
{
    if (s_wx_inited) { return; }
    s_wx_inited = true;
    s_wx_fake.state     = WX_STATE_OK;
    s_wx_fake.temp_c    = 8;
    s_wx_fake.feels_c   = 25;
    s_wx_fake.humidity  = 68;
    s_wx_fake.wind_kmph = 14;
    snprintf(s_wx_fake.wind_dir, sizeof(s_wx_fake.wind_dir), "%s", "SE");
    snprintf(s_wx_fake.desc, sizeof(s_wx_fake.desc), "%s", "晴");   /* 晴 */
    snprintf(s_wx_fake.city, sizeof(s_wx_fake.city), "%s", "北京");   /* 北京 */
    snprintf(s_wx_fake.region, sizeof(s_wx_fake.region), "%s", "China");
    {   /* 今日逐 3 小时假曲线（00..21 时） */
        static const int fake_h[8] = { -2, -1, 1, 4, 8, 6, 3, 1 };
        for (int i = 0; i < 8; i++) { s_wx_fake.hourly_t[i] = fake_h[i]; }
    }
    s_wx_fake.today_max = 26;
    s_wx_fake.today_min = 18;
    s_wx_fake.tmrw_max  = 24;
    s_wx_fake.tmrw_min  = 16;
    snprintf(s_wx_fake.updated, sizeof(s_wx_fake.updated), "%s", "17:20");
    s_wx_fake.aqi      = 122;   /* PC 模拟：北京实测 AQI */
    s_wx_fake.pm25_x10 = 1071;
    s_wx_fake.http_code = 200;
}

void weather_refresh_now(void) { }
void weather_get(wx_data_t *out) { *out = s_wx_fake; }

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

/* ---- HF 热榜（PC 模拟：固定假榜单） ---- */
#include "hf_trend.h"

static hf_data_t s_hf_fake;
static bool      s_hf_inited;

void hf_start(void)
{
    if (s_hf_inited) { return; }
    s_hf_inited = true;
    static const char *ids[10] = {
        "thinkingmachines/Inkling-2-Mini",
        "Sentinel7/h3",
        "higurashi-ai/Haru-4B",
        "harness-hub/Qwen3.8-27B-pi-adapted",
        "unsloth/Inkling-2-Mini-GGUF",
        "Qwen/Qwen3.8-27B",
        "Lightricks/LTX-2.5",
        "tencent/Hy4-flash",
        "sensenova/SenseNova-Loop-9B",
        "inworld/voice-isolator",
    };
    static const char *tasks[10] = {
        "image-text-to-text", "--", "audio-to-audio", "image-text-to-text",
        "image-text-to-text", "image-text-to-text", "image-to-video",
        "image-text-to-text", "image-text-to-text", "audio-to-audio",
    };
    static const int likes[10]  = { 908, 443, 5704, 872, 298, 20823, 12925, 555, 453, 510 };
    static const int dloads[10] = { 136, 0, 7323, 1874, 0, 6454419, 2964523, 3949, 1631, 0 };
    static const char *created[10] = {
        "11-20 14:32", "11-21 09:10", "11-19 22:44", "11-18 16:01",
        "11-20 08:22", "11-15 12:30", "11-17 19:05", "11-21 06:40",
        "11-20 21:18", "11-14 10:14",
    };
    s_hf_fake.state = HF_STATE_OK;
    s_hf_fake.count = 10;
    for (int i = 0; i < 10; i++)
    {
        snprintf(s_hf_fake.item[i].id, sizeof(s_hf_fake.item[i].id), "%s", ids[i]);
        snprintf(s_hf_fake.item[i].task, sizeof(s_hf_fake.item[i].task), "%s", tasks[i]);
        s_hf_fake.item[i].likes = likes[i];
        s_hf_fake.item[i].downloads = dloads[i];
        snprintf(s_hf_fake.item[i].created, sizeof(s_hf_fake.item[i].created), "%s", created[i]);
    }
    snprintf(s_hf_fake.updated, sizeof(s_hf_fake.updated), "%s", "18:45");
    s_hf_fake.http_code = 200;
}

void hf_refresh_now(void) { }
void hf_get(hf_data_t *out) { *out = s_hf_fake; }

/* ---- 国内 AI 产品榜（PC 模拟：2026-06 真实榜单快照，已映射英文名） ---- */
#include "ai_rank.h"

void ai_rank_get(ai_rank_data_t *out)
{
    static const char *web[10] = {
        "DeepSeek", "Nano AI Search", "Doubao", "Nano AI", "Baidu AI Search",
        "Qwen (Alibaba)", "Kimi", "Qwen.ai", "Baidu Wenku", "Tencent Yuanbao",
    };
    static const char *web_cn[10] = {
        "DeepSeek", "纳米AI搜索", "豆包｜抖音", "纳米AI", "AI 搜索｜百度",
        "千问 qianwen.com｜阿里", "Kimi｜月之暗面", "Qwen.ai｜阿里", "百度文库", "腾讯元宝",
    };
    static const char *app[10] = {
        "Doubao", "Qwen", "Quark", "DeepSeek", "Tencent Yuanbao",
        "Baidu Netdisk", "Jimeng AI", "Kimi", "Kling AI", "Nano AI",
    };
    static const char *app_cn[10] = {
        "豆包 - 字节跳动旗下 AI 助手", "千问 - 阿里最强模型官方AI助手", "夸克-阿里AI旗舰应用",
        "DeepSeek - AI 智能助手", "腾讯元宝-「DeepSeek+」智能新体验", "百度网盘",
        "即梦AI - 抖音旗下AI图片和视频工具", "Kimi", "可灵AI - AI图片&视频创作工具",
        "纳米AI - 首创多智能体蜂群",
    };
    memset(out, 0, sizeof(*out));
    out->state = AI_RANK_OK;
    snprintf(out->month, sizeof(out->month), "%s", "Jun 2026");
    snprintf(out->updated, sizeof(out->updated), "%s", "20:25");
    out->http_code = 200;
    out->gen = 1;
    out->web_count = 10;
    out->app_count = 10;
    for (int i = 0; i < 10; i++)
    {
        out->web[i].rank = i + 1;
        snprintf(out->web[i].name, sizeof(out->web[i].name), "%s", web[i]);
        snprintf(out->web[i].cn, sizeof(out->web[i].cn), "%s", web_cn[i]);
        out->app[i].rank = i + 1;
        snprintf(out->app[i].name, sizeof(out->app[i].name), "%s", app[i]);
        snprintf(out->app[i].cn, sizeof(out->app[i].cn), "%s", app_cn[i]);
    }
}

void ai_rank_start(void) { }
void ai_rank_refresh_now(void) { }

/* ---- B 站热搜（PC 模拟：2026-11-21 真实榜单快照） ---- */
#include "bili_hot.h"

void bili_start(void) { }
void bili_refresh_now(void) { }

void bili_get(bili_data_t *out)
{
    static const char *words[10] = {
        "多国领导人就一个中国原则明确表态",
        "终末地2.0版本卫星PV",
        "AI宣称彻底击破NS方程问题",
        "BLG庆功饭堂夜",
        "AI解决黎曼猜想数学博后反应",
        "探秘复旦大学食堂吃什么",
        "新赛季版本前瞻",
        "年度最佳游戏提名",
        "考研数学难度",
        "冬日治愈系vlog",
    };
    static const long heats[10] = { 39354137, 36251047, 3248060, 3109804, 2558510,
                                    1575113, 1203888, 998211, 754332, 512000 };
    memset(out, 0, sizeof(*out));
    out->state = BILI_OK;
    out->count = 10;
    for (int i = 0; i < 10; i++)
    {
        out->item[i].rank = i + 1;
        snprintf(out->item[i].word, sizeof(out->item[i].word), "%s", words[i]);
        out->item[i].heat = heats[i];
    }
    snprintf(out->updated, sizeof(out->updated), "%s", "21:15");
    out->gen = 1;
    out->http_code = 200;
}

/* ---------------- 时钟场景/配网 相关的最小桩（UI 代码演进后补上） ---------------- */
int ui_current_scene(void)
{
    return 0;   /* 模拟器无真实场景状态机 */
}

esp_err_t wifi_prov_start(void)
{
    return ESP_OK;
}

const char *wifi_prov_ssid(void)
{
    return "ESP32-S31-SETUP";
}
