set(src_dirs
            LCD
            LED
            SDMMC
            MYI2S
            MYES8389
            MYIIC
            TOUCH
            )

set(include_dirs
            LCD
            LED
            SDMMC
            MYI2S
            MYES8389
            MYIIC
            TOUCH
            )

set(requires
            esp_driver_gpio
            esp_lcd
            led_strip
            esp_driver_sdmmc
            sdmmc
            fatfs
            esp_driver_i2s
            esp_driver_i2c
            esp_codec_dev
            )

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)