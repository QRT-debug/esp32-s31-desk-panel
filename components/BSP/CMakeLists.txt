set(src_dirs
            LCD
            I2C
            TOUCH
            AUDIO/src
            STORAGE/src
            LED/src
            )

set(include_dirs
            LCD
            I2C
            TOUCH
            AUDIO/include
            STORAGE/include
            LED/include
            )

set(requires
            esp_driver_gpio
            esp_driver_i2c
            esp_driver_i2s
            esp_driver_ledc
            esp_driver_rmt
            esp_lcd
            esp_codec_dev
            led_strip
            fatfs
            sdmmc
            vfs
            esp_timer
            esp_mm          # esp_cache_msync：首屏清黑后把帧缓冲写回 PSRAM
            )

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)

# RGB 屏像素时钟（MHz）。
#   25 = 商家验证值（理论 48.5fps）
#   31 = 60.2fps，33 = 64.0fps —— 均超商家验证值，用于探"白条"出现的边界
# 改法：idf.py -DLCD_PCLK_MHZ=30 build    或直接改下面的默认值
# ★ 必须带 FORCE：CACHE 变量一旦写进 build/CMakeCache.txt，改这里的默认值不会生效，
#   会出现"明明改了 20，烧进去日志还显示 25"的情况。
#
# ★★★ 11-22 改 25 → 33：**验"边缘 flicker 是帧率型"这个假设**。
#   用户确证症状 = "矩形四个边缘在闪，每次不一定哪几个边，很快数不清"（内容完全静止）。
#   ⇒ 边缘像素无相邻像素、寄生电容与中心不同，VCOM 偏差在中心看不出、在边缘最明显
#     = 典型 VCOM flicker；"很快数不清"= 40~60Hz 量级，正好是当前 48.5Hz 帧率。
#   ⇒ 反向证据：之前实测 10MHz（19.4fps）"更闪且失真"，20/16MHz 也只是"没变好"
#     —— **帧率越低越闪**，符合 flicker 而非信号完整性问题。
#
#   33MHz → 976×528 总周期 = 64.0fps，越过人眼临界闪烁频率（60~70Hz）。
#   若边缘闪明显减轻 ⇒ 确诊 VCOM/帧率型，正式值就定 33（并接受轻微超规格）。
#   若出现白条/花屏（超规格）⇒ 退 31MHz（60.2fps）。
#   若完全无变化 ⇒ 与帧率无关，回到硬件（面板 VCOM 可调电阻 / 数据线端接）。
#
# ★★★ 11-22 二次修正 33 → **32**：33MHz 虽把 flicker 治住了，但文字边缘出现**波浪形**。
#   根因不在"超规格"，而在**分频比不是整数**：
#     时钟源 = PLL160M（160MHz）
#       160 / 25 = 6.40   （商家值，同样小数分频）
#       160 / 33 = 4.8485 ← 33MHz 实际是小数分频近似
#       160 / 32 = **5.00  整数分频，PCLK 周期严格均匀**
#   ESP32 LCD 的分频器是 integer + numerator/denominator，像 4.8485 这种会被近似成
#   相邻周期在 4/5 个源时钟之间跳变 ⇒ PCLK 抖动 ≈ ±6ns，对 30ns 周期是 20%
#   ⇒ 采样时刻不稳 ⇒ 文字/竖线边缘呈波浪。
#   32MHz → 62.09fps，仍高于 60Hz 临界；且是 160 的 1/5，**零抖动**。
#
# ★★★ 11-22 三次修正 → **40**：上面的"32 是 160 的 1/5"是**错的**，看错了对象。
#   IDF 的 lcd_hal_cal_pclk_freq() 固定**从 mo=2 起步**（源码注释：
#   "due to some unstable hardware issue, we prefer to start with mo=2 first"），
#   真正的公式是  **pixel_clk = 160MHz / (n + b/a) / mo**，mo 恒为 2。
#   所以要看的是"160 / (2×PCLK)"是不是整数，不是"160 / PCLK"：
#       25   → lcd_clk 50    → 160/50  = 3.20   小数
#       32   → lcd_clk 64    → 160/64  = 2.50   小数（每 2 个像素交替 25/37.5ns，±20%）
#       33   → lcd_clk 66    → 160/66  = 2.424  小数
#       40   → lcd_clk 80    → 160/80  = 2.00   **整数，零抖动**
#       26.67→ lcd_clk 53.33 → 160/53.3= 3.00   **整数** (备选，帧率仅 51.7fps 不够)
#   ⇒ 既零抖动又高帧率的只有 **40MHz**（976×528 = 77.6fps）。
#
#   ★ 这次同时是一刀切的实验：40MHz 抖动最低、但 PSRAM 瞬时需求最高(80MB/s)。
#     若波浪消失 ⇒ 主因是 PCLK 抖动；若加重 ⇒ 主因是 FIFO 欠载(PSRAM 供数不足)。
#   ★ 若面板采不住 40MHz（竖纹/花屏）⇒ 退回 32 并接受波浪，或走 26.67+缩 porch 路线。
#
# ★★★ **已定案（实测通过）**：40MHz 能跑，但必须同时满足两个配套条件，缺一不可 ——
#   ① porch 撑到 **1120×541**（否则帧率 77.6Hz，有效区占 74.5%，DMA 连轴转 ⇒ render 49ms、画面抖）
#      ★ V_total 的取值决定帧率，而帧率下限由**亮度**决定：CFF 随亮度上升，
#        100% 背光下 62Hz 会闪、64Hz 勉强、**66Hz（541）才有余量**。
#   ② `pclk_active_neg = **true**`（false 时右侧出现 1~2px 白竖线，行尾采错）
#   三者齐了才无闪、无波浪、无竖线。详细因果链见 docs/HARDWARE.md 第 2 节与 PROJECT_LOG 11-22。
set(LCD_PCLK_MHZ 40 CACHE STRING "RGB LCD pixel clock in MHz" FORCE)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LCD_PCLK_MHZ=${LCD_PCLK_MHZ})
