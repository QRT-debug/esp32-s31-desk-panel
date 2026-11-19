/** esp_chip_info.h (PC 模拟器 stub)：返回假的芯片信息 */
#pragma once

#include <stdint.h>

typedef struct {
    int      model;
    int      features;
    uint16_t revision;
    uint8_t  cores;
} esp_chip_info_t;

enum { CHIP_FEATURE_EMB_FLASH = 1 << 0, CHIP_FEATURE_WIFI_BGN = 1 << 1 };

void esp_chip_info(esp_chip_info_t *out);
