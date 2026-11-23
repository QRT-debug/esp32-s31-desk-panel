idf_component_register(
    SRC_DIRS     "src"
    INCLUDE_DIRS "include"
    REQUIRES     esp_wifi esp_http_client esp_timer esp_netif mbedtls esp-tls nvs_flash esp_http_server BSP espressif__zlib
)
