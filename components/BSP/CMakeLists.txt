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
#   30 = 58.2fps，33 = 64.0fps —— 均超商家验证值，用于探"白条"出现的边界
# 改法：idf.py -DLCD_PCLK_MHZ=30 build    或直接改下面的默认值
# ★ 必须带 FORCE：CACHE 变量一旦写进 build/CMakeCache.txt，改这里的默认值不会生效，
#   会出现"明明改了 20，烧进去日志还显示 25"的情况。
# 实测 25 / 20 / 16 MHz 对边缘闪无影响 ⇒ 不是时间余量问题，恢复 25 保证帧率。
set(LCD_PCLK_MHZ 25 CACHE STRING "RGB LCD pixel clock in MHz" FORCE)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LCD_PCLK_MHZ=${LCD_PCLK_MHZ})
