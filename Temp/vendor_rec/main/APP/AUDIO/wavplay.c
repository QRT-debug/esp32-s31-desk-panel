/**
 ******************************************************************************
 * @file        wavplay.c
 * @version     V1.0
 * @brief       WAV 解码播放
 *              music 后台任务,负责解码送 I2S, UI 只负责 start/pause/stop 控制
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "wavplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MUSIC_PRIO      4
#define MUSIC_STK_SIZE  5*1024

static TaskHandle_t MUSICTask_Handler = NULL;
static void music(void *pvParameters);

/* ------ 播放状态 ------ */
static __wavctrl s_wavctrl;                          /* 当前文件解码信息 */
static volatile uint32_t s_played = 0;               /* 本曲已播放字节数 (music 写) */
static volatile uint8_t  s_task_exit = 1;            /* music 任务已退出 */
static volatile uint8_t  s_stop_req  = 0;            /* 停止请求, music 看到后退出 */
static uint8_t s_active = 0;                         /* 本曲播放会话有效 */
static int src_factor = 1;                           /* 1=直通, 负数=÷|n| 下采样 */

/* ------- 状态查询 ------- */
/**
 * @brief   查询是否正在播放
 * @param   无
 * @retval  1=正在播放中
 */
uint8_t wav_play_is_playing(void) 
{ 
    return (s_active && !s_task_exit) ? 1 : 0; 
}

/**
 * @brief   查询本曲是否已结束
 * @param   无
 * @retval  1=已播完或被停止
 */
uint8_t wav_play_is_done(void) 
{ 
    return s_task_exit; 
}

/**
 * @brief   查询当前播放时长
 * @param   无
 * @retval  秒数
 */
uint32_t wav_play_cursec(void) 
{ 
    return s_wavctrl.cursec; 
}

/**
 * @brief   查询文件总时长
 * @param   无
 * @retval  秒数
 */
uint32_t wav_play_totsec(void) 
{ 
    return s_wavctrl.totsec; 
}

/**
 * @brief   查询比特率
 * @param   无
 * @retval  比特率 (bps)
 */
uint32_t wav_play_bitrate(void) 
{ 
    return s_wavctrl.bitrate; 
}

/* ------- WAV 头解析 ------- */
/**
 * @brief   解析 WAV 文件头
 * @param   fname  文件全路径
 * @param   wavx   输出: 解码信息
 * @retval  0=成功  1=打开失败  2=非WAV  3=无DATA块  0xFF=内存不足
 */
uint8_t wav_decode_init(uint8_t *fname, __wavctrl *wavx)
{
    FIL *ftemp = (FIL *)malloc(sizeof(FIL));
    uint8_t *buf = malloc(512);
    uint8_t res = 0;

    if (!ftemp || !buf) return 0xFF;
    memset(ftemp, 0, sizeof(FIL));

    if (f_open(ftemp, (TCHAR *)fname, FA_READ) == FR_OK) {
        uint32_t br = 0;
        f_read(ftemp, buf, 512, (UINT *)&br);

        ChunkRIFF *riff = (ChunkRIFF *)buf;
        if (riff->Format == 0x45564157) {                /* "WAVE" */
            ChunkFMT *fmt = (ChunkFMT *)(buf + 12);
            uint32_t data_ofs = 12 + 8 + fmt->ChunkSize;

            /* 可选 FACT chunk */
            ChunkFACT *fact = (ChunkFACT *)(buf + data_ofs);
            if (fact->ChunkID == 0x74636166 || fact->ChunkID == 0x5453494C) {
                data_ofs += 8 + fact->ChunkSize;
            }

            ChunkDATA *data = (ChunkDATA *)(buf + data_ofs);
            if (data->ChunkID == 0x61746164) {           /* "data" */
                wavx->audioformat = fmt->AudioFormat;
                wavx->nchannels    = fmt->NumOfChannels;
                wavx->samplerate   = fmt->SampleRate;
                wavx->bitrate      = fmt->ByteRate * 8;
                wavx->bps         = fmt->BitsPerSample;
                wavx->datasize     = data->ChunkSize;
                wavx->datastart    = data_ofs + 8;
            } else res = 3;
        } else res = 2;
    } else res = 1;

    f_close(ftemp); free(ftemp); free(buf);
    return res;
}

/**
 * @brief   根据文件读写位置刷新当前播放时间 (UI 每秒调用一次即可)
 */
void wav_play_update_time(void)
{
    if (!s_active || s_wavctrl.datasize == 0 || g_audiodev.file == NULL) return;

    s_wavctrl.totsec = s_wavctrl.datasize / (s_wavctrl.bitrate / 8);
    long long fpos = g_audiodev.file->fptr - s_wavctrl.datastart;
    if (fpos < 0) fpos = 0;
    s_wavctrl.cursec = fpos * s_wavctrl.totsec / s_wavctrl.datasize;
}

/* ------- 采样率转换 ------- */
/**
 * @brief   查询采样率转换因子
 * @retval  1=直通  -2=÷2下采样  0=不支持
 */
static int wav_get_src_factor(uint32_t samplerate)
{
    switch (samplerate) {
        case 8000: case 16000: case 44100: case 48000: return 1;
        case 32000: return -2;
        default:    return 0;
    }
}

/**
 * @brief   ÷n 下采样 (每 n 帧取平均, 就地转换)
 */
static uint32_t src_down_n(int16_t *buf, uint32_t nframes, uint32_t n)
{
    uint32_t out = nframes / n;
    for (uint32_t i = 0; i < out; i++) {
        int32_t l = 0, r = 0;
        for (uint32_t j = 0; j < n; j++) {
            l += buf[(i * n + j) * 2];
            r += buf[(i * n + j) * 2 + 1];
        }
        buf[i * 2]     = (int16_t)(l / n);
        buf[i * 2 + 1] = (int16_t)(r / n);
    }
    return out;
}

/**
 * @brief   格式转换 → 统一输出 16bit 立体声, 送 I2S TX
 * @retval  实际送入 I2S 的字节数
 */
static size_t wav_data_write(UINT bytes_read)
{
    int16_t *dst = (int16_t *)g_audiodev.tbuf;
    uint32_t nframes = 0;

    if (s_wavctrl.nchannels == 2 && s_wavctrl.bps == 16) {
        int16_t *src = (int16_t *)g_audiodev.rbuf;
        nframes = bytes_read / 4;
        memcpy(dst, src, nframes * 4);
    } else if (s_wavctrl.nchannels == 1 && s_wavctrl.bps == 16) {
        int16_t *src = (int16_t *)g_audiodev.rbuf;
        nframes = bytes_read / 2;
        for (uint32_t i = 0; i < nframes; i++) { dst[2 * i] = src[i]; dst[2 * i + 1] = src[i]; }
    } else if (s_wavctrl.nchannels == 2 && s_wavctrl.bps == 24) {
        uint8_t *src = g_audiodev.rbuf;
        nframes = bytes_read / 6;
        for (uint32_t i = 0; i < nframes * 2; i++) dst[i] = (int16_t)(src[3 * i + 1] | (src[3 * i + 2] << 8));
    } else if (s_wavctrl.nchannels == 1 && s_wavctrl.bps == 24) {
        uint8_t *src = g_audiodev.rbuf;
        nframes = bytes_read / 3;
        for (uint32_t i = 0; i < nframes; i++) { int16_t s = (int16_t)(src[3*i+1]|(src[3*i+2]<<8)); dst[2*i]=s; dst[2*i+1]=s; }
    }

    if (nframes > 0) {
        if (src_factor < 0) nframes = src_down_n(dst, nframes, (uint32_t)(-src_factor));
        return i2s_tx_write(g_audiodev.tbuf, nframes * 4);
    }
    return 0;
}

/* ------- music 后台任务 ------- */
/**
 * @brief   播放后台任务: f_read → wav_data_write → i2s_tx_write
 *          播完(或被停止)后置 s_task_exit 并自删
 */
static void music(void *pvParameters)
{
    (void)pvParameters;
    memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
    i2s_tx_write(g_audiodev.tbuf, WAV_TX_BUFSIZE);   /* 先填一段静音到 DMA */

    while (s_played < s_wavctrl.datasize) {
        /* --- 暂停: 记住文件位置, 等继续 --- */
        if ((g_audiodev.status & 0x0F) == 0x00) {
            FSIZE_t file_read_pos = f_tell(g_audiodev.file);
            while ((g_audiodev.status & 0x0F) != 0x03) {
                if (s_stop_req)  break;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            if (s_stop_req) break;
            f_lseek(g_audiodev.file, file_read_pos);
        }

        if (s_stop_req)  break;                        /* 停止请求(切歌/退出) */

        UINT bytes_read = 0;
        f_read(g_audiodev.file, g_audiodev.rbuf,
                WAV_RX_BUFSIZE / ((src_factor < 0) ? -src_factor : 1), &bytes_read);
        if (bytes_read == 0) break;

        wav_data_write(bytes_read);
        s_played += bytes_read;
    }

    s_played = 0;
    s_task_exit = 1;
    vTaskDelete(NULL);
}

/* ------- 播放控制 ------- */
/**
 * @brief   释放播放资源: 等待 music 任务退出并释放缓冲
 * @note    若 2s 内任务仍未退出则保留缓冲不释放, 避免任务访问已释放内存
 */
void wav_play_release(void)
{
    if (MUSICTask_Handler != NULL) {
        s_stop_req = 1;
        uint32_t wait = 0;
        while (!s_task_exit && wait < 2000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
        if (!s_task_exit) {
            printf("[WAVPLAY] music task not exit, keep buffers\n");
            MUSICTask_Handler = NULL;
            return;
        }
    }

    free(g_audiodev.file);  g_audiodev.file = NULL;
    free(g_audiodev.rbuf);  g_audiodev.rbuf = NULL;
    free(g_audiodev.tbuf);  g_audiodev.tbuf = NULL;
    MUSICTask_Handler = NULL;
    s_task_exit = 1;                        /* 无播放任务 → 视为已结束 */
    s_active = 0;
}

/**
 * @brief   开始播放一个 WAV 文件 (非阻塞, music 任务在后台解码播放)
 * @param   fname  WAV 文件全路径
 * @retval  0=成功  1=内存失败  2=打开失败  0xFF=格式不支持
 */
uint8_t wav_play_start(uint8_t *fname)
{
    uint8_t res = 0;

    wav_play_release();                     /* 先停掉上一曲并释放资源 */

    s_stop_req  = 0;
    s_task_exit = 0;
    s_played    = 0;

    g_audiodev.file = (FIL *)malloc(sizeof(FIL));
    g_audiodev.rbuf = (uint8_t *)malloc(WAV_RX_BUFSIZE);
    g_audiodev.tbuf = (uint8_t *)malloc(WAV_TX_BUFSIZE);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (!g_audiodev.file || !g_audiodev.rbuf || !g_audiodev.tbuf) {
        printf("[WAVPLAY] malloc fail\n");
        wav_play_release();
        return 1;
    }

    memset(g_audiodev.file, 0, sizeof(FIL));
    memset(g_audiodev.rbuf, 0, WAV_RX_BUFSIZE);
    memset(g_audiodev.tbuf, 0, WAV_TX_BUFSIZE);
    memset(&s_wavctrl, 0, sizeof(__wavctrl));

    printf("[WAVPLAY] %s\n", fname);
    res = wav_decode_init(fname, &s_wavctrl);
    if (res == 0) {
        /* 格式检查: PCM / 16或24bit / 单或双声道 */
        if (s_wavctrl.audioformat != 0x01 || (s_wavctrl.bps != 16 && s_wavctrl.bps != 24)
                || (s_wavctrl.nchannels != 1 && s_wavctrl.nchannels != 2)) {
            printf("unsupported wav format\n");
            res = 0xFF;
        }
    }
    if (res == 0) {
        src_factor = wav_get_src_factor(s_wavctrl.samplerate);
        if (src_factor == 0) { printf("unsupported sr=%ld\n", s_wavctrl.samplerate); res = 0xFF; }
    }
    if (res == 0) {
        uint32_t play_rate = (src_factor < 0) ? (s_wavctrl.samplerate / (-src_factor)) : s_wavctrl.samplerate;
        if (myes8389_set_fs(play_rate, I2S_DATA_BIT_WIDTH_16BIT) != 0) {
            printf("set_fs fail sr=%ld\n", play_rate);
            res = 0xFF;
        }
    }
    if (res == 0) {
        if (f_open(g_audiodev.file, (TCHAR *)fname, FA_READ) != FR_OK) {
            res = 2;
        } else {
            f_lseek(g_audiodev.file, s_wavctrl.datastart);
            audio_start();                                  /* 置播放中标志 */

            if (xTaskCreate(music, "music", MUSIC_STK_SIZE, NULL, MUSIC_PRIO, &MUSICTask_Handler) != pdPASS) {
                printf("[WAVPLAY] create music task fail\n");
                MUSICTask_Handler = NULL;
                s_task_exit = 1;
                res = 1;
            } else {
                s_active = 1;
            }
        }
    }

    if (res != 0) wav_play_release();                       /* 失败: 释放资源 */
    return res;
}

/**
 * @brief   暂停播放
 * @note    置 bit0~3=0, music 任务在下一轮循环进入暂停等待
 */
void wav_play_pause(void)
{
    if (!wav_play_is_playing()) return;
    g_audiodev.status = 0;
}

/**
 * @brief   继续播放
 */
void wav_play_resume(void)
{
    if (!wav_play_is_playing()) return;
    audio_start();                      /* 置回播放中标志 */
}
