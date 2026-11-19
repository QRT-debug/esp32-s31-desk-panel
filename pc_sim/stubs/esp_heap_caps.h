/** esp_heap_caps.h (PC 模拟器 stub)：返回假的堆数据，让系统信息页有内容可显示 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_DEFAULT   (1 << 12)

size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
