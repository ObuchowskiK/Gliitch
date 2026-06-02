#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "display.h"


QueueHandle_t gamepad_queue;

typedef struct {
    uint16_t joy_x;
    uint16_t joy_y;
    uint8_t buttons;
} __attribute__((packed)) gamepad_state_t;

void on_esp_now_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    if (len == sizeof(gamepad_state_t)) {
        gamepad_state_t incoming_input;
        memcpy(&incoming_input, data, sizeof(gamepad_state_t));
        // Push data to Core 1 without blocking the Wi-Fi task
        xQueueSend(gamepad_queue, &incoming_input, (TickType_t)0);
    }
}

void game_engine_task(void *pvParameters) {
    display_init();
    float player_x = 64.0;
    float player_y = 32.0;
    int player_size = 4;
    int food_x = rand() % 120;
    int food_y = rand() % 60;

    gamepad_state_t input = {2048, 2048, 0}; 

    while (1) {
        xQueueReceive(gamepad_queue, &input, 0);
        if (input.joy_x < 1800) player_x -= 1.5;
        if (input.joy_x > 2300) player_x += 1.5;
        if (input.joy_y < 1800) player_y -= 1.5;
        if (input.joy_y > 2300) player_y += 1.5;
        if (player_x < 0) player_x = 0;
        if (player_x > 128 - player_size) player_x = 128 - player_size;
        if (player_y < 0) player_y = 0;
        if (player_y > 64 - player_size) player_y = 64 - player_size;
        if (abs((int)player_x - food_x) < player_size && abs((int)player_y - food_y) < player_size) {
            player_size += 2; 
            if(player_size > 20) player_size = 20; 
            food_x = rand() % (128 - 4);
            food_y = rand() % (64 - 4);
        }
        display_clear_buffer();
        display_draw_rect(food_x, food_y, 2, 2);
        display_draw_rect((int)player_x, (int)player_y, player_size, player_size);
        display_update_screen();
        
        // 50 FPS Cap
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    gamepad_queue = xQueueCreate(5, sizeof(gamepad_state_t));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));
    xTaskCreatePinnedToCore(
        game_engine_task,  
        "GameEngine",      
        4096,              
        NULL,              
        5,                 
        NULL,              
        1                  
    );
}