idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_lcd driver esp_timer)

target_link_libraries(${COMPONENT_TARGET} PUBLIC m)