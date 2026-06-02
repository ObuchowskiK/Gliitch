#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "config.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"

#define JOY_X_ADC_CHANNEL ADC_CHANNEL_0 // GP0 (Physical Pin 4)
#define JOY_Y_ADC_CHANNEL ADC_CHANNEL_1 // GP1 (Physical Pin 5)

// Digital Buttons
#define BTN_WHITE_PIN  4  // GP4 (Physical Pin 8)
#define BTN_BLACK_PIN  5  // GP5 (Physical Pin 9)
#define BTN_YELLOW_PIN 6  // GP6 (Physical Pin 10)
#define BTN_RED_PIN    7  // GP7 (Physical Pin 11)

// --- HARDWARE INITIALIZATION ---
adc_oneshot_unit_handle_t adc_handle;

void init_hardware() {
    // 1. Initialize Buttons
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL<<BTN_WHITE_PIN) | (1ULL<<BTN_RED_PIN) | 
                        (1ULL<<BTN_YELLOW_PIN) | (1ULL<<BTN_BLACK_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1, // Assume active-low (buttons pull to GND)
    };
    gpio_config(&io_conf);
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1, 
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12, 
        .atten = ADC_ATTEN_DB_12,    
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_X_ADC_CHANNEL, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, JOY_Y_ADC_CHANNEL, &config));
}
static const char *TAG = "GAMEPAD_TX";
uint8_t receiver_mac[6] = {0xE0, 0x8C, 0xFE, 0x5D, 0xE7, 0x34}; 

void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGE(TAG, "Packet Delivery Fail");
    }
}

void start_transmitter(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo)); 
    memcpy(peerInfo.peer_addr, receiver_mac, 6);
    peerInfo.channel = 1;      
    peerInfo.encrypt = false;  

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer");
        return;
    }

    init_hardware();
    ESP_LOGI(TAG, "Gamepad Ready. Transmitting...");

    gamepad_state_t state;
    int raw_x, raw_y;

    while (1) {
        adc_oneshot_read(adc_handle, JOY_X_ADC_CHANNEL, &raw_x);
        adc_oneshot_read(adc_handle, JOY_Y_ADC_CHANNEL, &raw_y);
        state.joy_x = (uint16_t)raw_x;
        state.joy_y = (uint16_t)raw_y;

        state.buttons = 0;
        if (gpio_get_level(BTN_WHITE_PIN) == 0) state.buttons |= (1 << 0);
        if (gpio_get_level(BTN_RED_PIN) == 0)   state.buttons |= (1 << 1);
        if (gpio_get_level(BTN_YELLOW_PIN) == 0) state.buttons |= (1 << 2);
        if (gpio_get_level(BTN_BLACK_PIN) == 0)  state.buttons |= (1 << 3);

        esp_now_send(receiver_mac, (uint8_t *) &state, sizeof(state));

        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}