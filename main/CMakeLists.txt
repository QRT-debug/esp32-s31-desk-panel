idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP ui netapp voice nvs_flash esp_timer)
