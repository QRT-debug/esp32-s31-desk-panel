/**
 * @file    audio_player.h
 * @brief   ES8389 + I2S 音乐文件播放
 *
 * 硬件（原理图 page-3 活体）：
 *   U3  ES8389 codec  I2C 0x10（8-bit 写地址 0x20），挂 I2C0（IO0/IO1）
 *   U4/U5 NS4150B     功放，PA_CTRL = IO7 高有效
 *   I2S: BCLK=IO3  WS=IO4  DOUT=IO5  MCLK 关（R56 NC，codec 从模式）
 *
 * ★ 已验证配置来源：kmyclab/esp32-s31-korvo1-bringup examples/05
 *   （同构板二手实测：STD TX / 16bit / MCLK disabled / slave；本工程用 44.1k）
 * ★ codec 走官方 esp_codec_dev 组件，不手写寄存器序列（防错误驱动发热）。
 * ★ 音乐文件 = flash vfs FAT 分区（只读挂载 /music）里的 44.1kHz/16bit WAV。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 初始化 I2S + ES8389，打开后处于静音状态（mute=true）。
 *  需在 i2c_bus_init（touch_service_start 内）之后调用。 */
esp_err_t audio_player_init(void);

/** 播放器状态机：LOADING = 整曲从 flash 预载 PSRAM（数秒，期间可 STOP 取消） */
typedef enum
{
    AUD_STATE_IDLE = 0,
    AUD_STATE_LOADING,
    AUD_STATE_PLAYING,
} audio_player_state_t;

/** 当前状态（UI 轮询用，播放中/加载中均可 STOP） */
audio_player_state_t audio_player_state(void);

bool audio_player_is_playing(void);

/** 挂载音乐 FAT 分区（只读，分区标签 vfs → 挂到 /music）。
 *  在 audio_player_init 之后调用一次。 */
esp_err_t audio_player_mount(void);

/** 播放 /music 下的一个 WAV 文件（44.1kHz/16bit，mono 自动展开立体声）。
 *  正在播时调用 = 切歌（先停当前再起新的）。 */
esp_err_t audio_player_play_file(const char *filename);

/** 播放当前选中曲目（play_file 记住的最后一个文件名） */
esp_err_t audio_player_play(void);

/** 播放 /sdcard 根目录下的第一个 WAV（流式读，不预载；SD 未挂载返回错误） */
esp_err_t audio_player_play_sd(void);

/** 按文件名播放 /sdcard/<name>（流式读；name 为目录项里的名字，含 .wav） */
esp_err_t audio_player_play_sd_by_name(const char *name);

/** 当前曲目文件名（UI 高亮/显示用） */
const char *audio_player_current_file(void);

/** 停止播放（静音后自动 mute） */
esp_err_t audio_player_stop(void);

/** ★ 嘶嘶声分层定位（诊断用，播放中会先被复位为 0）：
 *  0 = 正常（DAC 开、PA 开）
 *  1 = DAC 数字静音，PA 仍开  → 此时若还有嘶嘶 ⇒ 功放/电源底噪（与 codec 输出无关）
 *  2 = PA 关（DAC 正常输出）  → 对照：喇叭应完全安静
 *  3 = DAC 静音 + PA 关       → 全静音对照
 *  4 = 数字静音音源（数据全 0，通路全开）→ 有嘶嘶 = 通路自噪；安静 = 与数据有关
 *  5 = 1kHz 正弦音源（-20dBFS，数字生成绝对干净）→ 有嘶嘶 = 信号相关失真/位对齐问题 */
esp_err_t audio_player_diag_set(int mode);
int       audio_player_diag_get(void);

/** 音量 0~100（写入 ES8389 DAC） */
esp_err_t audio_player_set_volume(int vol);
int      audio_player_get_volume(void);

/** 当前播放位置（毫秒）；未播放返回 0 */
uint32_t audio_player_position_ms(void);

/** 当前曲目总时长（毫秒）；未知返回 0 */
uint32_t audio_player_duration_ms(void);

/* ★ 麦克风采集已拆到 audio_rec.h（录音与播放器是两件事） */
