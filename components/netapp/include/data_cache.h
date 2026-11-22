/**
 * @file    data_cache.h
 * @brief   数据落盘缓存：断网也有画面、开机瞬间就有内容
 *
 * 为什么用 NVS 而不是 SPIFFS：
 *   数据总量 ~4KB（天气 220B / HF 1.2KB / AI 2.7KB / B站 若干），
 *   用不着文件系统，NVS 的 blob 直接对应"一个结构体一个 key"，长度不符就当作失效。
 *
 * ★★ 写 flash 有代价，调用方必须遵守：
 *   `nvs_commit()` 期间 **instruction cache 被禁用**，LVGL 的代码在 flash 里，
 *   任务一被调度就 stall；而 flush_cb 等 vsync 只等 200ms 就会超时放弃 ⇒ 画面撕裂。
 *   ⇒ **只在"数据已到、界面还没更新"的间隙写**（那几百 ms 画面是静止的，看不出来）。
 *   ⇒ 绝对不要在滚动/拖动过程中写。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

/** key 建议用短名（NVS 的 key 上限 15 字符）："wx" / "hf" / "ai" / "bili" */

/** 保存一个结构体。key 不存在则新建，存在则覆盖。 */
esp_err_t data_cache_save(const char *key, const void *data, size_t len);

/**
 * 读取。**只接受长度完全一致的数据**（结构体改过就当失效，不尝试兼容）。
 * 成功返回 ESP_OK 并把内容写进 data。
 */
esp_err_t data_cache_load(const char *key, void *data, size_t len);

/** 删掉某个 key（数据源判定缓存不可用时用） */
esp_err_t data_cache_erase(const char *key);
