/**
 ******************************************************************************
 * @file        exfuns.h
 * @version     V1.0
 * @brief       FATFS 扩展功能接口
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#ifndef __EXFUNS_H_
#define __EXFUNS_H_

#include <string.h>
#include "ff.h"


extern FATFS *fs[FF_VOLUMES];

/* 文件类型编码 (高4位=大类, 低4位=小类) */
#define T_BIN   0x00
#define T_LRC   0x10
#define T_NES   0x20
#define T_SMS   0x21
#define T_TEXT  0x30
#define T_C     0x31
#define T_H     0x32
#define T_WAV   0x40
#define T_MP3   0x41
#define T_APE   0x42
#define T_FLAC  0x43
#define T_BMP   0x51
#define T_JPG   0x52
#define T_JPEG  0x53
#define T_GIF   0x54
#define T_PNG   0x55
#define T_AVI   0x60


uint8_t exfuns_init(void);                              /* 申请内存 */
uint8_t exfuns_file_type(char *fname);                  /* 识别文件类型 */

#endif
