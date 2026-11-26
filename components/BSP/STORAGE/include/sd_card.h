/**
 * @file    sd_card.h
 * @brief   TF 卡（SDMMC 4-bit）挂载
 *
 * 引脚（原理图核对，各经 22Ω 串阻 R63~R68）：
 *   D0=IO20  D1=IO21  D2=IO22  D3=IO23  CLK=IO24  CMD=IO25
 *
 * ★ SDMMC 是独立控制器 + 独立 DMA，不占 MSPI ⇒ 读卡不会打断 RGB LCD 的 DMA。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 挂载到 /sdcard（FATFS）。卡不存在/格式不支持时返回错误，调用方应回退到 /music */
esp_err_t sd_card_mount(void);

/** 是否已成功挂载 */
bool sd_card_ready(void);

/** 取卡容量（字节）；未挂载返回 0 */
uint64_t sd_card_capacity(void);

/**
 * 插拔检测（每秒调一次即可，不阻塞）：
 *   - 已挂载 → 读卡状态，失败则判定"卡被拔掉"，自动卸载并置 ready=false
 *   - 未挂载 → 每 2 秒静默尝试重新挂载（卡插回时自动恢复）
 * ★ TF 卡座没有 CD 检测引脚（原理图只有 CLK/CMD/D0~D3），所以只能软件轮询。
 * @return 当前是否可用
 */
bool sd_card_poll(void);

/** 卸载 FATFS（MSC 模式要把整张卡交给 USB 时先调这个）。
 *  卸载后 sd_card_ready() 返回 false，播放/扫描都会失败——这是预期。 */
esp_err_t sd_card_unmount(void);

/** 取底层的 sdmmc_card_t*（供 USB MSC 直接读写块设备）；未挂载返回 NULL */
void *sd_card_handle(void);

/** 在 /sdcard 根目录找第一个 .wav（大小写不敏感）。找到写文件名并返回 true */
bool sd_card_find_first_wav(char *name, size_t len);

/** 扫描 /sdcard 根目录所有 .wav，按文件名升序写入 names（每项 64 字节）。
 *  返回找到的数量（最多 max 个）；未挂载返回 0。 */
int sd_card_scan_wav(char names[][64], int max);

/** 确保标准目录存在（music/wallpaper/voice/debug/data/font/cache/update）。幂等。 */
void sd_card_ensure_dirs(void);
