/**
 * @file    audio_player.c
 * @brief   ES8389 + I2S STD TX 音乐文件播放实现（PSRAM 整曲缓存版）
 *
 * ★ 配置全部照抄本板已验证的参考工程（korvo1-bringup examples/05）：
 *   STD TX / 48 kHz / 16 bit / stereo / MCLK 不输出 / codec slave / PA 高有效。
 * ★ 音源 = flash vfs FAT 分区（只读挂载 /music）里的 48kHz/16bit WAV 文件。
 * ★★ 总线争用教训（11-16 实测）：flash 和 PSRAM 共用 MSPI，RGB LCD DMA 每秒要
 *   从 PSRAM 拉 ~50MB；播放中每次 fread 都会打断 LCD DMA（屏幕条纹），
 *   而默认 I2S DMA 只有 ~30ms 余量，flash 读一卡就断流（喇叭"嘶啦嘶啦"）。
 *   ⇒ 修法：点曲时把整首 WAV 一次性预载进 PSRAM，播放期间零 flash 访问
 *   （PSRAM 读音频只占 LCD 带宽 0.4%）；I2S DMA 余量加大到 ~80ms。
 *   加载的那几秒仍可能有点条纹（flash 读），属预期，加载完即干净。
 * ★ 起播 30ms 线性淡入防"咔"声；播完或 stop 后自动静音并自删任务。
 */
#include "audio_internal.h"
#include "audio_player.h"
#include "sd_card.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "audio";

#define AUD_I2S_PORT       I2S_NUM_0
#define AUD_I2S_BCLK_GPIO  GPIO_NUM_3
#define AUD_I2S_WS_GPIO    GPIO_NUM_4
#define AUD_I2S_DOUT_GPIO  GPIO_NUM_5
#define AUD_I2S_DIN_GPIO   GPIO_NUM_6          /* codec ASDOUT -> ESP32 DIN（录音） */
#define AUD_PA_GPIO        GPIO_NUM_7          /* NS4150B 使能，高有效（R11 10k 下拉） */

/* ★ 双采样率（11-19 实测定案）：
 *   播放 44.1kHz（MP3 原生，零重采样）；录音 16kHz（ASR 标准采样率）。
 *   ★ ES8389 驱动的 PLL 分频表没有 44.1k 行（44.1k 时填的是 48k 行
 *     05=45/06=A4 ⇒ PLL 锁不住 ⇒ ADC 输出全幅垃圾，实测 min=-32768/max=32767）；
 *     16k 行正确 ⇒ ADC 数据干净。⇒ 录音固定 16k，切换模式时重配 I2S 时钟。 */
#define AUD_RATE_PLAY      44100
#define AUD_RATE_REC       16000
#define AUD_MCLK_MULTIPLE  256

/* ★★ 11-17 00:10 实测结论（重要）：
 *   32bit slot 方案**不可用**——IDF 的 I2S 时钟把每帧算成 128 BCLK，
 *   实测 DIAG RATE ≈ 22.1kHz（标称 44.1k，慢一倍、低八度）。
 *   ⇒ 回到 16bit slot（BCLK = 32fs = 1.4112MHz，LRCK 44.1k 正确），
 *     并把 codec 驱动按 32fs 配分频（见 managed_components es8389.c 的
 *     MCLK_FRE_FACTOR 修改），两者才自洽。 */
#define AUD_I2S_SLOT_32BIT 0

#if AUD_I2S_SLOT_32BIT
#define AUD_SLOT_BITS      I2S_DATA_BIT_WIDTH_32BIT
#else
#define AUD_SLOT_BITS      I2S_DATA_BIT_WIDTH_16BIT
#endif

#define AUD_DMA_DESC_NUM   16                  /* 16×240 帧 ≈ 80ms 余量（默认 6×240=30ms 太薄） */
#define AUD_FRAMES         4096                /* 每次送 DMA 的帧数 ≈ 93ms；同时把 PSRAM 读取
                                                * 频率降低 4 倍（LCD DMA 争用下更从容） */

#define AUD_FILE_CHUNK     4096                /* flash→PSRAM 中转读块（字节） */
#define AUD_MOUNT_POINT    "/music"
#define AUD_CACHE_SLOTS    2                   /* 曲目缓存槽数（两首 ≈ 9.4MB，PSRAM 16MB） */

/* 曲目缓存：WAV data 段整段驻留 PSRAM，播放期间不再碰 flash */
typedef struct
{
    char     name[32];                         /* 文件名（与 s_cur_file 同宽） */
    int16_t *samples;                         /* PSRAM */
    size_t   count;                           /* int16 样本数（data_bytes / 2） */
    int      ch;                              /* 声道数 */
} aud_cache_t;

static esp_codec_dev_handle_t s_codec;     /* 播放（TYPE_OUT）；REC 态复用它写 ADC 寄存器 */
static i2s_chan_handle_t      s_rx;    /* 录音（I2S RX） */
static i2s_chan_handle_t      s_tx;    /* 播放（I2S TX）——重配时钟用 */
static uint32_t               s_cur_i2s_rate;   /* I2S 当前时钟（避免重复重配） */

/* ---- 播放/录音互斥模式机 ----
 * 同一时刻只 open 一个 esp_codec_dev：两个句柄共享 codec_if/data_if，
 * 同时 open 会互踩 codec 的 PLL / 参考电压寄存器（厂商例程也是这个架构）。
 * 默认停在 REC（麦克风常开）：SYSTEM 页电平表、后续 AI 对话从这里取数据；
 * 放歌时切 PLAY，播完/停止自动切回 REC。
 * 播放 44.1k / 录音 16k：切换时重配 I2S 时钟。
 * ★ 录音不走 codec_dev：旧驱动（v1.x，DAC 播放验证过数天）没有 ADC 支持。
 *   ADC = 手动寄存器序列（从新驱动 open(IN) 移植，11-19 实测 16k 干净）+ 裸 I2S RX 读。
 *   11-20 决策：新驱动的 DAC@44.1k 嘶啦声三轮寄存器实验修不动 ⇒ 驱动退场。 */
typedef enum { AUD_MODE_REC = 0, AUD_MODE_PLAY } aud_mode_t;
static volatile aud_mode_t s_aud_mode = AUD_MODE_REC;   /* 意图值；实际以 s_*_open 为准 */
static volatile bool s_play_open;                       /* play 句柄当前是否 open */
static volatile bool s_rec_open;                        /* rec 句柄当前是否 open */
static SemaphoreHandle_t s_aud_mux;
static esp_err_t aud_enter(aud_mode_t target);          /* 定义在 audio_player_init 前 */
static void adc_manual_init(void);        /* REC 态手动初始化 ADC（见实现处注释） */
static volatile bool s_playing;
static volatile int  s_state = 0;              /* audio_player_state_t */
static volatile uint32_t s_pos_ms;             /* 播放位置（毫秒） */
static volatile uint32_t s_dur_ms;             /* 当前曲目总时长（毫秒） */
static volatile int  s_diag  = 0;              /* 诊断模式（0=正常） */
static TaskHandle_t  s_diag_task;              /* 诊断音源任务（数字静音/正弦） */
static volatile int  s_volume = 40;            /* 试听期默认 40（防止异常噪声炸耳）。
                                                * ★★ 不要为了"某段音频听着小"去动这个值：
                                                *  它是**全局播放音量**，音乐会跟着一起响。
                                                *  正确的做法是让那段音频的电平向标准看齐
                                                *  （2026-11-24 我把它改成 70 补偿 SoVITS，
                                                *   结果音乐过响，当天回退）。 */
static TaskHandle_t  s_task;
static char s_cur_file[32] = "NEWSONG.WAV";    /* 当前曲目文件名（UI 可切换） */
static char s_file_path[80];                   /* 完整路径（任务用） */
static aud_cache_t s_cache[AUD_CACHE_SLOTS];

static int16_t s_frame_st[AUD_FRAMES * 2];     /* mono→stereo 展开缓冲（16bit slot 用） */
#if AUD_I2S_SLOT_32BIT
static int32_t s_frame_s32[AUD_FRAMES * 2];    /* 32bit slot 版本（16bit 数据左移 16） */
#endif
static int16_t s_flash_buf[AUD_FILE_CHUNK / 2];   /* flash→PSRAM 中转（内部 RAM） */
static bool    s_mounted;

/* ---------------- WAV 头解析 ---------------- */

/* 解析 WAV 头：返回采样率/声道数/data 字节数；失败返回 false */
static bool parse_wav_header(FILE *f, int *sr_out, int *ch_out, uint32_t *data_bytes)
{
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
    {
        return false;
    }
    int sr = 0, ch = 0;
    for (int guard = 0; guard < 16; guard++)
    {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, f) != 8)
        {
            return false;
        }
        uint32_t sz = chdr[4] | (chdr[5] << 8) | ((uint32_t)chdr[6] << 16) | ((uint32_t)chdr[7] << 24);
        if (memcmp(chdr, "fmt ", 4) == 0)
        {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, f) != 16)
            {
                return false;
            }
            if (sz > 16 && fseek(f, sz - 16, SEEK_CUR) != 0)
            {
                return false;
            }
            uint16_t tag = fmt[0] | (fmt[1] << 8);
            ch          = fmt[2] | (fmt[3] << 8);
            sr          = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            uint16_t bits = fmt[14] | (fmt[15] << 8);
            if (tag != 1 || bits != 16 || (ch != 1 && ch != 2))
            {
                ESP_LOGE(TAG, "wav: unsupported fmt tag=%d ch=%d bits=%d", tag, ch, bits);
                return false;
            }
        }
        else if (memcmp(chdr, "data", 4) == 0)
        {
            *sr_out = sr;
            *ch_out = ch;
            *data_bytes = sz;
            return sr == AUD_RATE_PLAY;
        }
        else if (fseek(f, sz + (sz & 1), SEEK_CUR) != 0)   /* 跳过未知块（含奇数填充） */
        {
            return false;
        }
    }
    return false;
}

/* ---------------- 播放任务 ---------------- */

/* 整曲预载：flash → PSRAM。返回缓存槽；失败返回 NULL。 */
static aud_cache_t *load_to_psram(void)
{
    aud_cache_t *slot = &s_cache[0];
    bool found_free = false;

    /* 找空位；没有空位就淘汰"不是当前文件"的那个（本板只有两首，够用） */
    for (int i = 0; i < AUD_CACHE_SLOTS; i++)
    {
        if (s_cache[i].samples == NULL)
        {
            slot       = &s_cache[i];
            found_free = true;
            break;
        }
        if (strcmp(s_cache[i].name, s_cur_file) != 0)
        {
            slot = &s_cache[i];
        }
    }
    if (slot->samples != NULL)               /* 淘汰旧缓存 */
    {
        ESP_LOGI(TAG, "cache evict %s", slot->name);
        free(slot->samples);
        slot->samples = NULL;
    }
    (void)found_free;

    FILE *f = fopen(s_file_path, "rb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "open %s failed", s_file_path);
        return NULL;
    }

    int sr = 0, ch = 0;
    uint32_t data_bytes = 0;
    if (!parse_wav_header(f, &sr, &ch, &data_bytes) || data_bytes == 0)
    {
        ESP_LOGE(TAG, "bad wav header (need 44.1kHz/16bit)");
        fclose(f);
        return NULL;
    }

    int16_t *buf = heap_caps_malloc(data_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "PSRAM alloc %u bytes failed", data_bytes);
        fclose(f);
        return NULL;
    }

    const int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    while (got < data_bytes)
    {
        if (!s_playing)
        {
            break;                            /* stop 触发：中止加载 */
        }
        size_t want = data_bytes - got;
        if (want > AUD_FILE_CHUNK)
        {
            want = AUD_FILE_CHUNK;
        }
        size_t n = fread((uint8_t *)buf + got, 1, want, f);
        if (n == 0)
        {
            break;
        }
        got += n;
    }
    fclose(f);

    if (got != data_bytes || !s_playing)
    {
        ESP_LOGW(TAG, "load aborted (%u/%u bytes)", (unsigned)got, (unsigned)data_bytes);
        free(buf);
        return NULL;
    }

    snprintf(slot->name, sizeof(slot->name), "%s", s_cur_file);
    slot->samples = buf;
    slot->count   = data_bytes / sizeof(int16_t);
    slot->ch      = ch;
    ESP_LOGI(TAG, "cache load %s: %u B in %d ms (PSRAM hit next time)",
             slot->name, (unsigned)data_bytes, (int)((esp_timer_get_time() - t0) / 1000));
    return slot;
}

static void file_task(void *arg)
{
    (void)arg;

    /* 1) 缓存命中？未命中则整曲预载 PSRAM（期间 UI 显示 loading） */
    aud_cache_t *c = NULL;
    for (int i = 0; i < AUD_CACHE_SLOTS; i++)
    {
        if (s_cache[i].samples != NULL && strcmp(s_cache[i].name, s_cur_file) == 0)
        {
            c = &s_cache[i];
            break;
        }
    }
    if (c == NULL)
    {
        s_state = AUD_STATE_LOADING;
        c = load_to_psram();
        if (c == NULL)
        {
            goto fail;
        }
    }

    /* 2) 从 PSRAM 播放，全程零 flash 访问 */
    s_state = AUD_STATE_PLAYING;
    /* 互斥：切到 PLAY（关闭 rec 句柄、PA 开） */
    if (aud_enter(AUD_MODE_PLAY) != ESP_OK)
    {
        goto fail;
    }
#if AUD_I2S_SLOT_32BIT
    memset(s_frame_s32, 0, sizeof(s_frame_s32));
    esp_codec_dev_write(s_codec, s_frame_s32, sizeof(s_frame_s32));   /* 起播静音块 */
#else
    memset(s_frame_st, 0, sizeof(s_frame_st));
    esp_codec_dev_write(s_codec, s_frame_st, sizeof(s_frame_st));     /* 起播静音块 */
#endif
    esp_codec_dev_set_out_mute(s_codec, false);

    ESP_LOGI(TAG, "music start: %s (%u samples, %dch, from PSRAM, slot=%s)",
             s_cur_file, (unsigned)c->count, c->ch,
             AUD_I2S_SLOT_32BIT ? "32bit/BCLK 64fs" : "16bit/BCLK 32fs");

    const int fade_in = AUD_RATE_PLAY / 1000 * 30;   /* 30ms 淡入 */
    size_t played = 0;
    /* ★ 播放时长实测：wall clock vs 理论时长，比值 = 实际采样率 / 标称采样率。
     *   用于验证 I2S 44.1k 是否准确（用户反馈"比电脑慢"）。 */
    const int64_t t_play_start = esp_timer_get_time();

    while (s_playing && played < c->count)
    {
        size_t n = c->count - played;
        if (n > AUD_FRAMES)
        {
            n = AUD_FRAMES;
        }
        if (c->ch == 1)
        {
            for (size_t i = 0; i < n; i++)
            {
                int32_t v = c->samples[played + i];
                if (played + i < (size_t)fade_in)
                {
                    /* 线性淡入防爆音 */
                    v = (v * (int32_t)(played + i)) / fade_in;
                }
#if AUD_I2S_SLOT_32BIT
                /* 16bit 样本左移 16：占据 32bit slot 的高 16 位（标准 I2S MSB 对齐） */
                int32_t v32 = v << 16;
                s_frame_s32[2 * i]     = v32;
                s_frame_s32[2 * i + 1] = v32;
#else
                s_frame_st[2 * i]     = (int16_t)v;
                s_frame_st[2 * i + 1] = (int16_t)v;
#endif
            }
#if AUD_I2S_SLOT_32BIT
            esp_codec_dev_write(s_codec, s_frame_s32, (size_t)(n * 2 * sizeof(int32_t)));
#else
            esp_codec_dev_write(s_codec, s_frame_st, (size_t)(n * 2 * sizeof(int16_t)));
#endif
        }
        else
        {
            /* 立体声源目前用不到（曲目都是 mono），保留 16bit 直写 */
            esp_codec_dev_write(s_codec, &c->samples[played], (size_t)(n * sizeof(int16_t)));
        }
        played += n;
    }

    /* 收尾：静音块 + mute */
#if AUD_I2S_SLOT_32BIT
    memset(s_frame_s32, 0, sizeof(s_frame_s32));
    esp_codec_dev_write(s_codec, s_frame_s32, sizeof(s_frame_s32));
    esp_codec_dev_write(s_codec, s_frame_s32, sizeof(s_frame_s32));
#else
    memset(s_frame_st, 0, sizeof(s_frame_st));
    esp_codec_dev_write(s_codec, s_frame_st, sizeof(s_frame_st));
    esp_codec_dev_write(s_codec, s_frame_st, sizeof(s_frame_st));
#endif
    esp_codec_dev_set_out_mute(s_codec, true);

    s_playing = false;
    s_state   = AUD_STATE_IDLE;
    s_task    = NULL;
    aud_enter(AUD_MODE_REC);          /* 播完回 REC：麦克风恢复常开 */
    {
        const float wall_s = (float)(esp_timer_get_time() - t_play_start) / 1e6f;
        const float theory = (float)played / (float)AUD_RATE_PLAY;
        const float ratio  = (wall_s > 0.1f) ? (theory / wall_s) : 0.0f;
        ESP_LOGI(TAG, "music task stop (%u samples) wall=%.2fs theory=%.2fs "
                      "=> real sample rate %.0f Hz (nominal %d)",
                 (unsigned)played, wall_s, theory, ratio * AUD_RATE_PLAY, AUD_RATE_PLAY);
    }
    vTaskDelete(NULL);
    return;

fail:
    s_playing = false;
    s_state   = AUD_STATE_IDLE;
    s_task    = NULL;
    aud_enter(AUD_MODE_REC);
    vTaskDelete(NULL);
}

/* ---------------- ES8389 时钟表分析（暂缓，勿直接启用） ----------------
 * ★ 现象：esp_codec_dev 的 es8389 驱动在"无 MCLK"场景仍按 64fs
 *   （48k×16×4 = 3.072MHz）查分频表，而本板 codec 时钟源是 BCLK
 *   = 48k×16×2 = 1.536MHz（32fs）；原厂 Linux 驱动在 SCLK 模式下按
 *   sysclk = 声道×位宽×采样率 = 32fs 处理，表行 {32,1536000,48000}。
 *   两行表的差异在 0x05/0x06/0x07/0x09（0x45/A4/D0/D1 vs 0x51/04/C0/C1）。
 *
 * ★★ 但 11-16 23:19 实测：仅在 open 后覆盖这 4 个寄存器 ⇒ 系统黑屏 +
 *   codec 输出巨大噪声。原因推断：64fs 分支还带着 0x23 bit7 / 0xF0 bit3
 *   的 "internal reference (ADCL+DACR)" 设置，与 32fs 分频不自洽。
 *   ⇒ 要改必须整体切 32fs（含 no_dac_ref 的参考位），不能只改时钟。
 *   TODO: 下次带用户在场做 A/B：no_dac_ref=true + 32fs，失败立即回退。
 * 值：0x05=0x45, 0x06=0xA4, 0x07=0xD0, 0x09=0xD1（esp 表 32fs/48k 行）
 */

/* ---------------- 对外接口 ---------------- */

/* ★ IO6 悬空检测：模组焊盘在底面，万用表探不到，只能靠上拉/下拉判断
 *   - up=1 & down=0 ⇒ 悬空（走线断 / 0Ω 没贴）⇒ 硬件问题
 *   - up=0 & down=0 ⇒ codec 强拉低（连接正常，输出一直是 0）⇒ ADC 问题
 *   必须在 I2S 占用 IO6 之前调用（本函数在 audio_player_init 开头执行）。 */
static void io6_probe(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << 6,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(30));
    int up = gpio_get_level(GPIO_NUM_6);

    gpio_set_pull_mode(GPIO_NUM_6, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(30));
    int down = gpio_get_level(GPIO_NUM_6);

    const char *verdict = (up == 1 && down == 0) ? "FLOATING -> 走线断/0R 没贴（硬件）"
                        : (up == 0 && down == 0) ? "driven LOW -> codec 在输出（连接正常，问题在 ADC）"
                        : (up == 1 && down == 1) ? "driven HIGH -> 异常（codec 可能短到 3V3）"
                        : "unknown";
    ESP_LOGW(TAG, "IO6 probe: pullup=%d pulldown=%d  => %s", up, down, verdict);

    /* 恢复：交给 I2S 接管前先设成无上下拉 */
    gpio_set_pull_mode(GPIO_NUM_6, GPIO_FLOATING);
}



/* ---- PA（NS4150B 使能）应用层自管 ----
 * 录音态必须关功放：NS4150C 的底噪会经空气 + 电源两条路灌进麦克风。 */
static void pa_set(bool on)
{
    gpio_set_level(AUD_PA_GPIO, on ? 1 : 0);
}

/* ---- I2S 时钟重配（模式切换时调用；句柄都已 close，通道空闲） ---- */
static esp_err_t i2s_set_rate(uint32_t rate)
{
    if (s_cur_i2s_rate == rate)
    {
        return ESP_OK;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    /* ★ 11-20 01:20 终案：REC 16k = DEFAULT（160M÷4.096M=39.0625，小数 1/16 可精确
     *   表示 → 零抖动）；PLAY 44.1k = APLL（22.5792M 精确）。REC 态不碰 APLL →
     *   APLL 只被 PLAY 持有一种频率，永无互抢（IDF reconfig 不释放 APLL，
     *   01:15 版 REC/PLAY 分时切 APLL 实测 enter PLAY failed）。 */
    clk.clk_src       = (rate == AUD_RATE_PLAY) ? I2S_CLK_SRC_APLL : I2S_CLK_SRC_DEFAULT;
    /* ★ disable 容忍"已禁用"：esp_codec_dev 的 close 内部会把通道关掉
     *   （data_if 共享，enable(IN,false) 会连 duplex 伴通道一起关），
     *   此刻通道极可能已经 disabled，裸 disable 会报 INVALID_STATE。
     *   真正致命的错误会在 reconfig/enable 处暴露。 */
    i2s_channel_disable(s_tx);
    i2s_channel_disable(s_rx);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx, &clk), TAG, "tx reclk");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_rx, &clk), TAG, "rx reclk");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
    s_cur_i2s_rate = rate;
    ESP_LOGI(TAG, "I2S clock -> %lu Hz (APLL)", (unsigned long)rate);
    return ESP_OK;
}

/* ---- 手动 ADC 初始化（REC 态专用） ----
 * 序列逐条移植自新驱动 es8389_open(IN)（11-19 实测 16k 干净），外加当晚手动补丁：
 *   0x64=8F 模拟上电 / 0x24,25 HPF / 0x62 VSEL / 0x01 三态清除 / PGA 30dB(0x72,73=1C)。
 * 寄存器写走 s_codec 的 set_reg 通道——旧框架 write_reg 不要求句柄 open（已核实源码）。
 * ★ 这套序列与播放（旧驱动 DAC 行）互不干扰：切模式时各自整体重写自己那半边。 */
static void adc_manual_init(void)
{
    static const uint8_t seq[][2] = {
        {0xF3, 0x00}, {0x00, 0x7E}, {0xF3, 0x38},              /* 隔离 + 复位 */
        {0x24, 0x64}, {0x25, 0x04},
        {0x45, 0x03},
        {0x60, 0x2A}, {0x61, 0xC9}, {0x62, 0x4F}, {0x63, 0x06},
        {0x6B, 0x00}, {0x6D, 0x16}, {0x6E, 0xAA}, {0x6F, 0x66}, {0x70, 0x99},
        {0x23, 0x00},
        {0x72, 0x1C}, {0x73, 0x1C},                            /* PGA: 差分输入 + 30dB 档 */
        {0x10, 0xC4},
        {0x01, 0x08},
        {0xF1, 0x00},
        {0x12, 0x01}, {0x13, 0x01}, {0x14, 0x01}, {0x15, 0x01},
        {0x16, 0x35}, {0x17, 0x09}, {0x18, 0x91}, {0x19, 0x28},
        {0x1A, 0x01}, {0x1B, 0x01}, {0x1C, 0x11},
        {0x2A, 0x00},
        {0x20, 0x60},                                          /* ADC: 16bit I2S 立体声 */
        {0x40, 0x60},                                          /* DAC 格式（回 PLAY 时旧驱动重写） */
        {0x02, 0x00},
        /* ★★★ 11-23 17:30 修正：原来照抄厂商 ratio=256 行（假设 MCLK=4.096MHz），
         *   但本板 MCLK 脚未接（R56 标 NC），codec 只有 BCLK = 32×16k = **512kHz**
         *   当主时钟 —— 分频比差 8 倍！！
         *
         *   后果（实测）：ADC 数字滤波器带宽被设成 16k/8 = 2kHz，截止约 900Hz
         *     ⇒ 录到的音频 83% 能量压在 600Hz 以下、辅音高频全丢，
         *       听起来"像被子蒙着嘴"，ASR 只能瞎猜（"测试一下"→"呵呵,遇到"）。
         *
         *   修法：改用 **ratio=32** 那行（coeff_div[0] = {32,256000,8000}）的分频+滤波值。
         *     512kHz ÷ 32 = 16kHz ✔ 与 I2S LRCK 一致，滤波器也回到 16k 带宽。
         *   原值（ratio=256，宽带错误）保留在下方注释，便于回退：
         *     0x05=41 0x06=04 0x07=C0 0x08=01 0x09=D1 0x0A=90
         *     0x0C=01 0x0D=00(divider 256)  0x43=00  0x16=12 0x18=31 0x19=0E */
        {0x04, 0x00}, {0x05, 0x57}, {0x06, 0x84}, {0x07, 0xD0},
        {0x08, 0x03}, {0x09, 0xC1}, {0x0A, 0xB0},
        {0x0B, 0x04}, {0x0C, 0x00}, {0x0D, 0x20},              /* MCLK divider 256 -> 32 */
        {0x0F, 0x00}, {0x11, 0x00},
        {0x21, 0x1F}, {0x22, 0x7F}, {0x2F, 0xC0},
        {0x30, 0xC0}, {0x31, 0x00},
        {0x16, 0x09}, {0x17, 0x09}, {0x18, 0x19}, {0x19, 0x07},
        {0x44, 0x00}, {0x41, 0xFF}, {0x42, 0x7F}, {0x43, 0x01},
        {0x49, 0x0F}, {0x4C, 0xC0},
        {0x00, 0x00}, {0x03, 0xC3}, {0x00, 0x01},              /* 解复位 + 时钟开 */
        {0x4D, 0x00},
        {0x26, 0xBF}, {0x27, 0xBF}, {0x28, 0xBF},
        {0x46, 0xBF}, {0x47, 0xBF}, {0x48, 0xBE},
        /* ★ 厂商 set_bias_on 全套（11-20 对照厂商例程 es8389.c 277-292 行，录音实测有声）：
         *   0x69|=0x20（模拟偏置激活）→ 0x64=0x8F → 0x61=0xD9 → 0x10=E4（C4→E4，偏置使能 bit5）。
         *   0xF0=0x12 为 coeff 分频表每行标配（CHIP_MISC，此前整个序列漏写）。 */
        {0x69, 0x20},
        {0x64, 0x8F},                                          /* ★ ADC/PGA 模拟上电 */
        {0x61, 0xD9},                                          /* bias_on 顺带置位（干净态快照值） */
        {0x10, 0xE4},                                          /* ★ bias_on: C4→E4（模拟偏置使能） */
        {0xF0, 0x12},                                          /* ★ CHIP_MISC（coeff 表标配值） */
        {0x24, 0x6A}, {0x25, 0x0A},                            /* HPF */
        {0x62, 0x8C},                                          /* VSEL 恢复默认 */
        {0x01, 0x08},                                          /* ★ 厂商固定 bit3=1；三态位 bit7/6/5 本来就 0（11-19 的 0x00 补丁连 bit3 一起清了，错） */
        {0x02, 0x40},                                          /* 内部时钟源 = BCLK（无 MCLK，厂商 no_mclk 同款） */
    };
    for (size_t k = 0; k < sizeof(seq) / sizeof(seq[0]); k++)
    {
        esp_codec_dev_write_reg(s_codec, seq[k][0], seq[k][1]);
    }
}

/* ---- 模式机核心：切换 播放/录音 句柄（互斥） ----
 * 先 close 在用的 → 重配 I2S 时钟 → open 目标；PA 随模式切换。 */
static esp_err_t aud_enter(aud_mode_t target)
{
    if (s_aud_mux == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_aud_mux, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if ((target == AUD_MODE_PLAY && s_play_open) ||
        (target == AUD_MODE_REC  && s_rec_open))
    {
        xSemaphoreGive(s_aud_mux);
        return ESP_OK;                    /* 已在目标态 */
    }

    /* 1) 关掉在用的句柄 */
    if (s_play_open)
    {
        esp_codec_dev_set_out_mute(s_codec, true);
        esp_codec_dev_close(s_codec);
        pa_set(false);                    /* ★ 先关功放再开麦克风 */
        s_play_open = false;
    }
    if (s_rec_open)
    {
        /* REC 态没有 codec_dev 句柄（裸 I2S + 手动 ADC）——只需复位标志。
         * I2S 通道时钟由 i2s_set_rate 统一重配。 */
        s_rec_open = false;
    }

    /* 2) 重配 I2S 时钟 → 打开目标句柄（播放 44.1k / 录音 16k） */
    uint32_t rate = (target == AUD_MODE_PLAY) ? AUD_RATE_PLAY : AUD_RATE_REC;
    err = i2s_set_rate(rate);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_aud_mux);
        return err;
    }
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = rate,
        .mclk_multiple = AUD_MCLK_MULTIPLE,
    };

    if (target == AUD_MODE_PLAY)
    {
        err = esp_codec_dev_open(s_codec, &sample);
        if (err == ESP_OK)
        {
            esp_codec_dev_set_out_vol(s_codec, s_volume);
            pa_set(true);
            /* ★ 2026-11-24：等功放稳定再喂数据。
             *   原来 pa_set(true) 后立刻返回、马上开始推音频，NS4150C 从上电到
             *   增益稳定要一两百毫秒，结果**开头的字被吞掉或声音很小**
             *   （语音对话的第一句最明显，用户实测"第一个字没声音"）。
             *   而 TTS（edge / SoVITS）输出的开头本来就有 100~200ms 静音，
             *   这段等待正好被静音覆盖，用户听不出额外延迟。 */
            vTaskDelay(pdMS_TO_TICKS(200));
            s_play_open = true;
            s_aud_mode  = AUD_MODE_PLAY;
            ESP_LOGI(TAG, "aud mode -> PLAY (PA on, vol %d)", s_volume);
        }
        else
        {
            ESP_LOGE(TAG, "open play dev failed: %s", esp_err_to_name(err));
        }
    }
    else
    {
        /* ★ REC 不经 codec_dev：手动初始化 ADC（序列移植自新驱动 open(IN)，
         *   11-19 实测 16k 干净；旧驱动无 ADC 支持）。寄存器写在关闭态也有效
         *   （旧框架 write_reg 只要求 codec_if->set_reg 存在）。 */
        adc_manual_init();
        pa_set(false);
        s_rec_open = true;
        s_aud_mode = AUD_MODE_REC;
        ESP_LOGI(TAG, "aud mode -> REC (PA off, mic hot, manual ADC)");
    }

    xSemaphoreGive(s_aud_mux);
    return err;
}

esp_err_t audio_player_init(void)
{
    if (s_codec != NULL)
    {
        return ESP_OK;
    }

    s_aud_mux = xSemaphoreCreateMutex();

    /* PA：应用层自管（pa_cfg 里 pa_pin=-1），初始关 */
    gpio_config_t pa_io = {
        .pin_bit_mask = 1ULL << AUD_PA_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* R11 10k 已下拉，双保险 */
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_io);
    gpio_set_level(AUD_PA_GPIO, 0);

    /* I2S STD TX：MCLK 不用（codec slave，靠 BCLK 派生时钟） */
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear   = true;
    chan_cfg.dma_desc_num = AUD_DMA_DESC_NUM;         /* ★ 加大 DMA 余量 30ms→80ms */
    /* ★ 同时申请 RX：全双工共享 BCLK/WS，codec 的 ASDOUT 从 IO6 进来 */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &s_rx), TAG, "i2s_new_channel failed");
    s_tx           = tx;
    /* ★ 11-20 00:40 实测：init 若按 44.1k+APLL 创建，APLL 被占死在 22.5792MHz，
     *   开机 aud_enter(REC) 的 16k 请求 8.192MHz 静默失败（i2s_common 警告"APLL occupied"）
     *   ⇒ ES8389 ADC 时钟异常 ⇒ mic 全 0。开机默认 REC ⇒ 通道直接按 16k+APLL 创建，
     *   i2s_set_rate(16000) 短路 return，APLL 保持干净；44.1k 播放由 i2s_set_rate 换 DEFAULT 源。 */
    s_cur_i2s_rate = AUD_RATE_REC;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUD_RATE_REC),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(AUD_SLOT_BITS, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUD_I2S_BCLK_GPIO,
            .ws   = AUD_I2S_WS_GPIO,
            .dout = AUD_I2S_DOUT_GPIO,
            .din  = AUD_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple   = I2S_MCLK_MULTIPLE_256;
    /* ★ 开机按 REC 16k + DEFAULT 源创建（160M÷4.096M=39.0625 精确，零抖动）。
     *   APLL 只留给 PLAY 44.1k（切换时 APLL 从未被占 → 请求必成功）。 */
    std_cfg.clk_cfg.clk_src         = I2S_CLK_SRC_DEFAULT;
    std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width       = AUD_SLOT_BITS;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s init std failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s enable failed");

    /* RX 用同一套 std 配置（全双工共享时钟/位宽） */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx enable failed");

    /* ES8389：旧 esp_codec_dev v1.x（本地组件）——DAC 播放验证过数天。
     * ★ 11-20 决策：新驱动 DAC@44.1k 嘶啦声三轮寄存器实验修不动 ⇒ 驱动退场，
     *   麦克风改走"裸 I2S RX + 手动 ADC 寄存器序列"（见 adc_manual_init）。 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_BUS_PORT,               /* 旧框架的 i2c cfg 带 port 字段 */
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL)
    {
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = AUD_I2S_PORT,
        .rx_handle = NULL,          /* 麦克风走裸 I2S RX（i2s_channel_read），不经过 codec_dev */
        .tx_handle = tx,
        /* ★ 11-20 01:25 必传：不传（=0）时旧驱动 data_if open(OUT) 的 reconfig 会把
         *   TX 时钟覆盖回 DEFAULT（jitter 91%）——i2s_set_rate 配的 APLL 白配。
         *   传 APLL 后 reconfig 保持 APLL，与 i2s_set_rate 的 PLAY 配置一致。 */
        .clk_src = (int)I2S_CLK_SRC_APLL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (data_if == NULL)
    {
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL)
    {
        return ESP_FAIL;
    }

    es8389_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,   /* 旧驱动只管 DAC（播放） */
        .pa_pin = -1,           /* PA 应用层自管（模式机统一控制 IO7） */
        .master_mode = false,   /* codec 从模式 */
        .use_mclk = false,      /* 无 MCLK（R56 NC），时钟从 BCLK 派生 */
        .digital_mic = false,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
    if (codec_if == NULL)
    {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == NULL)
    {
        return ESP_FAIL;
    }

    /* 模式机启动：默认进 REC（麦克风常开 = 电平表 / 后续 AI 对话的数据源）。
     * 寄存器补丁（0x01 三态清除、0x64 ADC 上电等）在 aud_enter(REC) 里做。 */
    esp_err_t err = aud_enter(AUD_MODE_REC);
    ESP_RETURN_ON_ERROR(err, TAG, "enter REC mode failed");

    {
        int v = 0;
        esp_codec_dev_read_reg(s_codec, 0x01, &v);
        ESP_LOGI(TAG, "MISC ctrl: 0x01 = 0x%02X (want 0x00: TRI_ADCDAT/LRCK/BCLK normal)", v);
    }

    /* 寄存器快照（诊断用，只读，失败不影响主流程）：
     * 0x05/06/07/09 若为 51/04/C0/C1 ⇒ 驱动按 64fs 分了频（配合 32bit slot 的 BCLK 3.072MHz） */
    {
        static const uint8_t kr[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                     0x0A, 0x0F, 0x10, 0x11, 0x20, 0x23, 0x26, 0x69, 0xF0,
                                     0x40, 0x41, 0x42, 0x43, 0x46, 0x47, 0x48, 0x4C, 0x4D,
                                     0x61, 0x62, 0x63, 0x6D, 0x6F, 0x72, 0x73, 0xF0, 0xF3};
        char line[160];
        int  pos = 0;
        for (size_t i = 0; i < sizeof(kr) / sizeof(kr[0]) && pos < (int)sizeof(line) - 8; i++)
        {
            int v = -1;
            esp_codec_dev_read_reg(s_codec, kr[i], &v);
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x=%02x ", kr[i], v & 0xFF);
        }
        ESP_LOGI(TAG, "es8389 regs: %s", line);
    }

    /* ★ 11-17 00:50 实验：对齐原厂 Linux 驱动的参考位配置（怀疑嘶嘶声源）
     *   esp 驱动 open 写 0xF0=0x1A（bit3 = "internal reference (ADCL + DACR)"）
     *   且 0x23 bit7=1；原厂 Linux 驱动初始化写 0xF0=0x13（bit3=0、bit0=1），不动 0x23。
     *   纯放音不需要 ADC 参与 DAC 参考 ⇒ 按原厂值覆盖，观察嘶嘶是否消失。
     *   （不涉及时钟/音量/位深；若声音异常，删掉本段即可回退） */
    {
        int v23 = 0, vf0 = 0;
        int w23 = 0x00, wf0 = 0x13;
        esp_codec_dev_write_reg(s_codec, 0x23, w23);
        esp_codec_dev_write_reg(s_codec, 0xF0, wf0);
        esp_codec_dev_read_reg(s_codec, 0x23, &v23);
        esp_codec_dev_read_reg(s_codec, 0xF0, &vf0);
        ESP_LOGI(TAG, "ref fix (linux values): 0x23=%02x 0xF0=%02x (want 00/13)", v23, vf0);
    }

    ESP_LOGI(TAG, "ES8389 ready: mode=REC (mic hot)  PA IO%d app-managed", AUD_PA_GPIO);
    return ESP_OK;
}

audio_player_state_t audio_player_state(void)
{
    return (audio_player_state_t)s_state;
}

bool audio_player_is_playing(void)
{
    /* ★★ 2026-11-24：LOADING 也必须算"忙"。
     *   sd_play_task 会先把整个 WAV 读进 PSRAM（200~400KB，SD 卡上要 1~3 秒），
     *   这段期间 s_state 是 AUD_STATE_LOADING。只看 PLAYING 的话，调用方会以为
     *   "还没开始播"——实测 voice.c 的"等开始"循环等满 2s 超时后直接 led_idle()，
     *   而那时音频才刚要开始，绿灯就提前灭了。 */
    return s_state == AUD_STATE_PLAYING || s_state == AUD_STATE_LOADING;
}

esp_err_t audio_player_stop(void)
{
    if (s_codec == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_playing = false;      /* 任务自己淡出 → mute → 自删 */
    return ESP_OK;
}

esp_err_t audio_player_mount(void)
{
    if (s_mounted)
    {
        return ESP_OK;
    }
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,   /* 音乐分区烧写失败绝不能在板内格式化 */
        .allocation_unit_size  = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_ro(AUD_MOUNT_POINT, "vfs", &cfg);
    if (err != ESP_OK)
    {
        /* 音乐分区(/music)是历史遗留的备用源：现在音频只走 SD 卡（sd_card_scan_wav），
         * 分区表调整后它本来就不再有有效 FAT，挂载失败属正常，降级为 I 避免误导。 */
        ESP_LOGI(TAG, "music partition unused (SD-only): %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "music partition mounted at %s", AUD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t audio_player_play_file(const char *filename)
{
    if (s_codec == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_mounted && audio_player_mount() != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_task != NULL)
    {
        s_playing = false;    /* 先停当前任务，等它退干净 */
        int wait = 0;
        while (s_task != NULL && wait++ < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    snprintf(s_file_path, sizeof(s_file_path), "%s/%s", AUD_MOUNT_POINT, filename);
    snprintf(s_cur_file, sizeof(s_cur_file), "%s", filename);
    if (s_diag != 0)          /* 播放前复位诊断态，避免"静音播歌"的误会 */
    {
        audio_player_diag_set(0);
    }
    s_playing = true;
    s_state   = AUD_STATE_LOADING;
    BaseType_t ok = xTaskCreatePinnedToCore(file_task, "music", 6144, NULL, 6, &s_task, 0);
    if (ok != pdPASS)
    {
        s_state   = AUD_STATE_IDLE;
        s_playing = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** 播放当前选中的曲目（等价于 audio_player_play_file(当前文件名)） */
esp_err_t audio_player_play(void)
{
    return audio_player_play_file(s_cur_file);
}

const char *audio_player_current_file(void)
{
    return s_cur_file;
}

/* ★ 诊断音源任务：4 = 数字静音（数据全 0），5 = 1kHz 正弦（-20dBFS，数字生成绝对干净）
 *   两者都走与音乐完全相同的通路（DAC unmute、PA 开、同一 I2S 格式），
 *   用于区分"数据里带的噪声" vs "通路自噪 / 信号相关失真"。 */
static void diag_source_task(void *arg)
{
    (void)arg;

    static int16_t s_sin_tab[256];
    for (int i = 0; i < 256; i++)
    {
        s_sin_tab[i] = (int16_t)(3277.0f * sinf(6.28318530718f * (float)i / 256.0f));
    }

    gpio_set_level(AUD_PA_GPIO, 1);
    esp_codec_dev_set_out_mute(s_codec, false);

    uint32_t ph1k = 0;
    const uint32_t step1k = (uint32_t)((1000ULL << 32) / AUD_RATE_PLAY);
    uint32_t ph8k = 0;
    const uint32_t step8k = (uint32_t)((8000ULL << 32) / AUD_RATE_PLAY);

    /* 模式 7（RAM MUSIC）：把缓存里前 1 秒音乐搬到内部 RAM，循环播放，
     * 用于区分"PSRAM 读取路径" vs "内部 RAM 播放路径" */
    static int16_t *s_ram_music;
    static size_t   s_ram_n;
    size_t ram_idx = 0;

    uint32_t it = 0;
    int64_t  t_log = esp_timer_get_time();
    uint32_t frames_since = 0;
    while (s_diag >= 4)
    {
        const int kind = s_diag;

        if (kind == 7 && s_ram_music == NULL)
        {
            aud_cache_t *c = NULL;
            for (int i = 0; i < AUD_CACHE_SLOTS; i++)
            {
                if (s_cache[i].samples != NULL)
                {
                    c = &s_cache[i];
                    break;
                }
            }
            if (c != NULL)
            {
                size_t n = c->count > (size_t)AUD_RATE_PLAY ? (size_t)AUD_RATE_PLAY : c->count;
                s_ram_music = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (s_ram_music != NULL)
                {
                    memcpy(s_ram_music, c->samples, n * sizeof(int16_t));
                    s_ram_n = n;
                    ESP_LOGI(TAG, "ram music ready: %u samples (%u KB internal RAM)",
                             (unsigned)n, (unsigned)(n * 2 / 1024));
                }
            }
            if (s_ram_music == NULL)
            {
                ESP_LOGW(TAG, "ram music: no cache yet -> play a track once first");
            }
        }

        for (size_t i = 0; i < AUD_FRAMES; i++)
        {
            int32_t v = 0;
            if (kind == 5)
            {
                v = s_sin_tab[(ph1k >> 24) & 0xFF];
                ph1k += step1k;
            }
            else if (kind == 6)
            {
                v = s_sin_tab[(ph8k >> 24) & 0xFF];
                ph8k += step8k;
            }
            else if (kind == 7 && s_ram_music != NULL && s_ram_n > 0)
            {
                v = s_ram_music[ram_idx];
                if (++ram_idx >= s_ram_n)
                {
                    ram_idx = 0;
                }
            }
#if AUD_I2S_SLOT_32BIT
            s_frame_s32[2 * i]     = v << 16;
            s_frame_s32[2 * i + 1] = v << 16;
#else
            s_frame_st[2 * i]     = (int16_t)v;
            s_frame_st[2 * i + 1] = (int16_t)v;
#endif
        }
#if AUD_I2S_SLOT_32BIT
        esp_codec_dev_write(s_codec, s_frame_s32, AUD_FRAMES * 2 * sizeof(int32_t));
#else
        esp_codec_dev_write(s_codec, s_frame_st, AUD_FRAMES * 2 * sizeof(int16_t));
#endif
        if (++it % 10 == 0)
        {
            ESP_LOGI(TAG, "diag src running (kind=%d)", kind);
        }

        /* ★ 实测采样率：I2S 写是阻塞的、由 DMA 消费节奏决定，
         *   frames / 墙钟时间 = DAC 实际吃样本的速率 */
        frames_since += AUD_FRAMES;
        int64_t now = esp_timer_get_time();
        if (now - t_log >= 3000000)
        {
            float dt = (float)(now - t_log) / 1e6f;
            ESP_LOGI(TAG, "DIAG RATE: %.1f Hz  (nominal %d, frames %u in %.2fs)",
                     (float)frames_since / dt, AUD_RATE_PLAY, (unsigned)frames_since, dt);
            t_log = now;
            frames_since = 0;
        }
    }

    /* 退出前补一小段 0，避免残音；mute/PA 由 diag_set 负责 */
    memset(s_frame_st, 0, sizeof(s_frame_st));
    esp_codec_dev_write(s_codec, s_frame_st, AUD_FRAMES * 2 * sizeof(int16_t));
    s_diag_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_player_diag_set(int mode)
{
    if (s_codec == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode < 0 || mode > 7)
    {
        mode = 0;
    }

    /* 诊断音源与音乐互斥：切到 4/5 前先停音乐 */
    if (mode >= 4 && s_task != NULL)
    {
        s_playing = false;
        int w = 0;
        while (s_task != NULL && w++ < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    bool leaving_tone = (mode < 4 && s_diag >= 4);   /* 从诊断音源退出 */
    s_diag = mode;

    if (mode >= 4)
    {
        /* 诊断音源走 DAC 通路：切 PLAY（句柄互斥由模式机管理，PA 随之开） */
        if (aud_enter(AUD_MODE_PLAY) != ESP_OK)
        {
            return ESP_FAIL;
        }
    }
    else if (leaving_tone)
    {
        /* 等诊断任务退干净（它退出前还要补一段静音），然后恢复麦克风常开 */
        int w = 0;
        while (s_diag_task != NULL && w++ < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        aud_enter(AUD_MODE_REC);
    }
    else if (s_play_open)
    {
        /* 0~3 档只在 PLAY 态有意义：调 DAC mute / PA */
        int pa_level = (mode == 2 || mode == 3) ? 0 : 1;
        pa_set(pa_level);

        bool mute = (mode == 1 || mode == 3);
        esp_codec_dev_set_out_mute(s_codec, mute);
        ESP_LOGI(TAG, "diag mode=%d  PA=%d  dac_mute=%d", mode, pa_level, mute ? 1 : 0);
    }

    if (mode >= 4 && s_diag_task == NULL)
    {
        xTaskCreatePinnedToCore(diag_source_task, "diagsrc", 4096, NULL, 5, &s_diag_task, 0);
    }

    ESP_LOGI(TAG, "diag mode=%d  aud=%s", mode, s_aud_mode == AUD_MODE_PLAY ? "PLAY" : "REC");
    return ESP_OK;
}

int audio_player_diag_get(void)
{
    return s_diag;
}

esp_err_t audio_player_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_codec == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_play_open)
    {
        return ESP_OK;    /* 未在播放：先记住，起播时（enter PLAY）统一套用 */
    }
    return esp_codec_dev_set_out_vol(s_codec, vol);
}

int audio_player_get_volume(void)
{
    return s_volume;
}

/* ---------------- 从 SD 卡流式播放 ---------------- */

/** SD 播放任务：边读边播（SDMMC 不占 MSPI，读卡不会打断 LCD DMA）。
 *  每次读 AUD_FRAMES 帧，展开成 I2S 帧后写出。 */
static void sd_task(void *arg)
{
    char name[64];
    if (arg != NULL)
    {
        snprintf(name, sizeof(name), "%s", (const char *)arg);
        free(arg);
    }
    else if (!sd_card_find_first_wav(name, sizeof(name)))
    {
        ESP_LOGW(TAG, "sd: no wav to play");
        s_playing = false;
        s_state   = AUD_STATE_IDLE;   /* ★ 必须复位：漏了会让 UI 永远认为"正在播放" */
        s_task    = NULL;
        vTaskDelete(NULL);
        return;
    }

    char path[96];
    /* ★ 优先 /sdcard/music，回退根目录：兼容"音乐放子目录"和"音乐直接放根目录"两种卡 */
    snprintf(path, sizeof(path), "/sdcard/music/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        snprintf(path, sizeof(path), "/sdcard/%s", name);
        f = fopen(path, "rb");
    }
    if (f == NULL)
    {
        ESP_LOGE(TAG, "sd: open %s failed", path);
        s_playing = false;
        s_state   = AUD_STATE_IDLE;   /* ★ 必须复位，见上 */
        s_task    = NULL;
        vTaskDelete(NULL);
        return;
    }

    int      sr = 0, ch = 0;
    uint32_t data_bytes = 0;
    if (!parse_wav_header(f, &sr, &ch, &data_bytes))
    {
        ESP_LOGE(TAG, "sd: bad wav header (%s)", path);
        fclose(f);
        s_playing = false;
        s_state   = AUD_STATE_IDLE;   /* ★ 必须复位：voice_last.wav 录音未写完时就会走到这 */
        s_task    = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_pos_ms = 0;
    s_dur_ms = (uint32_t)((uint64_t)data_bytes * 1000ull /
                          ((uint64_t)AUD_RATE_PLAY * 2ull * (uint64_t)ch));

    ESP_LOGI(TAG, "sd play: %s  %dHz  %dch  %.1f s", name, sr, ch,
             (double)data_bytes / (double)(AUD_RATE_PLAY * 2 * ch));

    if (aud_enter(AUD_MODE_PLAY) != ESP_OK)   /* 互斥：切 PLAY（关 rec、PA 开） */
    {
        ESP_LOGE(TAG, "sd: enter PLAY failed");
        fclose(f);
        s_playing = false;
        s_state   = AUD_STATE_IDLE;
        s_task    = NULL;
        vTaskDelete(NULL);
        return;
    }
    esp_codec_dev_set_out_mute(s_codec, false);

    /* 读缓冲放内部 RAM（16KB），避免占 PSRAM 带宽 */
    static int16_t inbuf[AUD_FRAMES * 2];
    const size_t frame_bytes = (size_t)(2 * ch);
    uint32_t remain = data_bytes;
    uint32_t total  = data_bytes;
    const int fade_in = AUD_RATE_PLAY / 1000 * 30;
    uint32_t played_frames = 0;

    while (s_playing && remain >= frame_bytes)
    {
        size_t frames = remain / frame_bytes;
        if (frames > AUD_FRAMES)
        {
            frames = AUD_FRAMES;
        }
        size_t want = frames * frame_bytes;
        if (fread(inbuf, 1, want, f) != want)
        {
            ESP_LOGW(TAG, "sd: short read at %u/%u", (unsigned)(total - remain), (unsigned)total);
            break;
        }

        for (size_t i = 0; i < frames; i++)
        {
            int32_t v = (ch == 1) ? (int32_t)inbuf[i]
                                  : (((int32_t)inbuf[2 * i] + (int32_t)inbuf[2 * i + 1]) / 2);
            if (played_frames + i < (uint32_t)fade_in)
            {
                v = (v * (int32_t)(played_frames + i)) / fade_in;
            }
#if AUD_I2S_SLOT_32BIT
            s_frame_s32[2 * i]     = v << 16;
            s_frame_s32[2 * i + 1] = v << 16;
#else
            s_frame_st[2 * i]     = (int16_t)v;
            s_frame_st[2 * i + 1] = (int16_t)v;
#endif
        }
#if AUD_I2S_SLOT_32BIT
        esp_codec_dev_write(s_codec, s_frame_s32, frames * 2 * sizeof(int32_t));
#else
        esp_codec_dev_write(s_codec, s_frame_st, frames * 2 * sizeof(int16_t));
#endif
        played_frames += (uint32_t)frames;
        s_pos_ms       = (uint32_t)((uint64_t)played_frames * 1000ull / (uint64_t)AUD_RATE_PLAY);
        remain        -= (uint32_t)want;
    }

    /* 收尾：补一帧静音（★ 先别 mute！见下） */
    memset(s_frame_st, 0, sizeof(s_frame_st));
    esp_codec_dev_write(s_codec, s_frame_st, AUD_FRAMES * 2 * sizeof(int16_t));

    fclose(f);
    ESP_LOGI(TAG, "sd play done (%u frames)", (unsigned)played_frames);

    /* ★★ 2026-11-24 修：写完最后一块 ≠ 已经播完。
     *   AUD_FRAMES = 4096 帧 ≈ 93ms，写进 DMA 时缓冲里还有 1~2 块没放出去。
     *   原来紧接着就 set_out_mute(true) ⇒ 尾音被切掉（实测"话没说完就断"）；
     *   同时 s_state 立刻变 IDLE ⇒ 上层"等播完"的循环提前退出 ⇒ 绿灯提前灭。
     *   等 DMA 把尾巴放完（≈250ms，覆盖 2 个缓冲块）再静音、再置 IDLE。 */
    vTaskDelay(pdMS_TO_TICKS(250));

    esp_codec_dev_set_out_mute(s_codec, true);

    s_pos_ms  = 0;
    s_playing = false;
    s_state   = AUD_STATE_IDLE;
    s_task    = NULL;
    aud_enter(AUD_MODE_REC);          /* 播完回 REC */
    vTaskDelete(NULL);
}

esp_err_t audio_player_play_sd_by_name(const char *name)
{
    if (s_codec == NULL || !sd_card_ready() || name == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_diag != 0)
    {
        audio_player_diag_set(0);
    }

    char *arg = strdup(name);
    if (arg == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_playing = true;
    s_state   = AUD_STATE_LOADING;
    if (xTaskCreatePinnedToCore(sd_task, "musicsd", 6144, arg, 6, &s_task, 0) != pdPASS)
    {
        free(arg);
        s_playing = false;
        s_state   = AUD_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_player_play_sd(void)
{
    if (s_codec == NULL || !sd_card_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_diag != 0)
    {
        audio_player_diag_set(0);
    }
    s_playing = true;
    s_state   = AUD_STATE_LOADING;
    if (xTaskCreatePinnedToCore(sd_task, "musicsd", 6144, NULL, 6, &s_task, 0) != pdPASS)
    {
        s_playing = false;
        s_state   = AUD_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t audio_player_position_ms(void)
{
    return s_pos_ms;
}

uint32_t audio_player_duration_ms(void)
{
    return s_dur_ms;
}

/* ---------- 组件内部共享（audio_internal.h）：麦克风模块要取 RX 句柄 ---------- */

i2s_chan_handle_t audio_priv_rx(void)
{
    return s_rx;
}

bool audio_priv_rec_open(void)
{
    return s_rec_open;
}
