# 操作手册（照抄即可）

> 工程根目录：`C:\Users\Admin\Desktop\sample_project`
> 本文所有命令都在 **PowerShell** 里执行，先 `cd` 到工程根，再 `. .\idf_env.ps1` 载入环境。

## 0. 一次性准备

```powershell
Set-Location "C:\Users\Admin\Desktop\sample_project"
. .\idf_env.ps1        # 载入 IDF 环境 + 定义 flash / appflash 函数
```

## 1. 编译 / 烧录

| 场景 | 命令 | 耗时（实测） |
|---|---|---|
| 只编译 | `python $env:IDF_PATH\tools\idf.py build` | ~15 s（增量） |
| **编译 + 烧录** | `flash` | 18 s（改一行代码后全流程） |
| 只烧 app（跳过 bootloader/分区表） | `appflash` | 更快约 1 s |
| 全量烧录 | `flash`（内容变了才全写） | 12.6 s |

- 波特率固定 **1.5 Mbps**（`ESPBAUD`），已验证稳定；2 Mbps 更快但不稳，不用；
- **烧录中别按板上 RESET**，会切断 USB 串口报 `No more data to read`；
- 增量烧录依赖 `build/*_flashed.bin` 记录；**手动用 esptool 烧过之后，下一次会自动退回全量**（正常现象，不是故障）。

## 2. 看实时进度（长任务时）

- 我执行长任务时会把输出写进 `live.log`；
- 双击 `tools\watch_live.bat` 弹出滚动窗口，能看到 `ninja [1234/2087]` 这类进度；
- 每个新任务会重建 `live.log`，窗口里看不到新内容就关掉重开。

## 3. 抓串口日志

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" "tools\boot_capture.py"
```

- 脚本会拉 DTR 复位板子并抓约 18 秒，输出写到**当前工作目录**的 `boot_log.txt`；
- ⚠️ 用相对路径，**先确认当前目录**（历史上曾因此读错日志，误判"新代码没跑"）；
- ⚠️ 刚烧完立刻抓会报 `PermissionError`（端口没释放），等 2 秒再抓。

## 4. 音频文件（换歌）

### 4.1 源文件要求

- **必须是 44.1kHz**（与固件 `AUD_SAMPLE_RATE` 一致）；
- **禁止重采样**：脚本里如果源采样率≠目标会直接报错退出（这是刻意的，线性插值会把噪声底抬高 15~33dB）；
- 单声道/立体声都行，固件会把 mono 展开成立体声。

### 4.2 转码

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" "tools\mp3_to_wav.py"
```
脚本里改 `convert("xxx.mp3", "xxx.wav", max_seconds=60, start_seconds=100)` 即可截取任意片段。

### 4.3 打包 FAT 镜像并烧到 0x200000

```powershell
$t = "C:\Users\Admin\Desktop\sample_project\tools"
# 1) 把 WAV 复制到 fat_root，文件名用 8.3 短名（如 NEWSONG.WAV）
# 2) 生成 10MB 镜像
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" `
  "D:\esp\.espressif\v6.1\esp-idf\components\fatfs\fatfsgen.py" `
  "$t\fat_root" --output_file "$t\music_fat.bin" --partition_size 0xA00000 `
  --sector_size 4096 --sectors_per_cluster 1 --use_default_datetime
# 3) 烧写（约 80 秒）
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool --chip esp32s31 `
  -p COM5 -b 1500000 --before default_reset --after hard_reset `
  write_flash 0x200000 "$t\music_fat.bin"
```

- 分区 10MB，现有 2 首占 8.64MB；**换歌请同时删除旧文件**，否则会超；
- 文件名必须是 **8.3 短名**（固件没开 LFN）；
- 加歌后要同步改 `components/ui/src/scene_audio.c` 的 `k_tracks[] / k_files[]` 和 `audio_player.c` 的 `s_cur_file` 默认值。

## 5. 音频诊断按钮（DIAG，音频页播放圆右侧）

点一次切一档：

| 档位 | 含义 | 读法 |
|---|---|---|
| `DIAG: OFF` | 正常 | — |
| `DIAG: MUTE` | DAC 数字静音、PA 开 | 还有嘶 = 功放/电源；干净 = codec 输出侧 |
| `DIAG: PA OFF` | 关功放 | 应完全安静 |
| `DIAG: ALL OFF` | 都关 | 全静音对照 |
| `DIAG: SILENCE` | 数据全 0、通路全开 | 有嘶 = 通路自噪 |
| `DIAG: SINE 1k` | 1kHz 正弦（-20dBFS） | 有嘶 = 信号相关失真 |
| `DIAG: SINE 8k` | 8kHz 正弦 | 测高频通路 |
| `DIAG: RAM MUSIC` | 把缓存前 1 秒搬到内部 RAM 循环播 | 有嘶 = 与 PSRAM 无关 |

> 需要先播过一次歌，`RAM MUSIC` 才有缓存可用。

## 6. PC 监控端

```powershell
cd C:\Users\Admin\Desktop\sample_project\tools
python pc_monitor_server.py          # 或双击 启动监控.bat
```
- 需要 `psutil` + `pynvml`；
- 首次要放行防火墙 8080：`New-NetFirewallRule -DisplayName "PC Monitor 8080" -Direction Inbound -LocalPort 8080 -Protocol TCP -Action Allow`；
- 板子端目标地址在 `components/netapp/include/wifi_config.h`（`PC_MONITOR_HOST/PORT`、`WIFI_SSID/PASS`）。

## 7. 常用排查清单（"改了没生效"怎么办）

按顺序查，**先物理后逻辑**：

1. 配置真写进文件了吗（`grep` 原文）；
2. 该重编的编了吗（`build/**/*.obj` 的 mtime vs 源文件）；
3. **烧进去了吗**：看串口启动日志里的版本指纹/新加的日志；必要时 `read_flash` 回读与 `build/*.bin` 逐字节比对；
4. 才是逻辑对不对。

## 8. 坑位速查

| 坑 | 症状 | 对策 |
|---|---|---|
| `SRC_DIRS` 不递归 | 新组件源码没被编译，链接 undefined | 目录要写到 `src` 那一级 |
| `.ps1` 里写中文注释 | PowerShell 5.1 按 GBK 解析，代码被注释吞掉 | 脚本注释一律英文 |
| `Edit` 锚点用行首片段 | 会吃掉下一行 | 锚在上一行行尾 |
| 抓日志用相对路径 | 读到旧目录的旧日志，误判 | 先 `pwd`，或改用绝对路径 |
| 32fs 时钟配置 | 大嘶啦 + 低频"嗡" | **别试**，见 PROJECT_LOG 阶段七 |
| 32bit slot | 播放速度慢一倍（22.1kHz） | 用 16bit slot |
| 线性插值重采样 | 持续"雪花嘶嘶" | 源=目标采样率直通，或抗混叠重采样 |
