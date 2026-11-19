# 板载音频硬件核查（原理图实测）

> 来源：开发板原理图 `ESP32-S31 CB V1.1`（4 页，东莞市慧明智远科技有限公司，2026-09-17）逐页读图。
> 等级：**原理图实测**（一手，读的就是这份图），非通电实测。

## 结论

这是一块**完整音频板**（Korvo-1 同构）：ES8389 立体声 codec + 双 NS4150B 功放 + 双模拟 MEMS 麦克风，
I2S 全双工 5 根线全部引出到模组，**能同时做「网络放音」和「麦克风采集上行」**（对讲 / 网络音箱 / AirPlay 类都行）。

## 器件

| 位号 | 型号 | 功能 |
|---|---|---|
| U3 | **ES8389**（QFN-24） | 2×ADC + 2×DAC，SNR 103 dB，支持 8~192 kHz / 24 bit，I2S 主或从 |
| U4 / U5 | **NS4150B** ×2 | D 类 3W 功放，模拟输入，BTL 输出 → SPKL / SPKR |
| MIC1 / MIC2 | **LMA3729T381-OAS02** | 模拟 MEMS 麦克风（单端接成伪差分） |
| U6 | RT9013-33V | SYS_5V → AUDIO_3V3（codec 模拟电源） |
| SPKL / SPKR | 2P 排针 | 喇叭座（不是端子，需插线） |

## 引脚分配（原理图 page-1 活体）

| 信号 | ESP32 GPIO | 模组脚号 | 串联元件 | 方向 |
|---|---|---|---|---|
| I2S_MCLK | **IO2** | 6 | R61 0R | ESP32 → codec pin4（另有 R56 标 NC 的可选跳线） |
| I2S_SCLK (BCLK) | **IO3** | 7 | R59 0R | ESP32 → codec pin8 |
| I2S_LRCK (WS) | **IO4** | 10 | R52 0R | ESP32 → codec pin10 |
| I2S_DSDIN | **IO5** | 11 | R53 0R | **ESP32 DOUT → codec DAC 输入**（pin9） |
| I2S_SDOUT | **IO6** | 12 | R16 0R | **codec ADC 输出 → ESP32 DIN**（pin11 ASDOUT） |
| PA_CTRL | **IO7** | 13 | — | ESP32 → U4/U5 CTRL，**高有效**（R11 10k 下拉，默认静音） |
| I2C SDA | **IO0** | 8 | — | CDATA + TP_SDA + DVP_SDA 共用 |
| I2C SCL | **IO1** | 9 | — | CCLK + TP_SCL + DVP_SCL 共用 |

codec I2C：**7 bit 0x10 / 8 bit 写 0x20**（AD0 经 R20 标 NC + R22 100k 下拉 = 0；AD1 经 R21 标 NC + R23 100k = 0）。

## 三个坑（都踩得上）

1. **方向陷阱**：`DSDIN` / `ASDOUT` 是**从 codec 角度命名**的。
   ESP32 侧 `dout` 要接 **IO5**、`din` 接 **IO6**。接反 = 只能放不能录（或反之），示波器看时钟全正常，很难查。
2. **麦克风通道交叉**：MIC1 模块 → `ES_MIC2P/2N`，MIC2 模块 → `ES_MIC1P/1N`。
   即**物理 1 号麦走 codec 的 2 通道**。写双麦代码时左右声道要对调。
3. **共用 I2C 总线**：codec + GT1151Q 触摸 + 摄像头 DVP 同挂 IO0/IO1。
   codec 初始化/调音量时别和触摸任务抢总线（BSP 的 `i2c_bus` 已有互斥锁，走它就行）。

## 别人验证过的播放配置（二手实测，Korvo-1 bringup 仓库）

```
I2S mode      : STD TX
采样率        : 48 kHz
位宽          : 16 bit，立体声
MCLK          : disabled（codec slave 模式，靠 BCLK 派生）
PA_CTRL       : active high
```

- 官方 Notes 提到 ES8389 与 SoC 之间的数据「需以 TDM 格式传输」，但 bringup 仓库实测 STD TX 可出声。
  ⇒ 播放先按 STD 试；**采集（RX）若拿不到数据，再切 TDM / 检查 slot 位宽**。
- esp_codec_dev 已有 ES8389 驱动（寄存器序列被第三方 crate 引用为参照），可直接用。

## 与本项目的冲突点

- 音频 DMA 和 RGB LCD 抢同一条 MSPI/PSRAM 通道 ⇒ **放音时边缘频闪会比现在更明显**。
  缓解：音频缓冲区放**内部 RAM**（`MALLOC_CAP_INTERNAL`），别放 PSRAM；屏刷新降帧。
- 功放 5V 取自 SYS_5V 经 FB5，音量开大时 5V 会有纹波 ⇒ 与 USB 供电能力有关，外接 5V 电源更稳。
