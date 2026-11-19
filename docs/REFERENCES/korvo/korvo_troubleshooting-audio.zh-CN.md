# 音频排障

如果喇叭没有声音，建议按下面顺序检查。

## 1. PA_CTRL

```text
播放时 GPIO7 / PA_CTRL 应该是 3.3 V。
```

PA_CTRL 为低电平时，NS4150 功放关闭。

## 2. I2S 信号

已验证的 48 kHz stereo 16-bit STD I2S 路径下：

```text
GPIO4 WS/LRCLK: 约 48 kHz
GPIO3 BCLK:     约 1.536 MHz
GPIO5 DOUT:     持续变化的音频数据
GPIO2 MCLK:     已验证路径下不输出
```

## 3. ES8389 I2C

扫描 I2C 总线：

```text
ES8389 应该在 7-bit 地址 0x10 应答。
```

原理图中可能标 `0x20`，这是 8-bit write 地址。

## 4. 已知踩坑

早期手写版本尝试过 TDM、4 slot、16 kHz 和 MCLK 输出。示波器能看到 I2S 时钟，但 ES8389 LOUT/ROUT 没有音频波形。

已验证可工作的路径是：

```text
I2S STD TX
关闭 MCLK
ES8389 slave
PA_CTRL 高电平使能
```

最小参考工程是 `examples/05-es8389-i2s-playback`。
