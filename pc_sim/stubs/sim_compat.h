/**
 * @file    sim_compat.h  (PC 模拟器兼容层)
 * @brief   MinGW 缺 POSIX 的 localtime_r，用 localtime_s 包装成同名函数
 *
 * 通过 -include 强制注入所有模拟器源文件（只影响 pc_sim 这个 target，
 * 不会污染 LVGL 自己的编译）。
 */
#pragma once

#include <time.h>

static inline struct tm *sim_localtime_r(const time_t *t, struct tm *out)
{
    if (t != NULL && out != NULL)
    {
        time_t v = *t;
        localtime_s(out, &v);
    }
    return out;
}

#ifndef localtime_r
#define localtime_r sim_localtime_r
#endif
