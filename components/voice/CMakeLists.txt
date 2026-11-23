idf_component_register(
    SRC_DIRS     "src"
    INCLUDE_DIRS "include"
    REQUIRES     esp_http_client nvs_flash esp_timer BSP netapp
)
