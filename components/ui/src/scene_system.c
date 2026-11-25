/**
 * @file    scene_system.c
 * @brief   系统信息页：芯片/内存/存储 + WiFi 状态与手动开关 + 最近日志
 *
 * 三个用途：
 *   ① 看 S31 的真实配置（核心数、频率、flash/PSRAM、堆余量、任务数、运行时长）；
 *   ② **手动断开/重连 WiFi** —— 用来验证右上角信号图标（断开后应变全灰）；
 *   ③ 显示最近 10 条系统日志（拦截 esp_log 得到的环形缓冲，不含 syslog 噪音）。
 *
 * 刷新节奏：每 1 秒更新一次（scene 的 hold_ms = 100，即每 10 帧）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_player.h"
#include "audio_rec.h"
#include "backlight.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_fonts.h"
#include "ui_anim.h"
#include "ui_scenes.h"
#include "ui_theme.h"
#include "wifi_config.h"
#include "wifi_prov.h"
#include "voice.h"
#include "wifi_sta.h"

/* 11-20 主题化：指向可切换色板（本页还承载"深色模式"开关） */
#define COL_BG     (ui_theme()->bg_top)
#define COL_TEXT   (ui_theme()->title)
#define COL_DIM    (ui_theme()->sub)
#define COL_BLUE   0x4C9AFF
#define COL_ORANGE 0xF2A03D
#define COL_GREEN  0x35D07F
#define COL_RED    0xE0554B
#define COL_PANEL  (ui_theme()->card)    /* 白卡片 */

/* ---- 手机风设置页：每个列表行一个值 label（2026-11-26 重做） ---- */
static lv_obj_t *s_v_wifi;       /* 网络：连接状态 */
static lv_obj_t *s_v_ip;         /* 网络：IP */
static lv_obj_t *s_v_cfg;        /* 网络：配网行右侧提示 */
static lv_obj_t *s_v_chip;       /* 设备：芯片 */
static lv_obj_t *s_v_mhz;        /* 设备：主频 */
static lv_obj_t *s_v_ram;        /* 设备：SRAM */
static lv_obj_t *s_v_psram;      /* 设备：PSRAM */
static lv_obj_t *s_v_flash;      /* 设备：闪存 */
static lv_obj_t *s_v_up;         /* 设备：运行时间 */
static lv_obj_t *s_v_core;       /* 设备：双核占用 */
static lv_obj_t *s_lbl_log;      /* 日志 */
static lv_obj_t *s_lbl_bl;       /* 亮度百分比 */
static lv_obj_t *s_bar_mic;      /* 麦克风电平 */
static lv_obj_t *s_lbl_mic;      /* 麦克风数值 */

/* ★ 语音识别结果桥：voice 回调在 voice_task 上下文，绝不能直接碰 LVGL
 *   （lv_async_call 也死锁过）——只写标志，由本场景 draw 里轮询消费。 */
static lv_obj_t *s_lbl_mic;     /* 麦克风电平数值 */

/* ---------------- 回调 ---------------- */

/**
 * ★ 文本没变就不 set（2026-11-25）。
 *
 * LVGL 的 `lv_label_set_text` **不比较内容**，同一串也标脏。
 * 本页每 500ms 全量刷 18 个 label，再叠加每 100ms 的麦克风电平 ——
 * **实测撞到 `max=16 ovf=2`**：脏区达到 `MAX_DIRTY` 上限并溢出，
 * 溢出后 flush_cb 退化成"整屏 768KB sync"，这正是最该避免的情况。
 * 加了比对后 500ms 那批通常只剩 1~3 块真变的，整体远低于上限。
 */
static void set_text_if_changed(lv_obj_t *lbl, const char *s)
{
    if (lbl == NULL || s == NULL)
    {
        return;
    }
    const char *old = lv_label_get_text(lbl);
    if (old == NULL || strcmp(old, s) != 0)
    {
        lv_label_set_text(lbl, s);
    }
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);
}

static void wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_sta_connected())
    {
        wifi_sta_disconnect_user();
    }
    else
    {
        wifi_sta_reconnect_user();
    }
}

/* ---------------- 刷新 ---------------- */

/**
 * ★ 每核 CPU 占用（2026-11-25）。
 *
 * `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` + `..._USING_ESP_TIMER` **本来就开着**，
 * 所以不需要改 sdkconfig（也就避开了全量重编 + `x509_crt.c` 僵死那个坑）。
 *
 * 算法：每核的占用 = 100% − 该核 IDLE 任务的占比。
 * 用两次采样求差（`ulRunTimeCounter` 是累计值），分母用 `esp_timer` 的墙钟差 ——
 * **不能**用 `uxTaskGetSystemState` 返回的总时间，那是"所有任务时间之和"（≈2×墙钟）。
 * 任务名兼容 "IDLE0"/"IDLE1" 与 "IDLE<0>"/"IDLE<1>"（SMP 下带核号）。
 */
static void core_usage(int *u0, int *u1)
{
#ifdef ESP_PLATFORM
    static int64_t  s_last_us;
    static uint32_t s_last_idle[2];

    TaskStatus_t arr[40];
    uint32_t     total_rt = 0;
    UBaseType_t  n = uxTaskGetSystemState(arr, 40, &total_rt);

    uint32_t idle[2] = { 0, 0 };
    for (UBaseType_t i = 0; i < n; i++)
    {
        const char *nm = arr[i].pcTaskName;
        if (nm == NULL || strncmp(nm, "IDLE", 4) != 0)
        {
            continue;
        }
        const char *p = nm + 4;
        while (*p != '\0' && (*p < '0' || *p > '9'))
        {
            p++;                        /* 跳过 '<' 之类的分隔符 */
        }
        if (*p == '0')
        {
            idle[0] = arr[i].ulRunTimeCounter;
        }
        else if (*p == '1')
        {
            idle[1] = arr[i].ulRunTimeCounter;
        }
    }

    int64_t now = esp_timer_get_time();
    int64_t dt  = now - s_last_us;
    if (s_last_us != 0 && dt > 0)
    {
        for (int c = 0; c < 2; c++)
        {
            int64_t idle_d = (int64_t)idle[c] - (int64_t)s_last_idle[c];
            int     u      = 100 - (int)(idle_d * 100 / dt);
            if (u < 0)   { u = 0; }
            if (u > 100) { u = 100; }
            if (c == 0)  { *u0 = u; } else { *u1 = u; }
        }
    }
    s_last_us      = now;
    s_last_idle[0] = idle[0];
    s_last_idle[1] = idle[1];
#else
    /* PC 模拟器的 FreeRTOS stub 没有 TaskStatus_t / uxTaskGetSystemState / ulRunTimeCounter，
     * 这里给 0 即可 —— 模拟器只用来验 UI 布局，不看真实核占用。 */
    *u0 = 0;
    *u1 = 0;
#endif
}

static void refresh_all(void)
{
    char b[512];

    /* ---- 左：芯片 / 内存 / 存储 ---- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t cpu_mhz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t ps_total  = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t ps_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    int64_t  up_s = esp_timer_get_time() / 1000000;
    int      up_h = (int)(up_s / 3600);
    int      up_m = (int)((up_s % 3600) / 60);
    int      up_sec = (int)(up_s % 60);
    uint32_t tasks = (uint32_t)uxTaskGetNumberOfTasks();

    int c0 = 0, c1 = 0;
    core_usage(&c0, &c1);

    /* ★ 临时：每 5 秒把双核占用打一条日志，方便从串口直接读（本页 500ms 刷一次，
     *   10 次 = 5 秒）。看够了就删。 */
    static int s_core_log_tick = 0;
    if (++s_core_log_tick % 10 == 0)
    {
        ESP_LOGI("system", "core: c0 %d%%  c1 %d%%", c0, c1);
    }

    /* ---- 网络数据（重设计时这段声明跟着被替换掉了，补回来） ---- */
    char ip[20] = { 0 };
    wifi_sta_get_ip(ip, sizeof(ip));
    bool conn = wifi_sta_connected();

    /* ---- 设备组（每行一个值，都是短文本） ---- */
    snprintf(b, sizeof(b), "ESP32-S31");
    set_text_if_changed(s_v_chip, b);

    snprintf(b, sizeof(b), "%u MHz \u00b7 %d \u6838", (unsigned)cpu_mhz, chip.cores);
    set_text_if_changed(s_v_mhz, b);

    snprintf(b, sizeof(b), "%u/%u KB", (unsigned)(ram_free / 1024), (unsigned)(ram_total / 1024));
    set_text_if_changed(s_v_ram, b);

    snprintf(b, sizeof(b), "%u.%u/%u.%u MB",
             (unsigned)(ps_free / 1048576), (unsigned)((ps_free % 1048576) * 10 / 1048576),
             (unsigned)(ps_total / 1048576), (unsigned)((ps_total % 1048576) * 10 / 1048576));
    set_text_if_changed(s_v_psram, b);

    snprintf(b, sizeof(b), "%02d:%02d:%02d  %u \u4efb\u52a1", up_h, up_m, up_sec, (unsigned)tasks);
    set_text_if_changed(s_v_up, b);

    snprintf(b, sizeof(b), "c0 %d%%   c1 %d%%", c0, c1);
    set_text_if_changed(s_v_core, b);

    /* ---- 网络组 ---- */
    set_text_if_changed(s_v_wifi, conn ? "\u5df2\u8fde\u63a5" : "\u672a\u8fde\u63a5");
    lv_obj_set_style_text_color(s_v_wifi, lv_color_hex(conn ? COL_GREEN : COL_RED), 0);

    set_text_if_changed(s_v_ip, ip[0] ? ip : "\u2014");
    set_text_if_changed(s_v_cfg, conn ? "\u66f4\u6362\u7f51\u7edc >" : "\u53bb\u914d\u7f51 >");

    /* ---- 底部：最近日志（8 行） ---- */
    static char lines[7][96];
    int n = ui_logbuf_copy(lines, 7);
    b[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n && used < sizeof(b) - 100; i++)
    {
        int w = snprintf(b + used, sizeof(b) - used, "%s\n", lines[i]);
        if (w <= 0)
        {
            break;
        }
        used += (size_t)w;
    }
    if (n == 0)
    {
        snprintf(b, sizeof(b), "(no log yet)");
    }
    set_text_if_changed(s_lbl_log, b);
}

/* ---------------- 建界面 ---------------- */

static lv_obj_t *panel(lv_obj_t *scr, int x, int y, int w, int h, uint32_t border)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_shadow_width(p, 14, 0);
    lv_obj_set_style_shadow_opa(p, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(p, lv_color_hex(ui_theme()->shadow), 0);
    lv_obj_set_style_shadow_offset_y(p, 3, 0);
    (void)border;
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/**
 * 配网按钮：开 SoftAP 并弹一张指引卡。
 *
 * ★ 为什么用弹出卡而不是切页面：配网时用户手上是手机，屏幕只需"告诉连哪个热点、
 *   开哪个网址"就够了，不需要在屏上输入任何东西（输 WiFi 密码必须用真键盘）。
 * ★ 卡片挂在当前 scr 上，切页时随 lv_obj_clean 一起消失，不会残留。
 */
/** 记住从哪进的时钟，触摸返回时回这个场景（全局变量，scene_clock.c 里用） */
int g_clock_return_scene = UI_SCENE_HOME;

static void bl_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    backlight_set(v);
    if (s_lbl_bl != NULL)
    {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", v);
        set_text_if_changed(s_lbl_bl, b);
    }
}

/* ★ 11-20 深色模式开关：切色板 → 刷状态栏 → 重建本页（frame=0 会 lv_obj_clean 重画） */
static void theme_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool dark = lv_obj_has_state(sw, LV_STATE_CHECKED);
    /* ★ 只换色板 + 请求重建；状态栏刷新放到重建那一帧（build_system 末尾），
     *   否则会出现"顶部新色 + 页面旧色"的一帧混色。 */
    ui_theme_set(dark);
    ui_goto_scene(UI_SCENE_SYSTEM);
}

/* ★ 板载 RGB 灯调色（HUE/VAL 滑块实时驱动 WS2812） */
/* ★★ 11-21 教训：滑条 VALUE_CHANGED 每帧几十次，若在事件回调里直接走 RMT
 *   发 WS2812 波形（led_strip_refresh），RMT 与 LCD 共用 DMA，
 *   高频抢占会崩溃（乱屏 + 重启）。现在事件里只记值，由场景周期限速发送。 */


static lv_obj_t *s_root;   /* ---------------- 建界面：手机设置页风格（2026-11-26 重做） ----------------
 * 结构：顶部返回 + 标题；下面左右两列分组卡片，每组「小标题 + 圆角卡片 + 若干列表行」。
 * 行样式统一为「彩色圆点 + 中文标题 + 右侧灰色值」，分隔线左缩进（iOS 风格）。
 * ★ 中文一律 ui_font_cn18（其他字体是 ASCII 字模，中文会变方块）。 */

static void group_caption(lv_obj_t *scr, int x, int y, const char *t)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
    lv_obj_set_pos(l, x + 4, y);
    lv_label_set_text(l, t);
}

static lv_obj_t *group_card(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void row_sep(lv_obj_t *card, int y, int x0, int w)
{
    lv_obj_t *s = lv_obj_create(card);
    lv_obj_set_pos(s, x0, y);
    lv_obj_set_size(s, w, 1);
    lv_obj_set_style_bg_color(s, lv_color_hex(ui_theme()->card_line), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    lv_obj_set_style_radius(s, 0, 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/** 行左侧的彩色小圆点 */
static void row_dot(lv_obj_t *card, int y, int row_h, uint32_t color)
{
    lv_obj_t *d = lv_obj_create(card);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_pos(d, 16, y + (row_h - 8) / 2);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/** 行标题 */
static void row_title(lv_obj_t *card, int y, int row_h, const char *t)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_font(l, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(l, 36, y + (row_h - 24) / 2);
    lv_label_set_text(l, t);
}

/** 标准行：圆点 + 标题 + 右侧值；返回值 label */
static lv_obj_t *setting_row(lv_obj_t *card, int y, int row_h, uint32_t dot,
                             const char *title, const char *value)
{
    row_dot(card, y, row_h, dot);
    row_title(card, y, row_h, title);

    lv_obj_t *v = lv_label_create(card);
    lv_obj_set_style_text_font(v, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(COL_DIM), 0);
    lv_obj_set_width(v, 200);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(v, 160, y + (row_h - 24) / 2);
    lv_label_set_text(v, value);
    return v;
}

/** 整行可点：透明命中层盖在该行上（不挡视觉，只接点击） */
static void row_clickable(lv_obj_t *card, int y, int row_h, int w, lv_event_cb_t cb)
{
    lv_obj_t *hit = lv_obj_create(card);
    lv_obj_set_pos(hit, 0, y);
    lv_obj_set_size(hit, w, row_h);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_set_style_pad_all(hit, 0, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);
}

/** 打开配网热点（SoftAP + 网页表单，用手机连） */
static void prov_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = wifi_prov_start();
    ESP_LOGI("system", "prov start: %s", esp_err_to_name(err));
}

static void build_system(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;

    /* ---- 顶栏：圆形返回 + 标题 ---- */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 12, 8);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(ui_theme()->card), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *ba = lv_label_create(back);
    lv_obj_set_style_text_font(ba, &ui_font_20, 0);
    lv_obj_set_style_text_color(ba, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(ba, "<");   /* ★ 不用 LV_SYMBOL_LEFT：ui_font_20 是 ASCII 字模，符号位渲染成方块 */
    lv_obj_center(ba);
    lv_obj_add_event_cb(back, home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &ui_font_cn18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(title, "设置");
    lv_obj_set_pos(title, 58, 14);

    /* ================= 左列 ================= */
    group_caption(scr, 16, 52, "网络");
    lv_obj_t *c_net = group_card(scr, 16, 76, 376, 132);
    s_v_wifi = setting_row(c_net, 0, 44, COL_BLUE, "WiFi", "未连接");
    row_sep(c_net, 44, 36, 324);
    s_v_ip = setting_row(c_net, 44, 44, COL_BLUE, "IP 地址", "—");
    row_sep(c_net, 88, 36, 324);
    s_v_cfg = setting_row(c_net, 88, 44, COL_BLUE, "重新配网", "打开热点 >");
    row_clickable(c_net, 0, 44, 376, wifi_toggle_cb);   /* 第一行：断开 / 重连 */
    row_clickable(c_net, 88, 44, 376, prov_cb);         /* 第三行：开热点配网 */

    group_caption(scr, 16, 218, "显示");
    lv_obj_t *c_disp = group_card(scr, 16, 242, 376, 100);

    /* 亮度行 */
    row_dot(c_disp, 0, 50, COL_ORANGE);
    row_title(c_disp, 0, 50, "亮度");
    {
        lv_obj_t *sl = lv_slider_create(c_disp);
        lv_obj_set_size(sl, 140, 10);
        lv_obj_set_pos(sl, 110, 20);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, backlight_get(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(sl, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
        lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_add_event_cb(sl, bl_cb, LV_EVENT_VALUE_CHANGED, NULL);

        s_lbl_bl = lv_label_create(c_disp);
        lv_obj_set_style_text_font(s_lbl_bl, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_bl, lv_color_hex(COL_DIM), 0);
        lv_obj_set_width(s_lbl_bl, 90);
        lv_obj_set_style_text_align(s_lbl_bl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_lbl_bl, 270, 14);
        char b[8];
        snprintf(b, sizeof(b), "%d%%", backlight_get());
        set_text_if_changed(s_lbl_bl, b);
    }
    row_sep(c_disp, 50, 36, 324);

    /* 深色模式行 */
    row_dot(c_disp, 50, 50, COL_ORANGE);
    row_title(c_disp, 50, 50, "深色模式");
    {
        lv_obj_t *sw = lv_switch_create(c_disp);
        lv_obj_set_size(sw, 50, 26);
        lv_obj_set_pos(sw, 306, 12 + 50);
        if (ui_theme_is_dark())
        {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(ui_theme()->accent), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    /* ================= 右列 ================= */
    group_caption(scr, 408, 52, "设备");
    lv_obj_t *c_dev = group_card(scr, 408, 76, 376, 266);
    s_v_chip  = setting_row(c_dev, 0,   38, COL_GREEN, "芯片",  "ESP32-S31");
    s_v_mhz   = setting_row(c_dev, 38,  38, COL_GREEN, "主频",  "—");
    s_v_ram   = setting_row(c_dev, 76,  38, COL_GREEN, "内存",  "—");
    s_v_psram = setting_row(c_dev, 114, 38, COL_GREEN, "PSRAM", "—");
    s_v_flash = setting_row(c_dev, 152, 38, COL_GREEN, "闪存",  "16 MB");
    s_v_up    = setting_row(c_dev, 190, 38, COL_GREEN, "运行",  "—");
    s_v_core  = setting_row(c_dev, 228, 38, COL_GREEN, "双核",  "—");

    group_caption(scr, 408, 352, "日志");
    lv_obj_t *c_log = group_card(scr, 408, 376, 376, 90);

    /* 麦克风电平（日志卡顶部一行） */
    row_title(c_log, 0, 30, "麦克风");
    {
        s_bar_mic = lv_bar_create(c_log);
        lv_obj_set_size(s_bar_mic, 190, 10);
        lv_obj_set_pos(s_bar_mic, 110, 10);
        lv_bar_set_range(s_bar_mic, 0, 100);
        lv_bar_set_value(s_bar_mic, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(ui_theme()->card_line), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_bar_mic, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar_mic, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar_mic, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bar_mic, 5, LV_PART_INDICATOR);

        s_lbl_mic = lv_label_create(c_log);
        lv_obj_set_style_text_font(s_lbl_mic, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_mic, lv_color_hex(COL_DIM), 0);
        lv_obj_set_pos(s_lbl_mic, 310, 4);
        set_text_if_changed(s_lbl_mic, "0");
    }

    s_lbl_log = lv_label_create(c_log);
    lv_obj_set_style_text_font(s_lbl_log, &ui_font_14, 0);
    lv_obj_set_style_text_color(s_lbl_log, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_line_space(s_lbl_log, 0, 0);
    lv_obj_set_pos(s_lbl_log, 16, 34);
    lv_obj_set_width(s_lbl_log, 344);
    lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_WRAP);
    set_text_if_changed(s_lbl_log, "(no log yet)");

    refresh_all();
}

void scene_system(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        build_system();
        ui_statusbar_refresh();   /* 重建完再刷状态栏：整屏同帧换色，无混色过渡 */
    }
    else if (frame % 25 == 0)     /* 约 500ms 刷新一次（原 1s，加快一倍） */
    {
        refresh_all();
    }

    /* 麦克风电平单独快刷（100ms）：说话时条要跟得上，慢了像卡住 */
    if (frame > 0 && (frame % 5) == 0 && s_bar_mic != NULL)
    {
        int lv = audio_rec_level();
        lv_bar_set_value(s_bar_mic, lv, LV_ANIM_OFF);
        if (s_lbl_mic != NULL)
        {
            char b[8];
            snprintf(b, sizeof(b), "%d", lv);
            set_text_if_changed(s_lbl_mic, b);
        }

    }

    lvgl_port_task();
}
