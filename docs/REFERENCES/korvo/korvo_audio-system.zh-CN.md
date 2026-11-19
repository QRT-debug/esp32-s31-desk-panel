# 音频系统

板载音频输出链路：

```text
ESP32-S31
    |
    | I2S STD TX
    | BCLK GPIO3
    | WS   GPIO4
    | DOUT GPIO5
    v
ES8389 DAC
    |
    v
NS4150 功放
    |
    v
喇叭
```

控制总线：

```text
I2C SDA GPIO0
I2C SCL GPIO1
```

功放控制：

```text
GPIO7 -> PA_CTRL
高电平 -> 功放使能
低电平 -> 功放关闭
```

ES8389 地址：

```text
7-bit I2C 地址:   0x10
8-bit write 地址: 0x20
```

已验证可播放的配置：

```text
I2S mode:    STD TX
Sample rate: 48 kHz
Data width:  16-bit
Channels:    stereo
MCLK:        disabled
ES8389:      slave mode
PA_CTRL:     active high
```

手写 I2S 播放案例里故意关闭 MCLK，这是对齐本板已验证的 GMF board manager 配置。
