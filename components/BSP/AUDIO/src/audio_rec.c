/**
 * @file    audio_rec.c
 * @brief   麦克风采集实现（I2S RX，双声道交错 -> 单声道 16k 16bit）
 *
 * 从 audio_player.c 搬出来：录音与播放器共用同一路 I2S 控制器，
 * 句柄由播放器统一创建/开关，这里通过 audio_internal.h 只读取句柄。
 */

#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include "audio_internal.h"
#include "audio_rec.h"

static const char *TAG = "audiorec";

/**
 * 录音读一段 PCM，转成单声道 16bit（16 kHz）——给语音识别用。
 *
 * ★ 硬件事实（实测注释见 audio_rec_level）：I2S RX 是**双声道交错**，右声道有 ~1100 的
 *   固定直流偏置（单端输入）。这里对两路各自去直流、选能量大的那一路输出，
 *   避免依赖"哪一路接的是麦克风"。
 *
 * ★★ 录音通道锁定：绝不能每块都重新判"哪路能量大"——
 *   两路能量接近时通道会逐块跳变，音频被切成两段不同信号（甚至相位相反），
 *   ASR 直接听不懂（实测"测试一下"被识别成"来证据一下"）。
 *   正确做法：首次遇到有信号的块时判定一次，之后整段锁定。
 *   -1 = 待判定，0 = 左，1 = 右。
 */
static int s_rec_use_r = -1;

void audio_rec_begin(void)
{
    s_rec_use_r = -1;
}

int audio_rec_read_mono(int16_t *out, int max_samples, int timeout_ms)
{
    i2s_chan_handle_t rx = audio_priv_rx();
    if (rx == NULL || !audio_priv_rec_open() || out == NULL || max_samples <= 0)
    {
        return -1;
    }

    static int16_t raw[1024];          /* 512 帧 × 2 声道 ≈ 32ms @16k */
    size_t got = 0;
    if (i2s_channel_read(rx, raw, sizeof(raw), &got,
                         pdMS_TO_TICKS(timeout_ms)) != ESP_OK)
    {
        return -1;
    }

    int frames = (int)(got / (2 * sizeof(int16_t)));
    if (frames <= 0)
    {
        return 0;
    }

    long sl = 0, sr = 0;
    for (int i = 0; i < frames; i++)
    {
        sl += raw[i * 2];
        sr += raw[i * 2 + 1];
    }
    int dcl = (int)(sl / frames);
    int dcr = (int)(sr / frames);

    long pl = 0, pr = 0;
    for (int i = 0; i < frames; i++)
    {
        int vl = raw[i * 2] - dcl;
        int vr = raw[i * 2 + 1] - dcr;
        pl += (long)vl * vl;
        pr += (long)vr * vr;
    }
    if (s_rec_use_r < 0 && (pl > 4000 || pr > 4000))
    {
        s_rec_use_r = (pr > pl) ? 1 : 0;   /* 有信号才判定，之后整段锁定 */
        ESP_LOGI(TAG, "rec channel locked to %s (L=%ld R=%ld)",
                 s_rec_use_r ? "R" : "L", pl, pr);
    }
    int use_r = (s_rec_use_r == 1);

    int n = (frames < max_samples) ? frames : max_samples;
    for (int i = 0; i < n; i++)
    {
        int v = use_r ? (raw[i * 2 + 1] - dcr) : (raw[i * 2] - dcl);
        if (v < -32768) { v = -32768; }
        if (v > 32767)  { v = 32767; }
        out[i] = (int16_t)v;
    }
    return n;
}

int audio_rec_level(void)
{
    i2s_chan_handle_t rx = audio_priv_rx();
    if (rx == NULL || !audio_priv_rec_open())
    {
        return 0;    /* PLAY 态（放歌中）rec 句柄已关 ⇒ 无电平 */
    }

    static int16_t buf[512];          /* 512 样本 ≈ 16ms @16k（双声道） */
    /* ★ 裸读 I2S RX：ADC 寄存器由播放器侧配好（仅 REC 态有效） */
    size_t got = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got, 5) != ESP_OK || got == 0)
    {
        return 0;
    }

    int cnt = (int)(got / sizeof(int16_t));
    /* ★ 分通道去直流：R 通道（单端输入）有 ~1100 的固定偏置，
     *   不去掉的话电平表基线恒为 16、说话时看不出跳变 */
    long sum_l = 0, sum_r = 0;
    int    n   = 0;
    for (int i = 0; i + 1 < cnt; i += 2)
    {
        sum_l += buf[i];
        sum_r += buf[i + 1];
        n++;
    }
    int dc_l = n > 0 ? (int)(sum_l / n) : 0;
    int dc_r = n > 0 ? (int)(sum_r / n) : 0;
    int peak = 0;
    for (int i = 0; i + 1 < cnt; i += 2)
    {
        int vl = buf[i] - dc_l;
        int vr = buf[i + 1] - dc_r;
        if (vl < 0) vl = -vl;
        if (vr < 0) vr = -vr;
        if (vl > peak) peak = vl;
        if (vr > peak) peak = vr;
    }

    /* 16bit 满量程 32767 → 0~100（显示放大 ×4：环境噪声电平太小，不放大看不见跳动） */
    int lv = peak * 400 / 32768;
    return lv > 100 ? 100 : lv;
}

/** ★ 麦克风诊断：读一帧并打印每声道 DC / min / max（排查"有无数据/直流/毛刺"用） */
void audio_rec_debug_log(void)
{
    i2s_chan_handle_t rx = audio_priv_rx();
    if (rx == NULL || !audio_priv_rec_open())
    {
        ESP_LOGW(TAG, "mic dbg: rec not open");
        return;
    }
    static int16_t buf[512];
    size_t got = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got, 5) != ESP_OK || got == 0)
    {
        ESP_LOGW(TAG, "mic dbg: read fail");
        return;
    }
    int cnt  = (int)(got / sizeof(int16_t));
    long sl = 0, sr = 0;
    int  nl = 0, nr = 0;
    int  minl = 32767, maxl = -32768, minr = 32767, maxr = -32768;
    for (int i = 0; i + 1 < cnt; i += 2)
    {
        sl += buf[i];
        sr += buf[i + 1];
        nl++;
        nr++;
        if (buf[i]     < minl) minl = buf[i];
        if (buf[i]     > maxl) maxl = buf[i];
        if (buf[i + 1] < minr) minr = buf[i + 1];
        if (buf[i + 1] > maxr) maxr = buf[i + 1];
    }
    ESP_LOGW(TAG, "mic dbg: L dc=%ld min=%d max=%d | R dc=%ld min=%d max=%d",
             sl / nl, minl, maxl, sr / nr, minr, maxr);
}
