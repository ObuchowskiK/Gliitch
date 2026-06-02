#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "config.h" 

static const char *TAG = "ESP_NOW_RX";

void on_data_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
    gamepad_state_t payload;

    const uint8_t *mac = esp_now_info->src_addr;
    if (len == sizeof(gamepad_state_t)) {
        memcpy(&payload, incomingData, sizeof(gamepad_state_t));
        ESP_LOGI(TAG, "Received X: %d, Y: %d, Buttons: %d from MAC: %02X:%02X:%02X:%02X:%02X:%02X", 
                 payload.joy_x, payload.joy_y, payload.buttons,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "Received payload of unexpected length: %d bytes", len);
    }
}

void start_receiver(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Receiver MAC Address: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
             
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
}