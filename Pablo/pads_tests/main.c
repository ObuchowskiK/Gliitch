#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "config.h"

#define IS_SENDER 1

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    #if IS_SENDER
        start_transmitter();
    #else
        start_receiver();
    #endif
}