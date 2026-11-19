# ESP32-S31 开发板项目 · 文档总览

> 这块板子（慧勤智远 **ESP32-S31 CB V1.1**，与乐鑫 **Korvo-1** 同构）的全部记录集中在这里。
> 代码工程 = `C:\Users\Admin\Desktop\sample_project`。与"ESP32-S3 墨水屏自绘硬件"项目**完全独立**。

## 文档导航

| 文件 | 内容 |
|---|---|
| **HARDWARE.md** | 硬件事实：芯片规格、引脚分配、屏/触摸/音频/TF 卡/USB/电源、分区表 |
| **PROJECT_LOG.md** | 开发日志与踩坑史：屏幕条纹、音频嘶嘶、烧录提速、UI 重构的全过程与结论 |
| **PLAYBOOK.md** | 操作手册：编译、烧录、抓串口、换歌、打包镜像、诊断按钮用法 |
| **BOARD_AUDIO.md** | 音频链路专项核查（ES8389 寄存器级、I2S 引脚、坑点） |
| `REFERENCES/` | 原理图渲染图与文本层、Korvo 参考工程、顺芯原厂 Linux 驱动、裁剪脚本 |
| `../logs/` | 历史调试日志（build/flash/serial 输出），排查时有对照价值 |

## 一句话现状（2026-11-17 凌晨）

- 屏幕：800×480 RGB，双 framebuffer，50 FPS 稳定，播放音频时**无条纹**；
- 音频：ES8389 + NS4150B，44.1kHz/16bit，可播放 FAT 分区里的 WAV（当前 2 首，8.64MB/10MB）；
- 界面：LVGL 9.6，HOME 选择器（PC 监控 / 音频播放器）+ 常驻 WiFi 信号格；
- 网络：WiFi STA 每秒拉取 PC 状态（`tools/pc_monitor_server.py`）；
- 已知遗留：DIAG 诊断按钮仍在音频页（8 档）；整首 4 分钟的歌放不进 10MB 分区。

## 下一步待办（按优先级）

1. **SD 卡播放**：TF 卡座 = SDMMC 4-bit（IO27~32），独立控制器不抢 MSPI，是"正路"；
2. **WiFi 按需拉取**：只在监控页可见时拉数据，把射频让给蓝牙音频；
3. **蓝牙 A2DP Sink**：芯片支持经典蓝牙（`SOC_BT_CLASSIC_SUPPORTED=1`），需先验证 IDF 6.1 的栈；
4. 整首歌：合并 storage 4MB 给 vfs（14MB）+ 22.05k，或写 ADPCM 解码器。
