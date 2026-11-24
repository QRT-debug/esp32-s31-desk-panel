/**
 * 语音服务实现：录音 → 能量 VAD 断句 → WAV → POST /api/asr → 文本事件
 *
 * ★ 架构约束（写给以后的自己）：
 *   1. 本组件只产出**文本**，不关心谁在用（任务/对话/agent 各自订阅）。
 *   2. 绝不在本组件里调 LVGL —— 事件通过订阅回调抛给 UI 层，由 UI 自己切回
 *      LVGL 上下文（或让 ui_manager 轮询标志，参考 scene_clock 的教训）。
 *   3. 录音缓冲在 PSRAM（rec_max_sec × 32KB/s，10s ≈ 320KB）。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>     /* mkdir：调试录音子目录 */
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

#include "app_cfg.h"
#include "audio_player.h"
#include "audio_rec.h"
#include "led_service.h"
#include "voice.h"

static const char *TAG = "voice";

#define REC_RATE_HZ      16000
#define BYTES_PER_SEC    (REC_RATE_HZ * 2)     /* 16bit 单声道 */
#define BLOCK_SAMPLES    512                   /* 每次读 512 样本 = 32ms */
#define SILENCE_END_MS   800                   /* 静音这么久 = 一句话结束 */
#define MIN_SPEECH_MS    300                   /* 短于这个 = 误触，丢弃 */
#define MAX_SUBS         4

static voice_cb_t      s_subs[MAX_SUBS];
static int             s_sub_n;
static volatile voice_mode_t s_mode = VOICE_MODE_TASK;
static volatile bool   s_listening;
static volatile bool   s_busy;      /* 全流程忙碌：触发 -> 录音 -> 上传完成 */
static volatile bool   s_trigger;
static SemaphoreHandle_t s_lock;

/* ---------------- 事件分发 ---------------- */

static void emit(voice_ev_t t, const char *text)
{
    voice_evt_t e = { .type = t, .text = text, .text_len = text ? (int)strlen(text) : 0 };
    for (int i = 0; i < s_sub_n; i++)
    {
        if (s_subs[i])
        {
            s_subs[i](&e);
        }
    }
}

/* ---------------- WAV 头 ---------------- */

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    uint32_t chunk = 36 + data_bytes;
    uint32_t rate  = REC_RATE_HZ;
    uint32_t brate = REC_RATE_HZ * 2;
    memcpy(h + 0,  "RIFF", 4);
    memcpy(h + 4,  &chunk, 4);
    memcpy(h + 8,  "WAVEfmt ", 8);
    uint32_t sz = 16;
    memcpy(h + 16, &sz, 4);
    uint16_t fmt = 1, ch = 1;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &brate, 4);
    uint16_t align = 2, bits = 16;
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

/* ---------------- ASR 上传 ---------------- */

/* ★ perform() 之后 read_response 读不到 body（内部已消费）——用事件回调收。 */
typedef struct
{
    char *buf;
    int   len;
    int   cap;
} http_rx_t;

static esp_err_t on_http_rx(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data != NULL)
    {
        http_rx_t *r = (http_rx_t *)e->user_data;
        int n = e->data_len;
        if (n > 0 && r->len + n < r->cap)
        {
            memcpy(r->buf + r->len, e->data, (size_t)n);
            r->len += n;
            r->buf[r->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ★ 上传结果要区分原因：只说 "asr failed" 时，用户分不清"服务没开"和"识别出错" */
typedef enum
{
    ASR_OK = 0,
    ASR_ERR_CONNECT,   /* 连不上（PC 服务未启动 / 网络不通） */
    ASR_ERR_HTTP,      /* 服务返回非 200 */
    ASR_ERR_EMPTY,     /* 200 但响应为空（没识别出内容） */
} asr_status_t;

static asr_status_t asr_upload(const uint8_t *wav, int wav_len, char *out, size_t out_sz)
{
    const app_cfg_t *c = app_cfg_get();
    char url[160];

    if (c->asr_url[0] != '\0')
    {
        snprintf(url, sizeof(url), "%s", c->asr_url);
    }
    else
    {
        /* PC 中转：http://pc_host:pc_port/api/asr（key 不出 PC） */
        snprintf(url, sizeof(url), "http://%s:8081/api/asr", c->pc_host);   /* 8080 是 pc_monitor，避开 */
    }

    http_rx_t rx = { .buf = out, .len = 0, .cap = (int)out_sz };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 15000,
        .event_handler = on_http_rx,
        .user_data     = &rx,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL)
    {
        return ASR_ERR_CONNECT;
    }
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    if (c->asr_key[0] != '\0')
    {
        esp_http_client_set_header(cli, "Authorization", c->asr_key);
    }
    esp_http_client_set_post_field(cli, (const char *)wav, wav_len);

    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);

    asr_status_t st;
    if (err != ESP_OK || code != 200)
    {
        /* code==0 说明连 TCP 都没建立（服务没跑 / IP 变了 / 防火墙） */
        st = (code == 0) ? ASR_ERR_CONNECT : ASR_ERR_HTTP;
        ESP_LOGW(TAG, "asr http %d err %s -> %s", code, esp_err_to_name(err),
                 st == ASR_ERR_CONNECT ? "connect fail" : "http error");
    }
    else
    {
        int len = rx.len;
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
        {
            out[--len] = '\0';
        }
        st = (len > 0) ? ASR_OK : ASR_ERR_EMPTY;
        ESP_LOGI(TAG, "asr http 200, body %d B", len);
    }
    esp_http_client_cleanup(cli);
    return st;
}

/* ---------------- AI 回复：文本 → PC 合成 → 播回（2026-11-24） ----------------
 * ★ 为什么落盘到 SD 而不是内存：
 *   audio_player 只能从 /music(vfs) 或 /sdcard 读文件，没有内存播放接口；
 *   而写 vfs 就是写 flash —— flash 写入期间禁 i-cache，LVGL 会卡住、画面撕裂（已知坑）。
 *   所以走 SD 卡，并且放在 voice/ 子目录：AUDIO 页只扫根目录，不会把它列成曲目。
 *
 * ★ 为什么播完要在这里阻塞等待：
 *   麦克风是常开的，喇叭一响就会被录进去 ⇒ 能量 VAD 立刻触发 ⇒ 又发一次请求
 *   ⇒ 自己跟自己无限对话。等播放结束再返回，可彻底避免这个自激。 */
static esp_err_t reply_on_data(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data != NULL && e->data_len > 0)
    {
        FILE *f = (FILE *)e->user_data;
        if (fwrite(e->data, 1, (size_t)e->data_len, f) != (size_t)e->data_len)
        {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* 服务端把回复文本放在 X-Reply-Text 头里并做了 percent-encode */
static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dst_sz; i++)
    {
        if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0')
        {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else if (src[i] == '+')
        {
            dst[o++] = ' ';
        }
        else
        {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

static void request_and_play_reply(const char *user_text)
{
    const app_cfg_t *c = app_cfg_get();
    char             url[160];
    snprintf(url, sizeof(url), "http://%s:8081/api/reply", c->pc_host);

    mkdir("/sdcard/voice", 0777);            /* 已存在返回 EEXIST，无害 */
    FILE *f = fopen("/sdcard/voice/reply.wav", "wb");
    if (f == NULL)
    {
        emit(VOICE_EV_ERROR, "打不开 SD 卡");
        return;
    }

    /* JSON body：手写转义，只处理 " 和 \ 两种（够用，且不引入 cJSON 依赖） */
    char   body[600];
    size_t o = 0;
    o += (size_t)snprintf(body + o, sizeof(body) - o, "{\"text\":\"");
    for (const char *p = user_text; *p != '\0' && o + 4 < sizeof(body); p++)
    {
        if (*p == '"' || *p == '\\')
        {
            body[o++] = '\\';
        }
        body[o++] = *p;
    }
    snprintf(body + o, sizeof(body) - o, "\"}");

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 20000,      /* 对话+合成 2~3s，20s 足够且不会让人等太久 */
        .event_handler = reply_on_data,
        .user_data     = f,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL)
    {
        fclose(f);
        emit(VOICE_EV_ERROR, "http 初始化失败");
        return;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, (int)strlen(body));

    esp_err_t err  = esp_http_client_perform(cli);
    int       code = esp_http_client_get_status_code(cli);

    char  reply[256] = { 0 };
    char *hval       = NULL;
    if (esp_http_client_get_header(cli, "X-Reply-Text", &hval) == ESP_OK && hval != NULL)
    {
        url_decode(hval, reply, sizeof(reply));
    }
    esp_http_client_cleanup(cli);
    fclose(f);

    if (err != ESP_OK || code != 200)
    {
        ESP_LOGW(TAG, "reply http %d err %s", code, esp_err_to_name(err));
        emit(VOICE_EV_ERROR, (code == 0) ? "连不上 PC（服务没开？）" : "AI 回复失败");
        return;
    }
    if (reply[0] != '\0')
    {
        emit(VOICE_EV_REPLY, reply);
    }

    /* "voice/reply.wav" 会被拼成 /sdcard/voice/reply.wav */
    if (audio_player_play_sd_by_name("voice/reply.wav") != ESP_OK)
    {
        emit(VOICE_EV_ERROR, "播放失败");
        return;
    }

    /* ★ 等播完（最多 60s）：期间本任务阻塞，麦克风不会把喇叭声录回去 */
    for (int i = 0; i < 600 && audio_player_is_playing(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------------- 录音 + VAD ---------------- */

static void voice_task(void *arg)
{
    (void)arg;

    /* 缓冲按最长录音秒数分配（PSRAM） */
    uint32_t max_sec = app_cfg_get()->rec_max_sec;
    if (max_sec < 1 || max_sec > 30)
    {
        max_sec = 10;
    }
    const int cap_samples = (int)max_sec * REC_RATE_HZ;
    int16_t  *pcm = heap_caps_malloc((size_t)cap_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint8_t  *wav = heap_caps_malloc((size_t)cap_samples * sizeof(int16_t) + 44, MALLOC_CAP_SPIRAM);
    if (pcm == NULL || wav == NULL)
    {
        ESP_LOGE(TAG, "no psram for record buffer (%d samples)", cap_samples);
        vTaskDelete(NULL);
        return;
    }

    char text[512];

    int s_peak_rms = 0;     /* 本次录音峰值（每次开始清零） */

    while (1)
    {
        if (!s_trigger)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        s_trigger   = 0;
        s_listening = true;      /* 真正开始录音 */
        audio_rec_begin();      /* 重置通道判定，本次录音重新锁定 */
        s_peak_rms = 0;         /* ★ 每次清零：static 累计会让日志数据完全失真 */
        ESP_LOGI(TAG, "listening (max %us, vad %u)", (unsigned)max_sec,
                 (unsigned)app_cfg_get()->vad_thresh);

        /* ★ LED 服务用法示例：说话时彩虹渐变（提示"正在听"），结束后自动恢复底色。
         *   3600ms 走完一圈色相（≈100°/s）——太快会有"跳色"感，太慢又看不出在动。 */
        led_rainbow(3600);

        int  used       = 0;
        int  silence_ms = 0;
        int  voiced_ms  = 0;
        bool started    = false;
        /* s_peak_rms 已提升到任务内（每次录音清零），见下方 while 前 */

        while (used < cap_samples)
        {
            int n = audio_rec_read_mono(pcm + used, BLOCK_SAMPLES,
                                        (int)(cap_samples - used < BLOCK_SAMPLES
                                                  ? cap_samples - used : BLOCK_SAMPLES));
            if (n <= 0)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            /* 能量（RMS）判断 */
            long acc = 0;
            for (int i = 0; i < n; i++)
            {
                acc += (long)pcm[used + i] * pcm[used + i];
            }
            int rms = (int)sqrt((double)acc / n);

            used += n;
            int block_ms = n * 1000 / REC_RATE_HZ;

            if (rms > s_peak_rms)
            {
                s_peak_rms = rms;
            }
            if (rms > (int)app_cfg_get()->vad_thresh)
            {
                if (!started)
                {
                    started = true;
                    emit(VOICE_EV_SPEECH_START, NULL);
                }
                voiced_ms  += block_ms;
                silence_ms  = 0;
            }
            else if (started)
            {
                silence_ms += block_ms;
                if (silence_ms >= SILENCE_END_MS)
                {
                    break;
                }
            }
        }

        s_listening = false;
        led_stop_effect();      /* 录音结束即停呼吸（上传阶段不再占着灯） */

        if (voiced_ms < MIN_SPEECH_MS)
        {
            ESP_LOGI(TAG, "too short (%dms), discard", voiced_ms);
            s_busy = false;     /* ★ 提前退出也必须释放，否则此后永远"设备忙" */
            continue;
        }

        int data_bytes = used * 2;
        wav_header(wav, (uint32_t)data_bytes);
        memcpy(wav + 44, pcm, (size_t)data_bytes);
        emit(VOICE_EV_SPEECH_END, NULL);

        ESP_LOGI(TAG, "uploading %d bytes (%.1fs speech, peak rms %d)",
                 data_bytes + 44, voiced_ms / 1000.0f, s_peak_rms);
        /* ★ 调试取证：把真实录音存到 SD 卡，可拷到 PC 直接听/分析波形——
         *   区分"麦克风信号差"和"模型识别差"的唯一硬办法。
         * ★★ 必须放 debug/ 子目录，不能放 /sdcard 根目录：
         *   根目录是 AUDIO 页的曲目扫描范围（sd_card_scan_wav），而这个文件是
         *   16kHz 单声道、且录音时会被持续重写 —— 被当成曲目点播放时会解析失败，
         *   把播放器卡在 LOADING 态（2026-11-23 用户实测踩到）。
         *   扫描侧也加了同名排除，双保险。 */
        {
            mkdir("/sdcard/debug", 0777);          /* 已存在返回 EEXIST，无害 */
            FILE *dbg = fopen("/sdcard/debug/voice_last.wav", "wb");
            if (dbg != NULL)
            {
                fwrite(wav, 1, (size_t)(data_bytes + 44), dbg);
                fclose(dbg);
                ESP_LOGI(TAG, "dump /sdcard/debug/voice_last.wav (%d B)", data_bytes + 44);
            }
        }

        asr_status_t rst = asr_upload(wav, data_bytes + 44, text, sizeof(text));
        if (rst == ASR_OK)
        {
            ESP_LOGI(TAG, "asr: \"%s\"", text);
            /* ★ 打字节：若尾部是半个 UTF-8 字符（被截断），LVGL 会渲染成方框 */
            {
                char hx[200] = { 0 };
                int  ln = (int)strlen(text);
                for (int k = 0; k < ln && k < 60; k++)
                {
                    char t2[5];
                    snprintf(t2, sizeof(t2), "%02X ", (unsigned char)text[k]);
                    strlcat(hx, t2, sizeof(hx));
                }
                ESP_LOGI(TAG, "asr bytes(%d): %s", ln, hx);
            }
            emit(VOICE_EV_TEXT, text);

            /* ★ 2026-11-24：识别成功 → 交给 PC 生成回复并合成语音，拿回来播放。
             *   本调用会阻塞到播放结束（防自激），期间不监听麦克风。 */
            emit(VOICE_EV_THINKING, NULL);
            request_and_play_reply(text);
        }
        else
        {
            const char *msg = "识别失败";
            if (rst == ASR_ERR_CONNECT)
            {
                msg = "连不上识别服务（PC 上需运行 启动语音服务.bat）";
            }
            else if (rst == ASR_ERR_HTTP)
            {
                msg = "识别服务返回错误（看 PC 窗口日志）";
            }
            else if (rst == ASR_ERR_EMPTY)
            {
                msg = "没听清，请再说一次";
            }
            emit(VOICE_EV_ERROR, msg);
        }

        s_busy = false;     /* 全流程结束（含上传/识别），释放占位 */
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t voice_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    if (xTaskCreate(voice_task, "voice", 6144, NULL, 4, NULL) != pdPASS)
    {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "voice service ready (PC relay: http://%s:%u/api/asr)",
             app_cfg_get()->pc_host, (unsigned)app_cfg_get()->pc_port);
    return ESP_OK;
}

int voice_subscribe(voice_cb_t cb)
{
    if (cb == NULL)
    {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int id = -1;
    if (s_sub_n < MAX_SUBS)
    {
        id = s_sub_n;
        s_subs[s_sub_n++] = cb;
    }
    xSemaphoreGive(s_lock);
    return id;
}

void voice_set_mode(voice_mode_t m)
{
    s_mode = m;
}

voice_mode_t voice_get_mode(void)
{
    return s_mode;
}

bool voice_listen(void)
{
    /* ★ 三态语义（原来只有 s_listening，导致上传期间 UI 显示"正在听"却没在录）：
     *   s_busy      = 从触发到上传/识别完成，全程占位 → 防连点、防重入
     *   s_listening = 真正在录音 → UI 显示"正在听"、LED 呼吸
     *   必须先判 busy：上传阶段 listening 已是 false，只有 busy 能拦住第二次点击。 */
    if (s_busy || s_listening || audio_player_is_playing())
    {
        return false;
    }
    s_busy    = true;      /* 立刻占位，不等任务调度 */
    s_trigger = 1;
    return true;
}

bool voice_is_listening(void)
{
    return s_listening;
}

bool voice_is_busy(void)
{
    return s_busy;
}
