/**
 ******************************************************************************
 * @file        ui.c
 * @version     V1.0
 * @brief       触摸 UI界面
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "ui.h"
#include "touch.h"
#include "lcd.h"
#include "text.h"
#include "led.h"
#include "recorder.h"
#include "audioplay.h"
#include "wavplay.h"
#include "myes8389.h"
#include "exfuns.h"
#include "ff.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------- 页面 ---------------- */
typedef enum {
    PAGE_MENU = 0,                  /* 主菜单 */
    PAGE_REC,                       /* 录音页 */
    PAGE_PLAY                       /* 播放录音页 */
} ui_page_t;

/* ---------------- 按钮区域 (x1/y1 为右下开区间) ---------------- */
typedef struct { uint16_t x0, y0, x1, y1; } ui_rect_t;

static const ui_rect_t BTN_REC  = {170, 180, 350, 280};      /* 主菜单: 录音 */
static const ui_rect_t BTN_PLAY = {450, 180, 630, 280};      /* 主菜单: 播放录音 */
static const ui_rect_t BTN_EXIT = {720,  10, 790,  46};      /* 播放页右上角: 退出 */
static const ui_rect_t BTN_BOT1 = { 80, 400, 280, 460};      /* 底栏左  */
static const ui_rect_t BTN_BOT2 = {300, 400, 500, 460};      /* 底栏中  */
static const ui_rect_t BTN_BOT3 = {520, 400, 720, 460};      /* 底栏右  */
static const ui_rect_t BTN_VOL_DN = {328, 345, 388, 395};    /* 播放页: 音量 - */
static const ui_rect_t BTN_VOL_UP = {464, 345, 524, 395};    /* 播放页: 音量 + */

/* 音量行文字位置 (整行以屏幕水平中心 400 居中) */
#define VOL_LABEL_X   276                       /* "音量" 标签 */
#define VOL_NUM_X     408                       /* 音量数值 (3位, 24号字) */
#define VOL_ROW_Y     362                       /* 标签基线 */
#define VOL_NUM_Y     358                       /* 数值基线 */

#define VOL_STEP      10                        /* 每次点击调整的音量步进 */

/* ---------------- 状态 ---------------- */
static ui_page_t s_page = PAGE_MENU;            /* 当前页面 */

/* 录音页 */
static TickType_t s_rec_last_mark  = 0;         /* 上次计时打点 */
static uint32_t   s_rec_ticks      = 0;         /* 累计录音 tick (不含暂停时间) */
static uint32_t   s_rec_sec_shown  = 0xFFFFFFFF;/* 已显示的秒数(用于去重) */
static TickType_t s_rec_enter      = 0;         /* 进入录音页时刻(用于判断打开文件失败) */
static uint8_t    s_rec_btn_state  = 0xFF;      /* 按钮状态缓存: bit0=录音中 bit1=暂停 */

/* 播放页 */
static uint8_t **s_files        = NULL;         /* 录音文件名表 */
static uint16_t  s_file_num     = 0;            /* 录音文件个数 */
static uint16_t  s_cur_index    = 0;            /* 当前播放序号 */
static uint8_t   s_play_active  = 0;            /* 1=本曲正在播放会话中 */
static uint8_t   s_play_paused  = 0;            /* 1=本曲已暂停 */
static TickType_t s_play_mark   = 0;            /* 播放时间刷新打点 */

/* 没有录音文件时的闪烁提示 */
static uint8_t    s_blink_on    = 1;
static TickType_t s_blink_mark  = 0;

/* LED 指示 */
static TickType_t s_led_mark    = 0;
static uint8_t    s_led_on      = 0;

/* ---------------- 基本绘制 ---------------- */
/**
 * @brief       判断触摸点是否落在按钮区域内
 * @param       r  按钮区域
 * @param       x  触摸点 X 坐标
 * @param       y  触摸点 Y 坐标
 * @retval      1=落在区域内  0=区域外
 */
static uint8_t ui_in_rect(const ui_rect_t *r, uint16_t x, uint16_t y)
{
    return (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1) ? 1 : 0;
}

/**
 * @brief       绘制文字按钮 (汉字宽度=字号, 自动居中)
 * @param       r        按钮区域
 * @param       label    按钮文字 (GB2312 编码)
 * @param       size     字号
 * @param       color    按钮颜色
 * @param       enabled  0=灰色不可用
 * @retval      无
 */
static void ui_draw_button(const ui_rect_t *r, const char *label, uint8_t size, uint16_t color, uint8_t enabled)
{
    uint16_t c  = enabled ? color : GRAY;
    uint16_t tw = (uint16_t)(strlen(label) * size / 2);
    uint16_t tx = r->x0 + ((r->x1 - r->x0) - tw) / 2;
    uint16_t ty = r->y0 + ((r->y1 - r->y0) - size) / 2;

    lcd_fill(r->x0, r->y0, r->x1, r->y1, WHITE);                        /* 清底 */
    lcd_draw_rectangle(r->x0, r->y0, r->x1 - 1, r->y1 - 1, c);          /* 边框 */
    text_show_string(tx, ty, lcddev.width, lcddev.height, (char *)label, size, 0, c);
}

/**
 * @brief       播放录音页右上角绘制红色"退出"按钮
 * @note        录音页无此按钮, 只能通过底部"保存并退出"离开
 * @param       无
 * @retval      无
 */
static void ui_draw_exit(void)
{
    ui_draw_button(&BTN_EXIT, "退出", 16, RED, 1);
}

/* -------------- 主菜单页 ---------------- */
/**
 * @brief       绘制主菜单
 * @param       无
 * @retval      无
 */
static void ui_menu_enter(void)
{
    s_page = PAGE_MENU;
    lcd_clear(WHITE);

    text_show_string(30, 30, 400, 16, "慧勤智远 ESP32-S31 开发板", 16, 0, RED);
    text_show_string(30, 50, 400, 16, "录音机实验", 16, 0, RED);
    text_show_string(30, 70, 400, 16, "WKS SMART", 16, 0, RED);

    ui_draw_button(&BTN_REC,  "录音",     24, BLUE, 1);
    ui_draw_button(&BTN_PLAY, "播放录音", 24, BLUE, 1);
}

/* -------------- 录音页 ---------------- */
/**
 * @brief       绘制录音页底部按钮
 * @note        录音中"暂停"高亮, 暂停时"开始"高亮
 * @param       无
 * @retval      无
 */
static void ui_rec_buttons_refresh(void)
{
    uint8_t active = recorder_is_active();
    uint8_t paused = recorder_is_paused();

    s_rec_btn_state = (uint8_t)((active ? 1 : 0) | (paused ? 2 : 0));

    ui_draw_button(&BTN_BOT1, "暂停",       16, BLUE, (active && !paused) ? 1 : 0);
    ui_draw_button(&BTN_BOT2, "开始",       16, BLUE, (active &&  paused) ? 1 : 0);
    ui_draw_button(&BTN_BOT3, "保存并退出", 16, BLUE, active);
}

/**
 * @brief       显示/清除暂停提示
 * @param       paused  1=显示 "Paused"  0=清除
 * @retval      无
 */
static void ui_rec_show_paused(uint8_t paused)
{
    if (paused) text_show_string(30, 230, lcddev.width, 16, "Paused", 16, 0, RED);
    else        lcd_fill(30, 230, lcddev.width, 250, WHITE);
}

/**
 * @brief       重绘录音页
 * @param       无
 * @retval      无
 */
static void ui_rec_redraw(void)
{
    lcd_clear(WHITE);
    text_show_string(30, 40, 400, 32, "录音", 24, 0, RED);

    ui_rec_buttons_refresh();
    ui_rec_show_paused(recorder_is_paused());
    recoder_msg_show(s_rec_sec_shown == 0xFFFFFFFF ? 0 : s_rec_sec_shown, recorder_bitrate());
}

/**
 * @brief       进入录音页并立即开始录音
 * @note        先画页面再启动录音, 避免后端显示的 "REC: 文件名" 被清屏擦掉
 * @param       无
 * @retval      无
 */
static void ui_rec_enter(void)
{
    s_page = PAGE_REC;

    s_rec_ticks     = 0;
    s_rec_sec_shown = 0xFFFFFFFF;
    s_rec_last_mark = xTaskGetTickCount();
    s_rec_enter     = s_rec_last_mark;

    ui_rec_redraw();
    recorder_toggle();          /* 开始录音 (异步) */
}

/**
 * @brief       录音页逻辑轮询: 计时显示 + 状态同步
 * @param       无
 * @retval      无
 */
static void ui_rec_poll(void)
{
    TickType_t now = xTaskGetTickCount();
    uint8_t active = recorder_is_active();
    uint8_t paused = recorder_is_paused();

    /* --- 计时 (暂停时间不计入) --- */
    if (active && !paused) s_rec_ticks += (uint32_t)(now - s_rec_last_mark);
    s_rec_last_mark = now;

    uint32_t sec = s_rec_ticks * portTICK_PERIOD_MS / 1000;
    if (active && sec != s_rec_sec_shown) {
        s_rec_sec_shown = sec;
        recoder_msg_show(sec, recorder_bitrate());
    }

    /* --- 按钮/暂停提示 状态变化时重绘 --- */
    uint8_t st = (uint8_t)((active ? 1 : 0) | (paused ? 2 : 0));
    if (st != s_rec_btn_state) {
        ui_rec_buttons_refresh();
        ui_rec_show_paused(paused);
    }

    /* --- 录音文件打开失败: 返回主菜单 (错误提示由录音后端显示) --- */
    if (!active && (now - s_rec_enter) > pdMS_TO_TICKS(500)) {
        ui_menu_enter();
    }
}

/* -------------- 播放录音页 ---------------- */
/**
 * @brief       释放录音文件名表
 * @param       无
 * @retval      无
 */
static void ui_play_free_list(void)
{
    if (s_files) {
        for (uint16_t i = 0; i < s_file_num; i++) {
            if (s_files[i]) free(s_files[i]);
        }
        free(s_files);
        s_files = NULL;
    }
    s_file_num = 0;
}

/**
 * @brief       扫描 SD 卡 RECORDER 目录, 建立录音文件名表
 * @note        按目录顺序排列, 最后一个为最近一次录音
 * @param       无
 * @retval      无
 */
static void ui_play_load_list(void)
{
    ui_play_free_list();

    s_file_num = audio_get_tnum((uint8_t *)"0:/RECORDER");
    if (s_file_num == 0)  return;

    s_files = (uint8_t **)malloc(s_file_num * sizeof(uint8_t *));
    if (s_files == NULL) 
    { 
        s_file_num = 0; return; 
    }
    for (uint16_t i = 0; i < s_file_num; i++)  s_files[i] = NULL;

    FF_DIR  dir;
    FILINFO *fi = (FILINFO *)malloc(sizeof(FILINFO));
    if (fi == NULL) 
    { 
        ui_play_free_list(); return; 
    }

    if (f_opendir(&dir, "0:/RECORDER") == FR_OK) {
        uint16_t i = 0;
        while (f_readdir(&dir, fi) == FR_OK && fi->fname[0] != 0) {
            if ((exfuns_file_type(fi->fname) & 0xF0) == 0x40) {      /* 0x4x = WAV */
                s_files[i++] = (uint8_t *)strdup((const char *)fi->fname);
                if (i >= s_file_num) break;
            }
        }
        f_closedir(&dir);
    }
    free(fi);
}

/**
 * @brief       显示当前曲目信息 (文件名 / 时间 / 序号)
 * @param       无
 * @retval      无
 */
static void ui_play_show_info(void)
{
    lcd_fill(30, 170, lcddev.width - 20, 250, WHITE);

    if (s_files && s_files[s_cur_index]) {
        text_show_string(30, 190, 400, 16, (char *)s_files[s_cur_index], 16, 0, RED);
    }
    audio_index_show(s_cur_index + 1, s_file_num);
}

/**
 * @brief       刷新播放页底栏按钮 (上一个 / 暂停|继续 / 下一个)
 * @param       无
 * @retval      无
 */
static void ui_play_buttons_refresh(void)
{
    ui_draw_button(&BTN_BOT1, "上一个", 16, BLUE, 1);
    ui_draw_button(&BTN_BOT2, s_play_paused ? "继续" : "暂停", 16, BLUE, 1);
    ui_draw_button(&BTN_BOT3, "下一个", 16, BLUE, 1);
}

/**
 * @brief       刷新音量数值 (只重画数值, 不动按钮和标签)
 * @param       无
 * @retval      无
 */
static void ui_play_volume_show(void)
{
    /* mode=0x00: 高位补空格, 非叠加(背景填 g_back_color) */
    lcd_show_xnum(VOL_NUM_X, VOL_NUM_Y, myes8389_get_volume(), 3, 24, 0x00, RED);
}

/**
 * @brief       绘制播放页音量行: 音量 - [数值] +
 * @param       无
 * @retval      无
 */
static void ui_play_volume_draw(void)
{
    text_show_string(VOL_LABEL_X, VOL_ROW_Y, 64, 16, "音量", 16, 0, RED);
    ui_draw_button(&BTN_VOL_DN, "-", 24, BLUE, 1);
    ui_draw_button(&BTN_VOL_UP, "+", 24, BLUE, 1);
    ui_play_volume_show();
}

/**
 * @brief       播放指定序号的录音文件
 * @note        文件异常时自动跳到更早的一个, 全部失败则显示错误
 * @param       index  曲目序号 (0 起)
 * @retval      无
 */
static void ui_play_play_index(uint16_t index)
{
    uint8_t path[64];
    uint16_t tries = 0;

    s_play_active = 0;
    s_play_paused = 0;

    while (tries < s_file_num) {
        if (index >= s_file_num || s_files == NULL || s_files[index] == NULL) return;

        s_cur_index = index;
        ui_play_show_info();

        sprintf((char *)path, "0:/RECORDER/%s", (char *)s_files[s_cur_index]);
        if (wav_play_start(path) == 0) {
            s_play_active = 1;
            s_play_mark   = xTaskGetTickCount();
            wav_play_update_time();
            audio_msg_show(wav_play_totsec(), 0, wav_play_bitrate());
            ui_play_buttons_refresh();
            return;
        }
        index = (index == 0) ? (s_file_num - 1) : (index - 1);
        tries++;
    }

    lcd_fill(30, 170, lcddev.width - 20, 250, WHITE);
    text_show_string(30, 190, 400, 16, "录音文件无法播放!", 16, 0, RED);
}

/**
 * @brief       取上一个曲目序号 (更早的录音, 循环)
 * @param       无
 * @retval      曲目序号
 */
static uint16_t ui_play_prev_index(void)
{
    return (s_cur_index == 0) ? (s_file_num - 1) : (s_cur_index - 1);
}

/**
 * @brief       取下一个曲目序号 (更新的录音, 循环)
 * @param       无
 * @retval      曲目序号
 */
static uint16_t ui_play_next_index(void)
{
    return (s_cur_index + 1 >= s_file_num) ? 0 : (s_cur_index + 1);
}

/**
 * @brief       没有录音文件时的红色闪烁提示 (250ms 交替显示/清除)
 * @param       无
 * @retval      无
 */
static void ui_play_blink_no_file(void)
{
    TickType_t now = xTaskGetTickCount();
    if (now - s_blink_mark < pdMS_TO_TICKS(250)) return;
    s_blink_mark = now;
    s_blink_on = !s_blink_on;

    if (s_blink_on)
    {
       text_show_string(30, 190, 400, 16, "没有录音文件", 16, 0, RED);
    } 
    else
    {
       lcd_fill(30, 190, 300, 206, WHITE);
    }           
}

/**
 * @brief       进入播放录音页, 从最近一次录音开始播放
 * @note        没有录音文件时只显示闪烁提示, 点击屏幕任意处返回
 * @param       无
 * @retval      无
 */
static void ui_play_enter(void)
{
    s_page = PAGE_PLAY;
    s_play_active = 0;
    s_play_paused = 0;
    s_cur_index   = 0;
    s_blink_on    = 1;
    s_blink_mark  = xTaskGetTickCount();

    lcd_clear(WHITE);
    ui_draw_exit();
    text_show_string(30, 40, 400, 32, "播放录音", 24, 0, BLUE);

    ui_play_load_list();

    if (s_file_num == 0) {
        ui_play_blink_no_file();
        //text_show_string(30, 420, 400, 16, "点击屏幕返回主菜单", 16, 0, BLUE);
        return;
    }

    ui_play_buttons_refresh();
    ui_play_volume_draw();
    myes8389_play_start(I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);   /* 开 DAC + 功放 */
    ui_play_play_index(s_file_num - 1);                               /* 最近一次录音 */
}

/**
 * @brief       退出播放录音页: 停止播放并释放资源
 * @param       无
 * @retval      无
 */
static void ui_play_exit(void)
{
    wav_play_release();
    myes8389_play_stop();

    ui_play_free_list();
    s_play_active = 0;
    s_play_paused = 0;
}

/**
 * @brief       播放页逻辑轮询: 时间刷新 + 播完自动切上一个
 * @param       无
 * @retval      无
 */
static void ui_play_poll(void)
{
    if (s_file_num == 0) {
        ui_play_blink_no_file();
        return;
    }

    /* --- 本曲播完 → 自动播放更早的一个 --- */
    if (s_play_active && wav_play_is_done()) {
        ui_play_play_index(ui_play_prev_index());
        return;
    }

    /* --- 每秒刷新一次播放时间 --- */
    TickType_t now = xTaskGetTickCount();
    if (s_play_active && (now - s_play_mark) >= pdMS_TO_TICKS(1000)) {
        s_play_mark = now;
        wav_play_update_time();
        audio_msg_show(wav_play_totsec(), wav_play_cursec(), wav_play_bitrate());
    }
}

/* ---------------- 点击处理 ---------------- */
/**
 * @brief       保存当前录音并返回主菜单
 * @note        保存失败时留在录音页并显示提示
 * @param       无
 * @retval      无
 */
static void ui_rec_save_and_exit(void)
{
    if (recorder_is_active() && !recorder_save()) {
        text_show_string(30, 250, 400, 16, "保存失败!", 16, 0, RED);
        vTaskDelay(pdMS_TO_TICKS(800));
        lcd_fill(30, 250, 300, 266, WHITE);
        return;
    }
    ui_menu_enter();
}

/**
 * @brief       处理一次点击 (按下瞬间触发)
 * @param       x  触摸点 X 坐标
 * @param       y  触摸点 Y 坐标
 * @retval      无
 */
static void ui_handle_tap(uint16_t x, uint16_t y)
{
    /* --- 播放录音页右上角: 退出 (录音页没有此按钮, 只用底部"保存并退出") --- */
    if (s_page == PAGE_PLAY && ui_in_rect(&BTN_EXIT, x, y)) {
        ui_play_exit();
        ui_menu_enter();
        return;
    }

    switch (s_page) {
    /* ---------- 主菜单 ---------- */
    case PAGE_MENU:
        if (ui_in_rect(&BTN_REC, x, y))       ui_rec_enter();
        else if (ui_in_rect(&BTN_PLAY, x, y)) ui_play_enter();
        break;

    /* ---------- 录音页 ---------- */
    case PAGE_REC:
        if (ui_in_rect(&BTN_BOT1, x, y)) {                  /* 暂停 */
            if (recorder_is_active() && !recorder_is_paused())  recorder_toggle();
        } else if (ui_in_rect(&BTN_BOT2, x, y)) {           /* 开始 (从暂停恢复) */
            if (recorder_is_paused())   recorder_toggle();
        } else if (ui_in_rect(&BTN_BOT3, x, y)) {           /* 保存并退出 */
            ui_rec_save_and_exit();
        }
        break;

    /* ---------- 播放页 ---------- */
    case PAGE_PLAY:
        if (s_file_num == 0) { ui_play_exit(); ui_menu_enter(); break; }   /* 无文件: 点击返回 */
        if (ui_in_rect(&BTN_BOT1, x, y)) {                  /* 上一个 */
            ui_play_play_index(ui_play_prev_index());
        } else if (ui_in_rect(&BTN_BOT2, x, y)) {           /* 暂停 / 继续 */
            if (s_play_paused)  { wav_play_resume(); s_play_paused = 0; }
            else                { wav_play_pause();  s_play_paused = 1; }
            ui_play_buttons_refresh();
        } else if (ui_in_rect(&BTN_BOT3, x, y)) {           /* 下一个 */
            ui_play_play_index(ui_play_next_index());
        } else if (ui_in_rect(&BTN_VOL_DN, x, y)) {         /* 音量 - */
            myes8389_set_volume(myes8389_get_volume() - VOL_STEP);
            ui_play_volume_show();
        } else if (ui_in_rect(&BTN_VOL_UP, x, y)) {         /* 音量 + */
            myes8389_set_volume(myes8389_get_volume() + VOL_STEP);
            ui_play_volume_show();
        }
        break;
    }
}

/* ------------- UI 主循环 ------------- */
/**
 * @brief       UI 主循环: 触摸扫描 + 页面逻辑 + LED 指示 (不会返回)
 * @param       无
 * @retval      无
 */
void ui_run(void)
{
    uint8_t touch_prev = 0;

    ui_menu_enter();                            /* 开机进入主菜单 */

    while (1) {
        /* --- 触摸扫描 (按下瞬间触发一次) --- */
        tp_dev.scan(0);
        uint8_t down = (tp_dev.sta & TP_PRES_DOWN) ? 1 : 0;
        if (down && !touch_prev) {
            uint16_t tx = tp_dev.x[0];
            uint16_t ty = tp_dev.y[0];
            if (tx < lcddev.width && ty < lcddev.height) 
            {
                ui_handle_tap(tx, ty);
            }
        }
        touch_prev = down;

        /* --- 页面逻辑 --- */
        switch (s_page) {
            case PAGE_REC:  ui_rec_poll();  break;
            case PAGE_PLAY: ui_play_poll(); break;
            default:                        break;
        }

        /* --- LED 闪烁指示: 主菜单=蓝 录音=红 播放=绿 --- */
        TickType_t now = xTaskGetTickCount();
        if (now - s_led_mark >= pdMS_TO_TICKS(200)) {
            s_led_mark = now;
            s_led_on = !s_led_on;
            if (s_led_on) {
                led_set_rgb(s_page == PAGE_MENU ? LED_RGB_BLUE :
                            (s_page == PAGE_REC  ? LED_RGB_RED  : LED_RGB_GREEN));
            } else {
                led_off();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
