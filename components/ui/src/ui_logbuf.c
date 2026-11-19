/**
 * @file    ui_logbuf.c
 * @brief   最近日志环形缓冲（拦截 esp_log，供"系统信息"页显示）
 *
 * 做法：用 esp_log_set_vprintf() 接管日志输出——
 *   ① 先原样转给原来的输出函数（串口照常打印，不丢日志）；
 *   ② 同时格式化一份到环形缓冲（24 行 × 96 字符）。
 *
 * ★ 过滤掉 "sysmon"（每秒一条，会把有用信息冲掉）。
 * ★ 用临界区而不是互斥锁：日志可能在任何任务/中断上下文产生，
 *   拿不到就丢一行，绝不能让打印把系统卡住。
 */
#include "ui_scenes.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LB_LINES  24
#define LB_CHARS  96

static char s_lb[LB_LINES][LB_CHARS];
static volatile int s_head;      /* 下一个写入位置 */
static volatile int s_count;     /* 已有行数（<= LB_LINES） */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;

static int lb_vprintf(const char *fmt, va_list args)
{
    /* 先复制一份参数：原始输出会把 args 消耗掉 */
    va_list cp;
    va_copy(cp, args);
    char tmp[LB_CHARS];
    vsnprintf(tmp, sizeof(tmp), fmt, cp);
    va_end(cp);

    /* 过滤 sysmon（每秒一条，会淹没其它信息） */
    if (strstr(tmp, "sysmon") == NULL && tmp[0] != '\0')
    {
        portENTER_CRITICAL(&s_mux);
        snprintf(s_lb[s_head], LB_CHARS, "%s", tmp);
        s_head = (s_head + 1) % LB_LINES;
        if (s_count < LB_LINES)
        {
            s_count++;
        }
        portEXIT_CRITICAL(&s_mux);
    }

    return s_orig ? s_orig(fmt, args) : 0;
}

void ui_logbuf_init(void)
{
    static bool inited;
    if (inited)
    {
        return;
    }
    inited = true;
    s_head = 0;
    s_count = 0;
    s_orig = esp_log_set_vprintf(lb_vprintf);
}

int ui_logbuf_copy(char dst[][96], int max_lines)
{
    if (dst == NULL || max_lines <= 0)
    {
        return 0;
    }

    int n = 0;
    portENTER_CRITICAL(&s_mux);
    int count = s_count;
    int head  = s_head;
    int take  = (count < max_lines) ? count : max_lines;
    /* 最新的在最后：从 head-1 往回取 */
    for (int i = take - 1; i >= 0; i--)
    {
        int idx = (head - 1 - i + LB_LINES * 2) % LB_LINES;
        snprintf(dst[n], 96, "%s", s_lb[idx]);
        n++;
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}
