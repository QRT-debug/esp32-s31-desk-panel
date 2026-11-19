/**
 * @file    ui_statusbar.c
 * @brief   顶部状态栏：日期时间 + WiFi 扇形图标（4 条弧 + 中心点）
 *
 * ★ 建在 lv_layer_top 上：不随各场景的 lv_obj_clean(scr) 销毁，一次创建、全界面常驻。
 * ★ 必须去掉 CLICKABLE：不可点击的对象不会拦截下面场景的触摸。
 *
 * 图形：4 条同心弧（跨顶部 90°=LVGL 225°~315°）+ 中心点，按 RSSI 点亮 0~4 条：
 *   ≥-55 → 4 条 | ≥-66 → 3 | ≥-77 → 2 | ≥-88 → 1 | 更弱或未连接 → 0
 *
 * ★ 关于四角闪烁（2026-11-18 结论）：与 arc 无关——换成纯文字字形同样闪。
 *   真正原因是**开机瞬态**：WiFi 连接 / SNTP / 首次数据 / LVGL 缓存预热期间
 *   flash+PSRAM 访问量是稳态的数倍，与 RGB LCD 的 DMA 抢 MSPI 总线；
 *   负载降下来后闪烁自行消失（用户实测"运行一段时间就不明显、甚至没了"）。
 *   状态栏本身已做到"只在等级或分钟变化时才写"，静态零重绘。
 */
#include "ui_scenes.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui_fonts.h"
#include "ui_theme.h"
#include "sd_card.h"
#include "wifi_sta.h"

static const char *TAG = "statusbar";

#define SB_COL_ON    0x4C9AFF    /* 亮：蓝（品牌色恒定） */
#define SB_COL_OFF   (ui_theme()->sb_off)   /* 暗：随主题（白胶囊浅灰/深胶囊暗灰） */
#define SB_COL_SD    0x35D07F    /* SD 卡就绪：绿 */
#define SB_ARC_W     3

#define SB_RSSI_L4   (-55)
#define SB_RSSI_L3   (-66)
#define SB_RSSI_L2   (-77)
#define SB_RSSI_L1   (-88)

static lv_obj_t *s_arc[3];       /* 外 -> 内 */
static lv_obj_t *s_dot;
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_sd;       /* SD 卡图标 */
static lv_obj_t *s_cont;         /* 胶囊容器（主题刷新用） */

/* RSSI -> 0~4 条弧 */
static int rssi_to_level(int rssi)
{
    if (rssi == 0)          return 0;
    if (rssi >= SB_RSSI_L4) return 3;   /* 强 */
    if (rssi >= SB_RSSI_L2) return 2;   /* 中 */
    if (rssi >= SB_RSSI_L1) return 1;   /* 弱 */
    return 0;
}

static lv_obj_t *make_arc(lv_obj_t *parent, int dia, int x, int y)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, dia, dia);
    lv_obj_set_pos(a, x, y);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);

    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 225, 315);      /* 顶部 90° */
    lv_arc_set_angles(a, 225, 315);

    lv_obj_set_style_arc_width(a, SB_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, SB_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(SB_COL_OFF), LV_PART_INDICATOR);

    /* 去掉旋钮（否则弧两端会有圆点） */
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(a, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
    return a;
}

static void statusbar_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* ---- WiFi 强度：只在档位变化时改颜色（静态零重绘） ---- */
    int rssi  = wifi_sta_rssi();
    int level = rssi_to_level(rssi);

    static int last_drawn = -2;
    if (level != last_drawn)
    {
        last_drawn = level;
        /* ★ 从内向外点亮（信号越弱，亮起的圈越少）：
         *   s_arc[2]=最内圈（r=6）/ [1]=中（r=11）/ [0]=最外（r=16）
         *   level=3 全亮；2 亮内两圈；1 只亮最内圈；0 全灭
         *   之前的 (i < level) 是从外往里灭，与实际惯例相反（用户 11-18 抓出） */
        for (int i = 0; i < 3; i++)
        {
            bool on = (i >= (3 - level));
            lv_obj_set_style_arc_color(s_arc[i],
                                       lv_color_hex(on ? SB_COL_ON : SB_COL_OFF),
                                       LV_PART_INDICATOR);
        }
        lv_obj_set_style_bg_color(s_dot,
                                  lv_color_hex((level > 0) ? SB_COL_ON : SB_COL_OFF), 0);
        ESP_LOGI(TAG, "wifi icon: %d/3, rssi=%d dBm%s",
                 level, rssi, (wifi_sta_connected() ? "" : " (not connected)"));
    }

    /* ---- SD 卡：插拔变化时才改颜色 ---- */
    {
        static bool last_sd = false;
        bool sd = sd_card_poll();   /* 每秒轮询：拔卡/插卡都能感知 */
        if (sd != last_sd)
        {
            last_sd = sd;
            lv_obj_set_style_text_color(s_lbl_sd,
                                        lv_color_hex(sd ? SB_COL_SD : SB_COL_OFF), 0);
            ESP_LOGI(TAG, "sd card %s", sd ? "inserted" : "removed");
        }
    }

    /* ---- 日期时间：只在分钟变化时改文本 ---- */
    time_t now = 0;
    struct tm tm_now;
    time(&now);
    localtime_r(&now, &tm_now);

    char tb[24];
    if (tm_now.tm_year + 1900 < 2020)
    {
        snprintf(tb, sizeof(tb), "--:-- --:--");
    }
    else
    {
        snprintf(tb, sizeof(tb), "%02d-%02d %02d:%02d",
                 tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min);
    }

    static char last_tb[24] = "";
    if (strcmp(tb, last_tb) != 0)
    {
        snprintf(last_tb, sizeof(last_tb), "%s", tb);
        lv_label_set_text(s_lbl_time, tb);
    }

    /* 首次拿到有效时间时打一条日志（确认 SNTP 同步成功） */
    static bool time_logged;
    if (!time_logged && tm_now.tm_year + 1900 >= 2020)
    {
        time_logged = true;
        ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d:%02d (TZ CST-8)",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }
}

/* ★ 主题化：所有一次性设置的颜色集中这里，init / refresh 共用 */
static void statusbar_apply_theme(void)
{
    lv_obj_set_style_bg_color(s_cont, lv_color_hex(ui_theme()->sb_capsule), 0);
    lv_obj_set_style_shadow_color(s_cont, lv_color_hex(ui_theme()->shadow), 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(ui_theme()->sb_text), 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(SB_COL_OFF), 0);
    lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(SB_COL_OFF), 0);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_set_style_arc_color(s_arc[i], lv_color_hex(SB_COL_OFF), LV_PART_INDICATOR);
    }
    /* 按当前实际状态重画图标（切主题后亮色弧/绿 SD 立即恢复正确颜色） */
    int level = rssi_to_level(wifi_sta_rssi());
    for (int i = 0; i < 3; i++)
    {
        bool on = (i >= (3 - level));
        lv_obj_set_style_arc_color(s_arc[i],
                                   lv_color_hex(on ? SB_COL_ON : SB_COL_OFF),
                                   LV_PART_INDICATOR);
    }
    lv_obj_set_style_bg_color(s_dot,
                              lv_color_hex((level > 0) ? SB_COL_ON : SB_COL_OFF), 0);
    lv_obj_set_style_text_color(s_lbl_sd,
                                lv_color_hex(sd_card_poll() ? SB_COL_SD : SB_COL_OFF), 0);
}

void ui_statusbar_refresh(void)
{
    if (s_cont != NULL)
    {
        statusbar_apply_theme();
    }
}

void ui_statusbar_init(void)
{
    if (s_arc[0] != NULL)
    {
        return;
    }

    lv_obj_t *top = lv_layer_top();

    /* 容器：右上角 132x26（时间约 77px + 图标 30px + 间距） */
    lv_obj_t *cont = lv_obj_create(top);
    lv_obj_set_size(cont, 216, 30);
    lv_obj_align(cont, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_obj_set_style_bg_opa(cont, LV_OPA_80, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_radius(cont, 15, 0);                                  /* 胶囊 */
    lv_obj_set_style_shadow_width(cont, 12, 0);
    lv_obj_set_style_shadow_opa(cont, LV_OPA_20, 0);
    lv_obj_set_style_shadow_offset_y(cont, 2, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    s_cont = cont;

    /* 四条弧共用圆心 (115,23)，位置 = 圆心 - 半径 */
    s_arc[0] = make_arc(cont, 32, 184, 5);  /* r=16 最外 */
    s_arc[1] = make_arc(cont, 22, 189, 10); /* r=11 */
    s_arc[2] = make_arc(cont, 12, 194, 15); /* r=6  */

    /* 中心圆点 */
    s_dot = lv_obj_create(cont);
    lv_obj_set_size(s_dot, 4, 4);
    lv_obj_set_pos(s_dot, 198, 19);
    lv_obj_set_style_radius(s_dot, 2, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_pad_all(s_dot, 0, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);

    /* SD 卡状态图标：插卡=绿，无卡=灰 */
    s_lbl_sd = lv_label_create(cont);
    lv_obj_set_style_text_font(s_lbl_sd, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lbl_sd, LV_SYMBOL_SD_CARD);
    lv_obj_set_pos(s_lbl_sd, 152, 6);
    lv_obj_clear_flag(s_lbl_sd, LV_OBJ_FLAG_CLICKABLE);

    /* 日期时间（左） */
    s_lbl_time = lv_label_create(cont);
    lv_obj_set_style_text_font(s_lbl_time, &ui_font_24, 0);
    lv_label_set_text(s_lbl_time, "--:-- --:--");
    /* ★ 与圆点底边对齐（按字体实际参数算）：
     *   ui_font_24: line_height=24, base_line=5
     *   圆点底 y=23 = 数字基线 ⇒ label 底 = 23+5 = 28 ⇒ label 顶 = 28-24 = 4 */
    lv_obj_set_pos(s_lbl_time, 8, 4);   /* 左边界留 8px，避免贴着胶囊圆角 */
    lv_obj_clear_flag(s_lbl_time, LV_OBJ_FLAG_CLICKABLE);

    /* 立即刷一次 + 每秒检查（内部有变化才写） */
    statusbar_apply_theme();
    statusbar_timer_cb(NULL);
    lv_timer_create(statusbar_timer_cb, 1000, NULL);
}
