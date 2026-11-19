/**
 ******************************************************************************
 * @file        exfuns.c
 * @version     V1.0
 * @brief       FATFS 扩展功能, 提供文件类型识别
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "exfuns.h"


#define FILE_MAX_TYPE_NUM       7       /* 最多FILE_MAX_TYPE_NUM个大类 */
#define FILE_MAX_SUBT_NUM       7       /* 最多FILE_MAX_SUBT_NUM个小类 */

/* 文件类型定义 */
static const char *FILE_TYPE_TBL[FILE_MAX_TYPE_NUM][FILE_MAX_SUBT_NUM] = {
    {"BIN"," "," "," "," "," "," "},                        /* BIN文件 */
    {"LRC"," "," "," "," "," "," "},                        /* LRC文件 */
    {"NES", "SMS"," "," "," "," "," "},                     /* NES/SMS文件 */
    {"TXT", "C", "H"," "," "," "," "},                      /* 文本文件 */
    {"WAV", "MP3", "OGG", "FLAC", "AAC", "WMA", "MID"},     /* 音乐文件 */
    {"DB","BMP", "JPG", "JPEG", "GIF","PNG"," "},           /* 图片文件 */
    {"AVI"," "," "," "," "," "," "},                        /* 视频文件 */
};

/* 逻辑磁盘工作区(在调用任何FATFS相关函数之前,必须先给fs申请内存) */
FATFS *fs[FF_VOLUMES];


/**
 * @brief       为exfuns申请内存
 * @param       无
 * @retval      0=成功  1=失败
 */
uint8_t exfuns_init(void)
{
    uint8_t i;

    for (i = 0; i < FF_VOLUMES; i++)
    {
        fs[i] = (FATFS *)malloc(sizeof(FATFS));

        if (!fs[i])break;
    }

    if (i == FF_VOLUMES)
    {
        return 0;
    }

    return 1;
}

/**
 * @brief       将小写字母转为大写字母, 其他字符保持不变
 * @param       c  待转换字符
 * @retval      转换后的字符
 */
static uint8_t exfuns_char_upper(uint8_t c)
{
    if (c < 'A') return c;

    if (c >= 'a')
    {
        return c - 0x20;
    }

    return c;
}

/**
 * @brief       报告文件的类型
 * @param       fname : 文件名
 * @retval      高四位表示所属大类, 低四位表示所属小类; 0XFF表示无法识别
 */
uint8_t exfuns_file_type(char *fname)
{
    uint8_t tbuf[5];
    char *attr = 0;
    uint8_t i = 0, j;

    while (i < 250)
    {
        i++;

        if (*fname == '\0')break;

        fname++;
    }

    if (i == 250)return 0XFF;

    for (i = 0; i < 5; i++)
    {
        fname--;

        if (*fname == '.')
        {
            fname++;
            attr = fname;
            break;
        }
    }

    if (attr == 0) return 0XFF;

    strcpy((char *)tbuf, (const char *)attr);

    for (i = 0; i < 4; i++) tbuf[i] = exfuns_char_upper(tbuf[i]);

    for (i = 0; i < FILE_MAX_TYPE_NUM; i++)
    {
        for (j = 0; j < FILE_MAX_SUBT_NUM; j++)
        {
            if (*FILE_TYPE_TBL[i][j] == 0) break;

            if (strcmp((const char *)FILE_TYPE_TBL[i][j], (const char *)tbuf) == 0)
            {
                return (i << 4) | j;
            }
        }
    }

    return 0XFF;
}
