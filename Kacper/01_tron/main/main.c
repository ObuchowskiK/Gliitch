#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "MAIN_SYS";

// --- Hardware Layout ---
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO 13
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_DC   4
#define PIN_NUM_RST  9

// --- Display Dimension Boundaries ---
#define LCD_H_RES       480
#define LCD_V_RES       320
#define CHUNK_LINES     40  
#define TARGET_FPS      60  
#define FRAME_DELAY_MS  (1000 / TARGET_FPS)
#define SPI_MHZ_SPEED   80 

#define MAX_PLAYERS 4

// --- Endian-Swapping Macros ---
#define SWAP16(val) (uint16_t)((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#define MATTE_CHARCOAL SWAP16(0x18C3)

// --- TRON GAME CONFIGURATION ---
#define TRON_SCALE 4 
#define TRON_W (LCD_H_RES / TRON_SCALE) 
#define TRON_H (LCD_V_RES / TRON_SCALE) 

// --- Communication & Thread Synchronization Links ---
QueueHandle_t xBluetoothQueue = NULL;
QueueHandle_t xFilledFrameQueue = NULL;
QueueHandle_t xEmptyFrameQueue = NULL;
SemaphoreHandle_t xDmaDoneSemaphore = NULL;

// --- Specialized Memory Blocks ---
uint16_t *pGameCanvasA = NULL;
uint16_t *pGameCanvasB = NULL;
uint16_t *pDmaStreamBuffer = NULL; 

esp_lcd_panel_io_handle_t display_io_handle = NULL;

// Data Structures
typedef struct {
    uint8_t player_id; 
    int8_t joystick_x; 
    int8_t joystick_y;
    uint8_t buttons;   // Obsługa przycisków (Bit 0 to przycisk A)
} ble_packet_t;

typedef struct {
    int16_t x;
    int16_t y;
    int8_t dir_x;
    int8_t dir_y;
    uint16_t color;
    bool alive;
} tron_player_t;

// ISR Callback dla SPI DMA
static bool IRAM_ATTR vSpiDmaDoneCallback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t xHighPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xDmaDoneSemaphore, &xHighPriorityTaskWoken);
    return xHighPriorityTaskWoken == pdTRUE;
}

void display_send_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *params, size_t param_len) {
    esp_lcd_panel_io_tx_param(io, cmd, params, param_len);
}

void display_set_window(esp_lcd_panel_io_handle_t io, uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end) {
    uint8_t col_bounds[] = {x_start >> 8, x_start & 0xFF, x_end >> 8, x_end & 0xFF};
    uint8_t row_bounds[] = {y_start >> 8, y_start & 0xFF, y_end >> 8, y_end & 0xFF};
    display_send_cmd(io, 0x2A, col_bounds, 4);
    display_send_cmd(io, 0x2B, row_bounds, 4);
}

// ============================================================================
// CORE 0: Symulator Padów Bluetooth (Sztuczny ruch do testów)
// ============================================================================
void vBluetoothTask(void *pvParameters) {
    ble_packet_t packet;
    uint32_t loop_counter = 0;

    while (1) {
        for (uint8_t id = 0; id < MAX_PLAYERS; id++) {
            packet.player_id = id;
            packet.buttons = 0;
            
            // Losujemy drobne zmiany kierunku co jakiś czas, żeby linie skręcały same w celach testowych
            packet.joystick_x = 0;
            packet.joystick_y = 0;
            
            if (loop_counter % 20 == 0) {
                int random_move = esp_random() % 4;
                if (random_move == 0) packet.joystick_x = 50;
                if (random_move == 1) packet.joystick_x = -50;
                if (random_move == 2) packet.joystick_y = 50;
                if (random_move == 3) packet.joystick_y = -50;
            }

            // Jeśli gra się zawiesi (koniec rundy), zasymuluj wciśnięcie przycisku A po 3 sekundach
            if (loop_counter % 150 == 0) {
                packet.buttons |= 0x01; 
            }

            xQueueSend(xBluetoothQueue, &packet, 0);
        }
        loop_counter++;
        vTaskDelay(pdMS_TO_TICKS(30)); 
    }
}

// =============================================================================
// ENGINE: Logika i Renderowanie Gry Tron
// =============================================================================
void vGameLogicTask(void *pvParameters) {
    ESP_LOGI(TAG, "Game Engine: TRON MODE Online.");
    TickType_t xFrameDeadline = xTaskGetTickCount();
    ble_packet_t input_payload;
    uint16_t *pCurrentDrawCanvas = NULL;

    uint8_t *grid = heap_caps_malloc(TRON_W * TRON_H, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (grid == NULL) {
        ESP_LOGE(TAG, "Brak pamięci na siatkę Trona!");
        vTaskDelete(NULL);
    }

    tron_player_t players[MAX_PLAYERS];
    bool game_over = true;
    uint32_t clear_val32 = ((uint32_t)MATTE_CHARCOAL << 16) | MATTE_CHARCOAL;

    void reset_round() {
        memset(grid, 0, TRON_W * TRON_H);
        players[0] = (tron_player_t){.x = 15,         .y = TRON_H / 2,  .dir_x = 1,  .dir_y = 0,  .color = SWAP16(0xF800), .alive = true}; // Czerwony
        players[1] = (tron_player_t){.x = TRON_W - 15, .y = TRON_H / 2,  .dir_x = -1, .dir_y = 0,  .color = SWAP16(0x07E0), .alive = true}; // Zielony
        players[2] = (tron_player_t){.x = TRON_W / 2,  .y = 15,         .dir_x = 0,  .dir_y = 1,  .color = SWAP16(0x001F), .alive = true}; // Niebieski
        players[3] = (tron_player_t){.x = TRON_W / 2,  .y = TRON_H - 15, .dir_x = 0,  .dir_y = -1, .color = SWAP16(0xFFE0), .alive = true}; // Żółty
        
        for(int i = 0; i < MAX_PLAYERS; i++) {
            grid[players[i].y * TRON_W + players[i].x] = (i + 1);
        }
        game_over = false;
    }

    reset_round();

    while (1) {
        vTaskDelayUntil(&xFrameDeadline, pdMS_TO_TICKS(FRAME_DELAY_MS));
        xQueueReceive(xEmptyFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);

        while (xQueueReceive(xBluetoothQueue, &input_payload, 0) == pdTRUE) {
            uint8_t id = input_payload.player_id;
            if (id < MAX_PLAYERS && players[id].alive) {
                if (game_over && (input_payload.buttons & 0x01)) {
                    reset_round();
                }
                if (input_payload.joystick_x > 30 && players[id].dir_x == 0) {
                    players[id].dir_x = 1;  players[id].dir_y = 0;
                } else if (input_payload.joystick_x < -30 && players[id].dir_x == 0) {
                    players[id].dir_x = -1; players[id].dir_y = 0;
                } else if (input_payload.joystick_y > 30 && players[id].dir_y == 0) {
                    players[id].dir_x = 0;  players[id].dir_y = 1;
                } else if (input_payload.joystick_y < -30 && players[id].dir_y == 0) {
                    players[id].dir_x = 0;  players[id].dir_y = -1;
                }
            }
        }

        if (!game_over) {
            int alive_count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!players[i].alive) continue;

                players[i].x += players[i].dir_x;
                players[i].y += players[i].dir_y;

                if (players[i].x < 0 || players[i].x >= TRON_W || players[i].y < 0 || players[i].y >= TRON_H) {
                    players[i].alive = false;
                    continue;
                }
                if (grid[players[i].y * TRON_W + players[i].x] != 0) {
                    players[i].alive = false;
                    continue;
                }

                grid[players[i].y * TRON_W + players[i].x] = (i + 1);
                alive_count++;
            }

            if (alive_count <= 1) {
                game_over = true;
            }
        }

        // Czyszczenie tła
        uint32_t *pCanvas32 = (uint32_t *)pCurrentDrawCanvas;
        int half_total_pixels = (LCD_H_RES * LCD_V_RES) / 2;
        for (int i = 0; i < half_total_pixels; i++) {
            pCanvas32[i] = clear_val32;
        }

        // Rysowanie planszy Tron (Skalowanie punktów gry do pikseli ekranu)
        for (int gy = 0; gy < TRON_H; gy++) {
            for (int gx = 0; gx < TRON_W; gx++) {
                uint8_t cell = grid[gy * TRON_W + gx];
                if (cell != 0) {
                    uint16_t color = players[cell - 1].color;
                    for (int dy = 0; dy < TRON_SCALE; dy++) {
                        int pixel_y = gy * TRON_SCALE + dy;
                        for (int dx = 0; dx < TRON_SCALE; dx++) {
                            int pixel_x = gx * TRON_SCALE + dx;
                            pCurrentDrawCanvas[pixel_y * LCD_H_RES + pixel_x] = color;
                        }
                    }
                }
            }
        }

        xQueueSend(xFilledFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);
    }
}

// =============================================================================
// CORE 1: Dedykowany Hardware Display Streamer (DMA)
// =============================================================================
void vDisplayRenderTask(void *pvParameters) {
    uint16_t *pCanvasToRender = NULL;
    const size_t chunk_bytes = LCD_H_RES * CHUNK_LINES * sizeof(uint16_t);

    while (1) {
        if (xQueueReceive(xFilledFrameQueue, &pCanvasToRender, portMAX_DELAY) == pdTRUE) {
            for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
                uint16_t *pSourceLine = pCanvasToRender + (y * LCD_H_RES);
                memcpy(pDmaStreamBuffer, pSourceLine, chunk_bytes);
                display_set_window(display_io_handle, 0, y, LCD_H_RES - 1, y + CHUNK_LINES - 1);
                esp_lcd_panel_io_tx_color(display_io_handle, 0x2C, pDmaStreamBuffer, chunk_bytes);
                xSemaphoreTake(xDmaDoneSemaphore, portMAX_DELAY);
            }
            xQueueSend(xEmptyFrameQueue, &pCanvasToRender, portMAX_DELAY);
        }
    }
}

// =============================================================================
// HARDWARE & OS ROOT SETUP
// =============================================================================
void app_main(void) {
    pGameCanvasA     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pGameCanvasB     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pDmaStreamBuffer = (uint16_t *)heap_caps_malloc(LCD_H_RES * CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_bus_config_t bus_configuration = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * CHUNK_LINES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_configuration, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_bus_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = SPI_MHZ_SPEED * 1000 * 1000, 
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_bus_config, &display_io_handle));

    xDmaDoneSemaphore = xSemaphoreCreateBinary();
    xBluetoothQueue   = xQueueCreate(16, sizeof(ble_packet_t));
    xFilledFrameQueue = xQueueCreate(1, sizeof(uint16_t*));
    xEmptyFrameQueue  = xQueueCreate(2, sizeof(uint16_t*));

    esp_lcd_panel_io_callbacks_t display_callbacks = { .on_color_trans_done = vSpiDmaDoneCallback };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(display_io_handle, &display_callbacks, NULL));

    xQueueSend(xEmptyFrameQueue, &pGameCanvasA, 0);
    xQueueSend(xEmptyFrameQueue, &pGameCanvasB, 0);

    display_send_cmd(display_io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120)); 
   
    uint8_t rotation_param[] = {0xE8};
    display_send_cmd(display_io_handle, 0x36, rotation_param, 1);      
   
    uint8_t data_color_depth[] = {0x55};
    display_send_cmd(display_io_handle, 0x3A, data_color_depth, 1);      
   
    display_send_cmd(display_io_handle, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Spójne uruchomienie wątków systemowych
    xTaskCreatePinnedToCore(vBluetoothTask, "BLE_Stack", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(vDisplayRenderTask, "GFX_Render", 4096, NULL, 6, NULL, 1);
    xTaskCreate(vGameLogicTask, "Game_Engine", 4096, NULL, 5, NULL);
}