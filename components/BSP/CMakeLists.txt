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
set(LCD_PCLK_MHZ 33 CACHE STRING "RGB LCD pixel clock in MHz" FORCE)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LCD_PCLK_MHZ=${LCD_PCLK_MHZ})
