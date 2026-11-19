/**
 ******************************************************************************
 * @file        recorder.h
 * @version     V2.0
 * @brief       WAV 录音头文件
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __RECORDER_H
#define __RECORDER_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "audioplay.h"
#include "ff.h"
#include "myes8389.h"
#include "led.h"
#include "wavplay.h"
#include "text.h"
#include "myi2s.h"

#define REC_RX_BUF_SIZE     4*1024        /* RX 缓冲大小 */

/* ---- 录音后端 API (由 UI 调用, 内部任务异步执行) ---- */
void     recorder_init(void);            /* 初始化录音后端: 建目录 + 创建采集任务 (开机调用一次) */
void     recorder_toggle(void);          /* 未录音=开始录音; 录音中=暂停/继续 切换 (异步) */
uint8_t  recorder_save(void);            /* 保存当前录音文件并停止采集, 1=成功 (阻塞, 最长 5s) */
uint8_t  recorder_is_active(void);       /* 1=正在录音 (含暂停状态) */
uint8_t  recorder_is_paused(void);       /* 1=录音已暂停 */
uint32_t recorder_bitrate(void);         /* 当前录音比特率 (bps) */

/* ---- 显示 ---- */
void recoder_msg_show(uint32_t tsec, uint32_t kbps);    /* 显示录音时间(TIME)和比特率(KBPS) */

#endif
