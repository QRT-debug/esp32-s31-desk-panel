/**
 * @file    audio_rec.h
 * @brief   板载麦克风采集（ES8389 ADC + I2S RX）
 *
 * 从 audio_player.h 拆出来的原因：录音和"播放器"是两件事，
 * 挂在播放器名字下会让调用方误以为要动播放状态。
 *
 * 硬件事实（实测）：
 *   - I2S RX 是**双声道交错**，右声道有 ~1100 的固定直流偏置（单端输入）；
 *   - MIC1 走 codec 的 MIC2 通道，具体是左还是右未定 ⇒ 本模块**首次有信号时
 *     自动判定一次并整段锁定**（绝不能逐块重判，否则两路能量接近时通道会跳，
 *     音频被切成两段不同信号，ASR 直接听不懂）。
 *
 * ★ 只在 REC 态可用：播放中 rec 句柄已关，读会返回 0 / -1。
 */
#pragma once

#include <stdint.h>

/**
 * 读一段 PCM，转成单声道 16bit（16 kHz）—— 给语音识别用。
 * 两路各自去直流，选能量大的那一路输出。
 *
 * @param out          输出缓冲（int16 单声道）
 * @param max_samples  输出缓冲容量（样本数）
 * @param timeout_ms   阻塞超时
 * @return 实际输出样本数；<0 失败（播放态 / 句柄未开）
 */
int audio_rec_read_mono(int16_t *out, int max_samples, int timeout_ms);

/** 每次开始录音前调用：重置通道判定，让本次重新锁定（防跨次残留） */
void audio_rec_begin(void);

/**
 * 读一次麦克风电平（阻塞约 20ms）。
 * @return 0~100 的峰值电平（0 = 静音；显示放大 ×4，否则环境噪声看不出跳动）
 */
int audio_rec_level(void);

/** ★ 诊断：读一帧并打印每声道 DC / min / max（排查"有无数据/直流/毛刺"用） */
void audio_rec_debug_log(void);
