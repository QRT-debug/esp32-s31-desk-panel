/**
 ******************************************************************************
 * @file        recorder.c
 * @version     V1.0
 * @brief       WAV 录音后端: I2S 采集 + FatFs 写 SD 卡
 *              由 UI 通过 recorder_toggle / recorder_save 驱动
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "recorder.h"
#include "driver/gpio.h"
#include "text.h"

/* ------ 录音状态 ------ */
static uint32_t s_wav_size = 0;                 /* WAV 数据字节数 (不含文件头) */
static uint8_t  s_rec_sta  = 0;                 /* bit7=录音中  bit0=暂停 */
static __WaveHeader s_wavhead;                  /* WAV 文件头 */
static uint8_t  s_pname[64];                    /* 当前录音文件名 */
static uint8_t *s_cur_buf = NULL;               /* I2S RX 双缓冲 A */
static uint8_t *s_nxt_buf = NULL;               /* 双缓冲 B (A 写文件时 B 收数据, 交替使用) */

/* ------ FreeRTOS 同步 ------ */
static QueueSetHandle_t  s_qset       = NULL;   /* 队列集: 同时等待 开始/暂停 与 保存 两个信号量 */
static SemaphoreHandle_t s_toggle_sem = NULL;   /* 开始录音 / 暂停继续切换 */
static SemaphoreHandle_t s_save_sem   = NULL;   /* 保存文件并停止采集 */

/**
 * @brief   查询是否正在录音
 * @param   无
 * @retval  1=正在录音 (含暂停状态)
 */
uint8_t recorder_is_active(void)
{
    return (s_rec_sta & 0x80) ? 1 : 0;
}

/**
 * @brief   查询录音是否已暂停
 * @param   无
 * @retval  1=已暂停
 */
uint8_t recorder_is_paused(void)
{
    return ((s_rec_sta & 0x80) && (s_rec_sta & 0x01)) ? 1 : 0;
}

/**
 * @brief   查询当前录音比特率
 * @param   无
 * @retval  比特率 (bps)
 */
uint32_t recorder_bitrate(void)
{
    return s_wavhead.fmt.ByteRate * 8;
}

/**
 * @brief   显示录音时间与比特率
 * @param   tsec  录音时间 (秒)
 * @param   kbps  比特率 (bps, 显示时除以 1000)
 * @retval  无
 */
void recoder_msg_show(uint32_t tsec, uint32_t kbps)
{
    lcd_show_string(30, 210, 200, 16, 16, "TIME:", RED);
    lcd_show_xnum(30 + 40, 210, tsec / 60, 2, 16, 0x80, RED);   /* 分钟 */
    lcd_show_char(30 + 56, 210, ':', 16, 0, RED);
    lcd_show_xnum(30 + 64, 210, tsec % 60, 2, 16, 0x80, RED);   /* 秒 */
    lcd_show_string(140, 210, 200, 16, 16, "KBPS:", RED);
    lcd_show_num(140 + 40, 210, kbps / 1000, 4, 16, RED);
}

/**
 * @brief   初始化 WAV 文件头
 * @note    ChunkSize 字段先填 0, 录音结束时 f_lseek(0) 回填实际值
 * @param   无
 * @retval  无
 */
static void recorder_wav_init(void)
{
    s_wavhead.riff.ChunkID   = 0x46464952;      /* "RIFF" */
    s_wavhead.riff.ChunkSize = 0;               /* 结束时回填 */
    s_wavhead.riff.Format    = 0x45564157;      /* "WAVE" */
    s_wavhead.fmt.ChunkID    = 0x20746D66;      /* "fmt " */
    s_wavhead.fmt.ChunkSize  = 16;              /* PCM 格式固定 16 字节 */
    s_wavhead.fmt.AudioFormat  = 0x01;          /* PCM */
    s_wavhead.fmt.NumOfChannels = 2;            /* 双声道 */
    s_wavhead.fmt.SampleRate    = 16000;
    s_wavhead.fmt.BitsPerSample = 16;
    s_wavhead.fmt.BlockAlign = 2 * (16 / 8);    /* 声道数 * (每采样位数 / 8) */
    s_wavhead.fmt.ByteRate   = 16000 * 4;       /* 采样率 * 块对齐 */
    s_wavhead.data.ChunkID   = 0x61746164;      /* "data" */
    s_wavhead.data.ChunkSize = 0;               /* 结束时回填 */
}

/**
 * @brief   生成下一个可用的录音文件路径
 * @note    结果写入 s_pname, 形如 "0:/RECORDER/REC00001.wav"
 * @param   无
 * @retval  无
 */
static void recorder_new_pathname(void)
{
    uint16_t index = 0;
    static FIL ftemp;                           /* FatFs 0.15+: 需清零 (FF_USE_DYN_BUFFER) */

    memset(&ftemp, 0, sizeof(FIL));
    while (index < 0xFFFF) {
        sprintf((char *)s_pname, "0:/RECORDER/REC%05d.wav", index);
        if (f_open(&ftemp, (const TCHAR *)s_pname, FA_READ) == FR_NO_FILE) break;
        f_close(&ftemp);
        index++;
    }
}

/**
 * @brief   录音后端任务: 等待 UI 命令并完成 I2S 采集
 * @note    空闲/暂停时阻塞在队列集上等命令, 收到命令后执行:
 *          s_toggle_sem = 开始录音 / 暂停继续切换, s_save_sem = 保存文件并停止
 *          采集期间为非阻塞查询命令, 持续做 I2S RX → FatFs 写 SD 卡
 *          保存完成后通过 xTaskNotifyGive 通知 UI 任务
 * @param   arg  UI 任务句柄 (用于保存完成后发通知)
 * @retval  无
 */
static void recorder_task(void *arg)
{
    QueueSetMemberHandle_t activate_member = NULL;
    UINT bw;
    size_t bytes_read = 0;
    uint8_t file_write_res = 0;      /* 本轮 f_write 结果 (1=成功写入) */
    FF_DIR recdir;
    static FIL f_rec;
    TaskHandle_t ui_task_hdl = (TaskHandle_t)arg;

    /* --- 内存分配 --- */
    s_cur_buf = malloc(REC_RX_BUF_SIZE);
    s_nxt_buf = malloc(REC_RX_BUF_SIZE);
    if (!s_cur_buf || !s_nxt_buf) goto exit;

    memset(s_cur_buf, 0, REC_RX_BUF_SIZE);
    memset(s_nxt_buf, 0, REC_RX_BUF_SIZE);
    memset(&f_rec, 0, sizeof(FIL));
    memset(&s_wavhead, 0, sizeof(__WaveHeader));
    memset(s_pname, 0, sizeof(s_pname));

    recorder_wav_init();

    /* --- 确保 RECORDER 目录存在 --- */
    if (f_opendir(&recdir, "0:/RECORDER") != FR_OK) {
        f_mkdir("0:/RECORDER");
        f_opendir(&recdir, "0:/RECORDER");
    }
    f_closedir(&recdir);

    /* --- 通知 UI: 后端就绪 --- */
    xTaskNotifyGive(ui_task_hdl);

    while (1) {
        /* --- 等待 UI 命令 ---
         * 采集期间采用非阻塞查询: I2S RX 的 DMA 只有 6×240 帧 = 5760 字节(约90ms),
         */
        TickType_t wait = ((s_rec_sta & 0x80) && !(s_rec_sta & 0x01)) ? 0 : pdMS_TO_TICKS(100);
        activate_member = xQueueSelectFromSet(s_qset, wait);

        if (activate_member != NULL) {
            xSemaphoreTake(activate_member, portMAX_DELAY);

            /* --- 开始录音 / 暂停继续 切换 --- */
            if (activate_member == s_toggle_sem) {
                if (s_rec_sta & 0x80) {
                    /* 录音中 → 暂停/继续 切换 */
                    s_rec_sta ^= 0x01;
                } else {
                    /* 未录音 → 开始录音: 开 ADC + 新建文件 */
                    gpio_set_level(PA_CTRL_GPIO_PIN, 0);        /* 录音时关功放 */
                    myes8389_play_stop();
                    myes8389_record_start(I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);
                    recorder_new_pathname();

                    text_show_string(30, 190, lcddev.width, 16, "REC:", 16, 0, RED);
                    text_show_string(30 + 40, 190, lcddev.width, 16, (char *)s_pname + 11, 16, 0, RED);

                    if (f_open(&f_rec, (const TCHAR *)s_pname, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                        f_lseek(&f_rec, sizeof(__WaveHeader));  /* 先跳过文件头, 结束时回填 */
                        s_wav_size = 0;
                        s_rec_sta = 0x80;                       /* 标记: 正在录音 */
                    } else {
                        text_show_string(30, 170, lcddev.width, 16, "File Open Fail!", 16, 0, RED);
                        s_rec_sta = 0;
                    }
                }
            }
            /* --- 保存文件并停止采集 --- */
            else if (activate_member == s_save_sem) {
                if ((s_rec_sta & 0x80) && (file_write_res == 1 || (s_wav_size == 0 && file_write_res == 0))) {
                    /* 回填 WAV 头 + 关闭文件 */
                    s_wavhead.riff.ChunkSize = s_wav_size + 36;
                    s_wavhead.data.ChunkSize = s_wav_size;
                    f_lseek(&f_rec, 0);
                    f_write(&f_rec, &s_wavhead, sizeof(__WaveHeader), &bw);
                    f_close(&f_rec);
                    myes8389_record_stop();

                    s_wav_size = 0;
                    s_rec_sta = 0;
                    lcd_fill(30, 170, lcddev.width, 248, WHITE);     /* 清掉录音信息区 */

                    xTaskNotifyGive(ui_task_hdl);                    /* 通知 UI: 保存完成 */
                }
            }
        }

        /* --- 录音中且未暂停: I2S RX → FatFs --- */
        if ((s_rec_sta & 0x80) && !(s_rec_sta & 0x01)) {
            file_write_res = 0;
            bytes_read = i2s_rx_read(s_cur_buf, REC_RX_BUF_SIZE);
            bytes_read &= ~3U;                                  /* 对齐到 16bit 立体声帧边界 */

            if (bytes_read > 0) {
                if (f_write(&f_rec, s_cur_buf, bytes_read, &bw) == FR_OK) {
                    file_write_res = 1;
                    s_wav_size += bytes_read;
                    /* 双缓冲交换 */
                    uint8_t *temp = s_cur_buf;
                    s_cur_buf = s_nxt_buf;
                    s_nxt_buf = temp;
                }
            }
        }
    }

exit:
    free(s_cur_buf); free(s_nxt_buf);
    s_cur_buf = NULL; s_nxt_buf = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief   初始化录音后端 (开机调用一次)
 * @note    创建同步对象并启动采集任务, 等待任务就绪后返回
 * @param   无
 * @retval  无
 */
void recorder_init(void)
{
    /* --- 同步对象: 队列集 + 两个二值信号量, 构成 UI → 录音任务的命令通道 --- */
    s_qset       = xQueueCreateSet(2);                  /* 队列集容量 2, 对应下面两个信号量 */
    s_toggle_sem = xSemaphoreCreateBinary();            /* 命令: 开始录音 / 暂停继续切换 */
    s_save_sem   = xSemaphoreCreateBinary();            /* 命令: 保存文件并停止采集 */
    xQueueAddToSet(s_toggle_sem, s_qset);               /* 都加入队列集, 任务才能一次等到任意一个 */
    xQueueAddToSet(s_save_sem, s_qset);

    /* --- 创建录音采集任务: 固定在核心0(与核心1的UI分开), 优先级10高于UI的5 ---
     * 参数 ui_task 作为任务入参传出, 保存完成后用它 xTaskNotifyGive 通知 UI 任务
     */
    TaskHandle_t ui_task = xTaskGetCurrentTaskHandle();
    BaseType_t task_created = xTaskCreatePinnedToCore(recorder_task, "recorder_task",
                                                      16384, ui_task, 10, NULL, 0);
    assert(task_created == pdTRUE);                     /* 创建失败则停机 */

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);            /* 等待任务就绪 */
}

/**
 * @brief   开始录音 / 暂停继续 切换
 * @note    异步: 只给后端任务发信号, 实际动作由 recorder_task 完成
 * @param   无
 * @retval  无
 */
void recorder_toggle(void)
{
    if (s_toggle_sem)
    {
        xSemaphoreGive(s_toggle_sem);
    }
}

/**
 * @brief   保存当前录音文件并停止采集
 * @note    阻塞等待后端完成, 最长 5s
 * @param   无
 * @retval  1=保存成功  0=未在录音/保存失败
 */
uint8_t recorder_save(void)
{
    if (!(s_rec_sta & 0x80)) return 0;                  /* 未在录音 */

    xSemaphoreGive(s_save_sem);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) != pdTRUE) {
        printf("[REC] save timeout!\n");
        return 0;
    }
    return 1;
}
