/**
 * @file    pc_monitor.h
 * @brief   拉取电脑状态（HTTP JSON）并提供一份线程安全的快照
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PC_MAX_DISKS 4

typedef struct
{
    char  name[8];
    float percent;
    float free_gb;
    float total_gb;
} pc_disk_t;

typedef struct
{
    bool  valid;              /* 是否有过成功的数据 */
    char  host[32];
    int   uptime_s;

    char  cpu_name[48];       /* CPU 型号（服务端从注册表取，如 "i5-14600K"） */
    float cpu_pct;
    int   cpu_mhz;
    int   cpu_cores;

    float mem_pct;
    float mem_used_gb;
    float mem_total_gb;

    bool  gpu_ok;
    char  gpu_name[40];
    int   gpu_pct;
    float gpu_mem_pct;
    float gpu_mem_used_gb;
    float gpu_mem_total_gb;
    int   gpu_temp;
    float gpu_power;

    int         disk_count;
    pc_disk_t   disks[PC_MAX_DISKS];

    float net_up_kbps;
    float net_down_kbps;

    int   fail_count;         /* 连续失败次数，用来在界面上显示离线 */
    int64_t last_ok_ms;
} pc_stats_t;

/** 启动后台轮询任务（需在 WiFi 连上之前或之后调用都可以） */
esp_err_t pc_monitor_start(void);

/** 拷贝一份当前快照（线程安全） */
void pc_monitor_get(pc_stats_t *out);

/** 最近一次用的 URL，调试显示用 */
const char *pc_monitor_url(void);
