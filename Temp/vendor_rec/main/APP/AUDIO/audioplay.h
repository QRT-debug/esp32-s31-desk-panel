/**
 ******************************************************************************
 * @file        audioplay.h
 * @version     V1.0
 * @brief       音乐播放器应用层
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __AUDIOPLAY_H
#define __AUDIOPLAY_H

#include "ff.h"
#include "wavplay.h"
#include "exfuns.h"
#include "myi2s.h"
#include "lcd.h"
#include "text.h"
#include "myes8389.h"


typedef struct
{
    uint8_t *rbuf;      /* 文件读取缓冲 */
    uint8_t *tbuf;      /* 播放缓冲(送入 I2S 前的立体声 16bit 数据) */
    FIL *file;

    uint8_t status;     /* bit0: 暂停标志; bit1: 播放开启标志 */
} __audiodev;

extern __audiodev g_audiodev;

void audio_start(void);                                                     /* 置"播放中"标志 */
uint16_t audio_get_tnum(uint8_t *path);                                     /* 统计路径下的 WAV 文件个数 */
void audio_index_show(uint16_t index, uint16_t total);                      /* 显示 曲号/总数 */
void audio_msg_show(uint32_t totsec, uint32_t cursec, uint32_t bitrate);    /* 显示 当前时间/总时间/比特率 */

#endif
