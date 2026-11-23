/**
 * @file    audio_internal.h
 * @brief   BSP/AUDIO 组件**内部**共享（不对上层暴露）
 *
 * 麦克风模块（audio_rec.c）需要拿到播放器（audio_player.c）创建的 I2S RX 句柄，
 * 但句柄的创建/开关由播放器统一管理（播放和录音共用一路 I2S 控制器，时钟要重配）。
 * 所以这里只开两个只读入口，避免把整个 audio_player 的内部状态暴露出去。
 *
 * ★ 不要把本文件 include 到 BSP 之外。
 */
#pragma once

#include <stdbool.h>

#include "driver/i2s_std.h"

/** 取 I2S RX 句柄（播放器初始化时创建；未初始化返回 NULL） */
i2s_chan_handle_t audio_priv_rx(void);

/** rec 句柄当前是否 open（播放态为 false） */
bool audio_priv_rec_open(void);
