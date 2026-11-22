# ★ SRC_DIRS 不是递归的：src/fonts 下生成的 lv_font_*.c 必须单独列出来，
#   否则链接期报 undefined reference to `ui_font_XX`。
idf_component_register(
    SRC_DIRS     "src" "src/fonts"
    INCLUDE_DIRS "include"
    REQUIRES     BSP netapp esp_timer nvs_flash lvgl__lvgl esp_mm esp_driver_jpeg   # esp_mm: esp_cache_msync（帧缓冲 cache 回写）
)
