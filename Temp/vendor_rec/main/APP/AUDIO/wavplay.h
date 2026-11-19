/**
 ******************************************************************************
 * @file        wavplay.h
 * @version     V2.0
 * @brief       WAV 解码播放
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __WAVPLAY_H
#define __WAVPLAY_H

#include "audioplay.h"
#include "ff.h"
#include "myes8389.h"
#include "driver/i2s_std.h"
#include "myi2s.h"


#define WAV_TX_BUFSIZE    3072                  /* 播放缓冲大小(16bit立体声) */
#define WAV_RX_BUFSIZE    (WAV_TX_BUFSIZE / 2)  /* 文件读取缓冲大小 */

typedef struct
{
    uint32_t ChunkID;           /* "RIFF" */
    uint32_t ChunkSize;         /* 文件总大小-8 */
    uint32_t Format;            /* "WAVE" */
} ChunkRIFF;

typedef struct
{
    uint32_t ChunkID;           /* "fmt " */
    uint32_t ChunkSize;         /* 不包含ID和Size */
    uint16_t AudioFormat;       /* 0x01=线性PCM, 0x11=IMA ADPCM */
    uint16_t NumOfChannels;     /* 1=单声道, 2=双声道 */
    uint32_t SampleRate;        /* 采样率 */
    uint32_t ByteRate;          /* 字节速率 */
    uint16_t BlockAlign;        /* 块对齐(字节) */
    uint16_t BitsPerSample;     /* 单个采样数据大小 */
} ChunkFMT;

typedef struct
{
    uint32_t ChunkID;           /* "fact" */
    uint32_t ChunkSize;
    uint32_t NumOfSamples;
} ChunkFACT;

typedef struct
{
    uint32_t ChunkID;           /* "data" */
    uint32_t ChunkSize;
} ChunkDATA;

typedef struct
{
    ChunkRIFF riff;             /* riff块 */
    ChunkFMT fmt;               /* fmt块 */
    ChunkDATA data;             /* data块 */
} __WaveHeader;                 /* wav文件头(recorder.c写录音文件用) */

typedef struct
{
    uint16_t audioformat;       /* 0x01=线性PCM */
    uint16_t nchannels;         /* 1或2 */
    uint32_t datasize;          /* WAV数据大小 */

    uint32_t totsec;            /* 整首歌时长(秒) */
    uint32_t cursec;            /* 当前播放时长 */

    uint32_t bitrate;           /* 比特率(位速) */
    uint32_t samplerate;        /* 采样率 */
    uint16_t bps;               /* 16/24bit */

    uint32_t datastart;         /* 数据帧在文件内的偏移 */
} __wavctrl;


/* ---- 解码 ---- */
uint8_t wav_decode_init(uint8_t *fname, __wavctrl *wavx);   /* 解析 WAV 头, 0=成功 */

/* ---- 由 UI 驱动的播放控制 (非阻塞) ---- */
uint8_t  wav_play_start(uint8_t *fname);    /* 开始播放一个 WAV 文件, 0=成功 (其余为错误码) */
uint8_t  wav_play_is_playing(void);         /* 1=正在播放中 */
uint8_t  wav_play_is_done(void);            /* 1=本曲已播完(或被停止) */
void     wav_play_pause(void);              /* 暂停 */
void     wav_play_resume(void);             /* 继续 */
void     wav_play_release(void);            /* 停止播放并释放缓冲 (阻塞, 等待播放任务退出) */
void     wav_play_update_time(void);        /* 刷新当前播放时间 (播放中每秒调用一次) */

/* ---- 当前曲目信息 ---- */
uint32_t wav_play_cursec(void);             /* 当前播放时长(秒) */
uint32_t wav_play_totsec(void);             /* 文件总时长(秒) */
uint32_t wav_play_bitrate(void);            /* 比特率(bps) */

#endif
