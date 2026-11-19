/**
 ******************************************************************************
 * @file        audioplay.c
 * @version     V1.0
 * @brief       音乐播放器应用层
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "audioplay.h"
#include <stdlib.h>

__audiodev g_audiodev;

/**
 * @brief   置播放中标志
 * @note    bit0=0 非暂停, bit1=1 播放开启
 * @param   无
 * @retval  无
 */
void audio_start(void)
{
    g_audiodev.status = 3 << 0;
}

/**
 * @brief   统计路径下的 WAV 文件个数
 * @param   path  目录路径, 如 "0:/RECORDER"
 * @retval  WAV 文件个数
 */
uint16_t audio_get_tnum(uint8_t *path)
{
    uint16_t rval = 0;
    FF_DIR tdir;
    FILINFO *fi = (FILINFO *)malloc(sizeof(FILINFO));

    if (!fi) return 0;

    if (f_opendir(&tdir, (const TCHAR *)path) == FR_OK) {
        while (f_readdir(&tdir, fi) == FR_OK && fi->fname[0] != 0) {
            if ((exfuns_file_type(fi->fname) & 0xF0) == 0x40) rval++;
        }
        f_closedir(&tdir);
    }

    free(fi);
    return rval;
}

/**
 * @brief   显示当前曲目编号/总曲目数
 * @param   index  当前序号 (从 1 开始)
 * @param   total  总曲目数
 * @retval  无
 */
void audio_index_show(uint16_t index, uint16_t total)
{
    lcd_show_num(30, 230, index, 3, 16, RED);
    lcd_show_char(30 + 24, 230, '/', 16, 0, RED);
    lcd_show_num(30 + 32, 230, total, 3, 16, RED);
}

/**
 * @brief   显示播放进度与比特率
 * @param   totsec   文件总时长(秒)
 * @param   cursec   当前播放时长(秒)
 * @param   bitrate  比特率(bps)
 * @retval  无
 */
void audio_msg_show(uint32_t totsec, uint32_t cursec, uint32_t bitrate)
{
    lcd_show_xnum(30, 210, cursec / 60, 2, 16, 0X80, RED);
    lcd_show_char(30 + 16, 210, ':', 16, 0, RED);
    lcd_show_xnum(30 + 24, 210, cursec % 60, 2, 16, 0X80, RED);
    lcd_show_char(30 + 40, 210, '/', 16, 0, RED);
    lcd_show_xnum(30 + 48, 210, totsec / 60, 2, 16, 0X80, RED);
    lcd_show_char(30 + 64, 210, ':', 16, 0, RED);
    lcd_show_xnum(30 + 72, 210, totsec % 60, 2, 16, 0X80, RED);
    lcd_show_num(140, 210, bitrate / 1000, 4, 16, RED);
    lcd_show_string(172, 210, 64, 16, 16, "Kbps", RED);
}
