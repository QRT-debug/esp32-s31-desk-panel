# ESP32-S31 开发板项目 · 长期记忆

> 工程：`C:\Users\Admin\Desktop\sample_project`
> 完整文档在 `docs/`（README / HARDWARE / PROJECT_LOG / PLAYBOOK / BOARD_AUDIO）。
> 本文件只放"下次开工必须先知道"的结论，**详细内容查 docs，不要在这里重复**。

## 一句话现状（2026-11-20）

LVGL 四页界面（HOME / PC 监控 / 音频播放器 / 系统信息+日志，状态栏常驻）+ **浅色/深色双主题**（系统页 DARK MODE 开关，NVS 记忆）+ **麦克风已打通**（16k 裸 I2S RX + 手动 ES8389 ADC 序列，录音/播放模式互斥）+ SD 卡播放 + 开机自播已删。固件 ≈1.8MB。

## 音频架构（2026-11-20 定稿）

- **旧 esp_codec_dev v1.x 回归**（新驱动修不动，退场 `Temp/esp_codec_dev_old_backup`）；组件全本地化，构建离线。
- **I2S 双速率**：REC 16k=DEFAULT 源（零抖动）/ PLAY 44.1k=APLL。**APLL 全芯片单资源且 reconfig 不释放**——分时切换必炸，只有"一边完全不碰 APLL"才安全；`audio_codec_i2s_cfg_t.clk_src` 必须显式传 APLL。
- **mic 手动 ADC 序列**（`adc_manual_init`）= 厂商 open + bias_on（0x69|=0x20、0x10=E4、0x64=0x8F）+ coeff 256 行（05=41 06=04 07=C0 08=01 09=D1）+ 0xF0=0x12 + 0x01 bit3=1。细节见 2026-11-20 日报。
- 模式互斥：`aud_enter()` 拿锁 → close 在用 → `i2s_set_rate()` → open 目标；PA(IO7) REC 态强制关。

## 铁律（踩过坑，别再犯）

1. **音源统一 44.1kHz，禁止线性插值重采样**（曾把噪声底抬高 15~33dB ⇒ 持续"雪花嘶嘶"）。`tools/mp3_to_wav.py` 现在源≠目标会直接报错。
2. **不要改 ES8389 的时钟分频到 32fs**：两次实测都输出巨大噪声（大嘶啦 + 低频嗡）。本板 codec 与 I2S 的时钟差（BCLK 32fs vs 驱动假设 64fs）只能靠补焊 MCLK（IO2→R56，标 NC）解决，收益不值得。
3. **不要用 32bit I2S slot**：IDF 的 BCLK 仍按 16bit 算，实测播放速度慢一倍（22.1kHz）。用 16bit slot。
4. **播放路径不要碰 flash**：整曲预载 PSRAM 再播（否则 fread 打断 RGB LCD DMA ⇒ 屏幕条纹 + 音频断流）。
5. **烧录提速配置**：`NO_STUB` 必须为 not set；`flash` = 1.5Mbps + stub + 增量（18 s 全流程）。
6. **抓日志先看 `pwd`**：`boot_capture.py` 用相对路径写 `boot_log.txt`，曾因此读错旧日志误判。
7. **动音频配置前先做零风险测量**：`DIAG: SINE/SILENCE` 分层切除、固件内 `DIAG RATE` 自测采样率，比盲改安全得多。

## 硬件要点（详见 docs/HARDWARE.md）

- 屏：800×480 RGB，PCLK 25MHz，`.pclk_active_neg = false`（消边缘闪的关键）；
  ★ **PCLK 必须 25MHz（11-20 定案）**：30MHz 时屏幕下方波浪状模糊（PSRAM 被 LCD DMA+PPA 抢，行消隐内取数不及=underflow），降 25 消失。25MHz×16bit=50MB/s 持续，已是 SPI 屏 80MHz(10MB/s 峰值) 的 5 倍带宽。再遇波浪先降 PCLK/加 VBP。
- 音频：ES8389(I2C 0x10) + NS4150B×2；I2S BCLK=IO3 / WS=IO4 / DOUT=IO5 / PA=IO7；
- 烧录用 **COM 口**（CH343P，COM5）；USB-A 是 Host 口；PWR 口不能传数据；
- TF 卡 = **SDMMC 4-bit**：CLK=31, CMD=32, D0~D3=27~30，**不占 MSPI**（未来放歌的首选）；
- 分区：app 1.9MB / **vfs(fat) 10MB**（音乐）/ storage 4MB（空置，可并给 vfs）。

## S31 深层硬件能力索引（写软件前对照 · soc_caps.h 实测 11-20）

触发式查表：写代码遇到场景 → 用对应硬件，别用 CPU 硬算。
- 大块内存搬运（帧缓冲/音频缓冲拷贝）→ **ASYNC_MEMCPY**（esp_async_memcpy，DMA 零 CPU）；
- 显示网络图片/JPEG → **硬件 JPEG 编解码**（esp_jpeg）；
- 图像格式重排/颜色空间转换 → **BITSCRAMBLER**（DMA 字节流重排）；
- UI 卡顿 → 先查 PPA 两行 sdkconfig（LV_USE_PPA=y + LV_DRAW_BUF_ALIGN=64，见渲染铁律）；
- 16k↔44.1k 音频互转 → 芯片有 **ASRC** 但 IDF 6.1 驱动未实现（盯 esp_driver_i2s 更新，驱动落地可替代 APLL 独占方案）；
- AI 流式/网络重任务 → **钉 core1**（xTaskCreatePinnedToCore(...,1)，与音频/渲染分核，core1 现闲置）；
- 低功耗待机监听 → **LP Core**（独立 RISC-V 小核带 I2C/UART/SPI/GPIO/Timer，esp_lp_core）；
- 无线音频双路 → **BT Classic A2DP + BLE 5.0 LE Audio(LC3)**；
- 外设间硬件事件联动（零 CPU 响应）→ **ETM**（GPIO/GDMA/I2S/MCPWM/ANA_CMPR 均支持）；
- 加密/签名 → 硬件 AES-GCM/ECC P-384/ECDSA/HMAC/KeyManager（量产加密用）；
- 硬件三角函数/矢量 → CORDIC；
- **用不上别碰**：EMAC(千兆网 MAC)、MCPWM/PCNT(电机)、IEEE802154、MIPI CSI、SDIO slave。

## 待办（按优先级）

1. ~~SD 卡播放~~（已完成：SDMMC 挂载 + 目录扫曲 + 插拔检测图标）；
2. **WiFi 按需拉取**：`pc_monitor_set_active()`，监控页不可见时停 HTTP（给蓝牙让射频）；
3. **蓝牙 A2DP Sink**：芯片支持经典蓝牙（`SOC_BT_CLASSIC_SUPPORTED=1`），但 IDF 6.1 文档说 S31 Bluedroid 仅 BLE ⇒ 必须实测，建议走 ADF/`esp_bt_audio`；
4. 整首 4 分钟歌：合并 storage(4MB)→vfs(14MB) + 22.05k，或写 ADPCM 解码器（整首 5.5MB）；
5. **AI 对话接入**：麦克风（REC 16k）常开即数据源，需 WiFi + 流式 ASR/TTS（录音/播放互斥架构已就位）。
