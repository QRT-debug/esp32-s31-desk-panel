/**
 * @file    ui_theme.c
 * @brief   浅色/深色两套色板 + NVS 持久化
 *
 * 深色板设计参考手表/手机暗色模式：底色蓝黑（不是纯黑，避免 OLED 视觉断层），
 * 卡片微亮一档，文字按 3 档亮度递减。品牌色（紫/蓝/橙/绿/红/黄）两套同值。
 */
#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "ui_theme";

#ifdef PC_SIM
/* PC 模拟器：无 NVS，主题只在内存态（默认浅色，开关即时生效不持久化） */
static void theme_save(bool dark) { (void)dark; }
static void theme_load(bool *dark) { *dark = false; }
#else
#include "nvs_flash.h"
#include "nvs.h"

static void theme_save(bool dark)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "dark", dark ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    else
    {
        ESP_LOGW(TAG, "nvs open failed, theme not persisted");
    }
}

static void theme_load(bool *dark)
{
    nvs_handle_t h;
    uint8_t v = 0;
    *dark = false;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_u8(h, "dark", &v) == ESP_OK)
        {
            *dark = (v != 0);
        }
        nvs_close(h);
    }
}
#endif

/* ---------------- 浅色（原 4 页 UI 的色值原样搬入） ---------------- */
static const ui_theme_t s_light = {
    .bg_top    = 0xE8EBF3,
    .bg_bot    = 0xDCE0EC,
    .card      = 0xFAFBFF,
    .card_line = 0xD5DAE8,
    .title     = 0x1E2230,
    .sub       = 0x8A90A0,
    .faint     = 0xB6BCCB,
    .shadow    = 0xC9C2F0,
    .accent    = 0x6C4CF1,
    .accent_l  = 0xE7E3FC,
    .accent_l2 = 0xD6CDFF,
    .track     = 0xE2E6F0,
    .panel_dark  = 0x161A22,
    .panel_line  = 0x3A4050,
    .panel_text  = 0xE6EAF0,
    .panel_dim   = 0x8A93A6,
    .sb_capsule  = 0xFFFFFF,
    .sb_text     = 0x3A4050,
    .sb_off      = 0xB8BECC,
};

/* ---------------- 深色 ---------------- */
static const ui_theme_t s_dark = {
    .bg_top    = 0x14171E,   /* 蓝黑，非纯黑（纯黑显断层+指纹反光） */
    .bg_bot    = 0x0F1117,
    .card      = 0x1D212C,   /* 卡片亮一档出层次 */
    .card_line = 0x2E3542,
    .title     = 0xE6EAF0,
    .sub       = 0x8A93A6,
    .faint     = 0x566073,
    .shadow    = 0x0A0C10,   /* 深色下阴影加深色底即可 */
    .accent    = 0x7B5CFF,   /* 紫提亮一档（深底上 6C4CF1 偏闷） */
    .accent_l  = 0x2A2545,
    .accent_l2 = 0x36305C,
    .track     = 0x2A303E,
    .panel_dark  = 0x1E2430,
    .panel_line  = 0x45506B,
    .panel_text  = 0xE6EAF0,
    .panel_dim   = 0x8A93A6,
    .sb_capsule  = 0x22262F,
    .sb_text     = 0xB8BFCE,
    .sb_off      = 0x5A6273,
};

static const ui_theme_t *s_cur = &s_light;
static bool s_dark_mode = false;

const ui_theme_t *ui_theme(void)
{
    return s_cur;
}

bool ui_theme_is_dark(void)
{
    return s_dark_mode;
}

void ui_theme_set(bool dark)
{
    s_dark_mode = dark;
    s_cur = dark ? &s_dark : &s_light;
    theme_save(dark);
    ESP_LOGI(TAG, "theme -> %s", dark ? "DARK" : "LIGHT");
}

void ui_theme_init(void)
{
    bool dark = false;
    theme_load(&dark);
    s_dark_mode = dark;
    s_cur = dark ? &s_dark : &s_light;
    ESP_LOGI(TAG, "theme init: %s", s_dark_mode ? "DARK" : "LIGHT");
}
