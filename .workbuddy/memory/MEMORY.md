# ESP32-S31 开发板项目 · 长期记忆

> 工程：`C:\Users\Admin\Desktop\sample_project`
> 完整文档在 `docs/`（README / HARDWARE / PROJECT_LOG / PLAYBOOK / BOARD_AUDIO）。
> 本文件只放"下次开工必须先知道"的结论，**详细内容查 docs，不要在这里重复**。

## 一句话现状（2026-11-23）

**9 个场景**（天气 HOME / PC 监控 / 音频 / 系统 / APPS / HF 热榜 / AI 榜 / B 站 / **屏保时钟**），
**浅深双主题 + 运行时配置层 + 手机配网 + 四源数据缓存**。固件 ≈2.6MB / 4MB 分区（36% 余量）。

**11-22 晚新增（这是当前最值钱的部分）**：
- **配置层** `app_cfg.{h,c}`：WiFi/城市/坐标/PC 地址/背光/时区全存 NVS 一个 blob，
  `wifi_config.h` 降级为"首次上电默认值"⇒ **换网络不用重烧**。
- **配网** `wifi_prov.{h,c}`：SoftAP + 内嵌网页表单（不依赖文件系统）+ SYSTEM 页 WIFI 按钮；
  开机 30s 连不上自动开热点。**实测配错密码能自救**。
- **数据缓存** `data_cache.{h,c}`：`wx/hf/ai/bili` 四源落 NVS，**断网/开机瞬间都有画面**
  （缓存 5.5s 读完，WiFi 要 7s 才连上）。
- **壁纸上传** `POST /upload`：4KB 缓冲边收边写 SD，已实测 6 张进 `/sdcard/wallpaper/`。
- **屏保时钟** `scene_clock.c`：SD 壁纸 + **硬件 JPEG 解码** + 96px 等宽时间。**尚未烧录验证**。

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
8. ★★★ **写 flash 会撕裂画面**：`nvs_commit()` 期间**禁用 instruction cache**，而 LVGL 代码住在 flash
   ⇒ 任务 stall ⇒ `flush_cb` 等 vsync 200ms 超时后放弃 ⇒ 画面时序错乱。表现是"点保存→屏幕花一下→重启"。
   ⇒ 所有写 NVS/SPIFFS 都放在**画面静止**时（数据已到、界面未更新）；配网保存前先 `backlight_set(0)`。
   ★ 注意 S31 的 `SOC_PSRAM_MEMSPI_IS_INDEPENDENT=1`，**不是 PSRAM/DMA 被抢**（旧结论已推翻）。
9. ★★ **`strlcpy` 不是 `snprintf`/`strncpy`**：`wifi_config_t.sta.ssid` 是 `uint8_t[32]`**不含 NUL**，
   目标比源小时前两者分别触发 `-Werror=format-truncation` / `stringop-truncation`（一晚上踩 4 次）。
10. ★★ **新增 .c 必须先 `idf.py reconfigure`**：`SRC_DIRS` 是 CMake glob，不重扫会链接报
    `undefined reference`——**长得像"函数没写"，实际是"文件没进构建"**。新加组件依赖同理（`REQUIRES` 要补）。
11. ★★ **中文标签只能用 `ui_font_cn18*`**：`ui_font_14/20/24/28/32` 是 Montserrat/Segoe ASCII 字模，
    中文渲染成方块（tofu）。★ LVGL 场景子对象坐标相对**内容区**（已扣 padding），算尺寸用 `宽-2*pad`。
12. ★ **三个 Type-C 分工**：PWR(纯供电) / **COM(CH343P，烧录+日志 COM5)** / USB-A(原生 Host，接电脑会倒灌)。
    **只插 PWR 时设备能跑但没 COM5**，别以为是串口坏了。

13. ★★★ **碰串口只用 `tools/watch_log.py`，禁止手写 SerialPort**（2026-11-25 再次复发）：
    auto-download 电路靠 DTR/RTS 翻转，`SerialPort.Open()` 瞬间 Windows 驱动默认拉高两线
    ⇒ **打开串口 = 复位设备**。正确做法：`DtrEnable=false; RtsEnable=false` 必须放在 `Open()` **之前**，
    open 之后再设已晚。2026-11-23 已记录并修好 `watch_log.py`（零扰动）；
    **11-25 我又手写 PowerShell SerialPort 读 15 秒，把用户正要拍的"画面错位"现场复位掉了**。
    ⇒ 用户正在观察某现象时**碰串口先问**；要主动复位抓启动日志用 `catch_boot`，两者分工不同。
    **教训：观察工具不能扰动被观察对象——这条 11-23 就写过，我 11-25 又犯。**

## 硬件要点（详见 docs/HARDWARE.md）

- 屏：800×480 RGB，**PCLK 40MHz / 总时序 1120×541 / 66.02fps / `.pclk_active_neg = true`**
  （11-22 定案，四症状全清：闪烁·波浪·抖动·白竖线）。
  ★ **25MHz 时代已结束**：48.5Hz 低于人眼 CFF ⇒ 矩形边缘 flicker；33/80MHz 非整数分频产生
  beat 抖动；80MHz 面板采不住（建立窗口仅 7.5ns）。**40MHz 是 160MHz 整数分频（零抖动）且
  25.0ns 周期对面板 30ns 建立时间仍有余量**，DMA 占用 63.4%、`render` 23ms < 26ms 预算。
  ★ `pclk_active_neg=true` 不是风格选择：`true` 让面板在上升沿采样，对齐建立/保持窗口；
  设 false 会出**白竖线**（实测）。
  ★ 别再用"降 PCLK 治波浪"——那条结论来自 25MHz 之前，已被 40MHz 全清推翻。
- 音频：ES8389(I2C 0x10) + NS4150B×2；I2S BCLK=IO3 / WS=IO4 / DOUT=IO5 / PA=IO7；
- 烧录用 **COM 口**（CH343P，COM5）；USB-A 是 Host 口；PWR 口不能传数据；
- TF 卡 = **SDMMC 4-bit**：CLK=31, CMD=32, D0~D3=27~30，**不占 MSPI**（未来放歌的首选）；
- 分区（`partitions-16MiB.csv` 实际值）：**app 4MB**(0x10000) / **vfs(fat) 3MB**(0x410000) /
  **storage(spiffs) 2MB**(0x710000) / **coredump 128KB**(0x910000)。app 现用 2.6MB、余 36%。

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

1. **烧录验证屏保时钟**（`scene_clock.c` 已写、编译通过、**没烧过**）——缺 COM 口。
2. **时钟页加天气**（现在只有时间和日期）+ **无触摸 5 分钟自动进** + **每 4h 偏移防烧屏**；
   屏保开关写进 `app_cfg`。★ 壁纸比纯黑更伤屏（此屏 VCOM 未校准），必须轮换+偏移。
3. **WiFi 按需拉取**：`pc_monitor_set_active()`，监控页不可见时停 HTTP（给蓝牙让射频）；
4. **蓝牙 A2DP Sink**：芯片支持经典蓝牙（`SOC_BT_CLASSIC_SUPPORTED=1`），但 IDF 6.1 文档说 S31 Bluedroid 仅 BLE ⇒ 必须实测，建议走 ADF/`esp_bt_audio`；
5. **OTA**：分区表只有 factory，且 app 2.6MB/4MB，切双 OTA 后每份 2MB 装不下 ⇒ 要先减肥（字体是刀口）。
6. 整首 4 分钟歌：合并 storage(4MB)→vfs(14MB) + 22.05k，或写 ADPCM 解码器（整首 5.5MB）；
7. **AI 对话接入**：麦克风（REC 16k）常开即数据源，需 WiFi + 流式 ASR/TTS。
