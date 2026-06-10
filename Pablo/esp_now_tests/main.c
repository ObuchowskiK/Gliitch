#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "esp_timer.h"

static const char *TAG = "MAIN_SYS";

// --- Hardware Layout ---
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO 13
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_DC   4
#define PIN_NUM_RST  9

// --- Display Dimension Boundaries ---
// ST7796 controller.
#define LCD_H_RES       480
#define LCD_V_RES       320
#define CHUNK_LINES     40  //base is 40, 320 lines / 40 = 8 perfect drawing blocks
#define TARGET_FPS      60   // stable at 30 but can experiment
#define FRAME_DELAY_MS  (1000 / TARGET_FPS)
#define SPI_MHZ_SPEED 80 // 40 is stable but laggy


#define MAX_PLAYERS 4
#define BOX_SIZE 30

// --- Endian-Swapping Macros ---
#define SWAP16(val) (uint16_t)((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#define MATTE_CHARCOAL SWAP16(0x18C3)

#define NEON_CYAN      SWAP16(0x07FF)
#define NEON_PINK      SWAP16(0xF81F)
#define PURE_WHITE     SWAP16(0xFFFF)
#define TEXT_GRAY      SWAP16(0x7BEF)

// --- Communication & Thread Synchronization Links ---
QueueHandle_t xGamepadQueue = NULL;
QueueHandle_t xFilledFrameQueue = NULL;
QueueHandle_t xEmptyFrameQueue = NULL;
SemaphoreHandle_t xDmaDoneSemaphore = NULL;

// --- Specialized Memory Blocks ---
uint16_t *pGameCanvasA = NULL;
uint16_t *pGameCanvasB = NULL;
uint16_t *pDmaStreamBuffer = NULL; 

uint64_t input_rx_timestamp[MAX_PLAYERS] = {0};
uint64_t final_latency_us[MAX_PLAYERS] = {0};

esp_lcd_panel_io_handle_t display_io_handle = NULL;

// game state machine - Pawel
typedef enum {
    STATE_MAIN_MENU,
    STATE_GAME_BRICKS,
    STATE_PAD_SETTINGS
} console_state_t;
// Global variable to leader pad rights
console_state_t current_state = STATE_MAIN_MENU;
uint8_t leader_player_id = 1;

// Paweł Data Structures changed due to expanding status
typedef struct {
    uint8_t player_id;  
    uint16_t joy_x;
    uint16_t joy_y;
    uint8_t buttons;
} __attribute__((packed)) gamepad_state_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t color;
    bool is_active;
    uint32_t last_seen;
} player_state_t;

player_state_t players[MAX_PLAYERS] = {
        { .x = 50,  .y = 60,  .color = SWAP16(0xF800), .is_active = true, .last_seen = 0 }, // Red
        { .x = 400, .y = 60,  .color = SWAP16(0x07E0), .is_active = true, .last_seen = 0 }, // Green
        { .x = 220, .y = 20,  .color = SWAP16(0x001F), .is_active = true, .last_seen = 0 }, // Blue
        { .x = 100, .y = 200, .color = SWAP16(0xFFE0), .is_active = true, .last_seen = 0 }  // Yellow
    };


    
// mock font for menu subtitles
const uint8_t font8x8[][8] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['>'] = {0x00,0x44,0x22,0x11,0x22,0x44,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
    [':'] = {0x00,0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00},
    ['-'] = {0x00,0x00,0x00,0x3e,0x00,0x00,0x00,0x00},
    ['1'] = {0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00},
    ['2'] = {0x38,0x44,0x04,0x08,0x10,0x20,0x7c,0x00},
    ['3'] = {0x3c,0x04,0x04,0x1c,0x04,0x04,0x3c,0x00},
    ['4'] = {0x08,0x18,0x28,0x48,0x7c,0x08,0x08,0x00},
    ['A'] = {0x18,0x24,0x42,0x42,0x7e,0x42,0x42,0x00},
    ['B'] = {0x7c,0x42,0x42,0x7c,0x42,0x42,0x7c,0x00},
    ['C'] = {0x3c,0x42,0x40,0x40,0x40,0x42,0x3c,0x00},
    ['D'] = {0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00},
    ['E'] = {0x7e,0x40,0x40,0x7c,0x40,0x40,0x7e,0x00},
    ['F'] = {0x7e,0x40,0x40,0x7c,0x40,0x40,0x40,0x00},
    ['G'] = {0x3c,0x42,0x40,0x4e,0x42,0x42,0x3c,0x00},
    ['H'] = {0x42,0x42,0x42,0x7e,0x42,0x42,0x42,0x00},
    ['I'] = {0x3e,0x08,0x08,0x08,0x08,0x08,0x3e,0x00},
    ['L'] = {0x40,0x40,0x40,0x40,0x40,0x40,0x7e,0x00},
    ['M'] = {0x42,0x66,0x5a,0x42,0x42,0x42,0x42,0x00},
    ['N'] = {0x42,0x62,0x52,0x4a,0x46,0x42,0x42,0x00},
    ['O'] = {0x3c,0x42,0x42,0x42,0x42,0x42,0x3c,0x00},
    ['P'] = {0x7c,0x42,0x42,0x7c,0x40,0x40,0x40,0x00},
    ['R'] = {0x7c,0x42,0x42,0x7c,0x48,0x44,0x42,0x00},
    ['S'] = {0x3c,0x42,0x40,0x3c,0x02,0x42,0x3c,0x00},
    ['T'] = {0x7e,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
    ['U'] = {0x42,0x42,0x42,0x42,0x42,0x42,0x3c,0x00},
    ['V'] = {0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00},
    ['W'] = {0x42,0x42,0x42,0x42,0x5a,0x66,0x42,0x00},
    ['X'] = {0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00},
    ['Y'] = {0x42,0x42,0x24,0x18,0x08,0x08,0x08,0x00},
};

void canvas_draw_text(uint16_t *canvas, int x, int y, const char *text, uint16_t color, int scale) {
    while (*text) {
        uint8_t c = (uint8_t)*text;
        if (c < (sizeof(font8x8) / sizeof(font8x8[0]))) {
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 8; col++) {
                    if (font8x8[c][row] & (1 << (7 - col))) {
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int out_x = x + (col * scale) + sx;
                                int out_y = y + (row * scale) + sy;
                                if (out_x >= 0 && out_x < LCD_H_RES && out_y >= 0 && out_y < LCD_V_RES) {
                                    canvas[out_y * LCD_H_RES + out_x] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
        x += 8 * scale; 
        text++;
    }
}
// ISR Callback triggered automatically when an SPI DMA chunk completes transmission
static bool IRAM_ATTR vSpiDmaDoneCallback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t xHighPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xDmaDoneSemaphore, &xHighPriorityTaskWoken);
    return xHighPriorityTaskWoken == pdTRUE;
}

// Push control commands directly over the display bus
void display_send_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *params, size_t param_len) {
    esp_lcd_panel_io_tx_param(io, cmd, params, param_len);
}

// Slice out window view coordinates over the display bus
void display_set_window(esp_lcd_panel_io_handle_t io, uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end) {
    uint8_t col_bounds[] = {x_start >> 8, x_start & 0xFF, x_end >> 8, x_end & 0xFF};
    uint8_t row_bounds[] = {y_start >> 8, y_start & 0xFF, y_end >> 8, y_end & 0xFF};
    display_send_cmd(io, 0x2A, col_bounds, 4);
    display_send_cmd(io, 0x2B, row_bounds, 4);
}

// ============================================================================
// CORE 0: ESP-Now Network Stack Driver
// ============================================================================
void on_esp_now_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    if (len == sizeof(gamepad_state_t)) {
        gamepad_state_t incoming_input;
        memcpy(&incoming_input, data, sizeof(gamepad_state_t));uint8_t p_id = incoming_input.player_id;
        if (p_id >= 1 && p_id <= MAX_PLAYERS) {
            input_rx_timestamp[p_id - 1] = esp_timer_get_time();
        }
        // Push data to Core 1 without blocking the Wi-Fi task
        xQueueSend(xGamepadQueue, &incoming_input, (TickType_t)0);
    }
}

// =============================================================================
// UNPINNED: Game Logic Core Engine (Asynchronous Frame Coordinator)
// =============================================================================
void vGameLogicTask(void *pvParameters) {
    ESP_LOGI(TAG, "Game Engine Task online (Unpinned). Working Core: %d", xPortGetCoreID());
    TickType_t xFrameDeadline = xTaskGetTickCount();
    gamepad_state_t input_payload;
    uint16_t *pCurrentDrawCanvas = NULL;
    int menu_selection = 0;
    uint32_t last_menu_move = 0;

    uint32_t clear_val32 = ((uint32_t)MATTE_CHARCOAL << 16) | MATTE_CHARCOAL;

    while (1) {
        vTaskDelayUntil(&xFrameDeadline, pdMS_TO_TICKS(FRAME_DELAY_MS));

        // 1. Pobranie wolnego bufora z kolejki
        xQueueReceive(xEmptyFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);

        // PRZYWRÓCONE: Czyszczenie tła całego bufora przed rysowaniem nowej klatki
        uint32_t *pCanvas32 = (uint32_t *)pCurrentDrawCanvas;
        int half_total_pixels = (LCD_H_RES * LCD_V_RES) / 2;
        for (int i = 0; i < half_total_pixels; i++) {
            pCanvas32[i] = clear_val32;
        }

        // 2. Odbiór danych z padów przez ESP-NOW
        while (xQueueReceive(xGamepadQueue, &input_payload, 0) == pdTRUE) {
            uint8_t p_id = input_payload.player_id;
            if (p_id >= 1 && p_id <= MAX_PLAYERS) {
                int idx = p_id - 1;
                players[idx].is_active = true;
                players[idx].last_seen = xTaskGetTickCount();

                // --- STAN: ROZGRYWKA ---
                if (current_state == STATE_GAME_BRICKS) {
                    if (input_payload.joy_x < 1800) players[idx].x -= 3;
                    if (input_payload.joy_x > 2300) players[idx].x += 3;
                    if (input_payload.joy_y < 1800) players[idx].y -= 3;
                    if (input_payload.joy_y > 2300) players[idx].y += 3;
                    if (players[idx].x >= LCD_H_RES) players[idx].x = 0;
                    if (players[idx].x < 0) players[idx].x = LCD_H_RES - BOX_SIZE;
                    if (players[idx].y >= LCD_V_RES) players[idx].y = 0;
                    if (players[idx].y < 0) players[idx].y = LCD_V_RES - BOX_SIZE;
                    
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3))) {
                        current_state = STATE_MAIN_MENU;
                    }
                }

                // --- STAN: GŁÓWNE MENU ---
                if (current_state == STATE_MAIN_MENU && p_id == leader_player_id) {
                    if (xTaskGetTickCount() - last_menu_move > pdMS_TO_TICKS(220)) {
                        if (input_payload.joy_y > 2500) { menu_selection = (menu_selection + 1) % 2; last_menu_move = xTaskGetTickCount(); }
                        if (input_payload.joy_y < 1500) { menu_selection = (menu_selection - 1 + 2) % 2; last_menu_move = xTaskGetTickCount(); }
                    }
                    if (input_payload.buttons & (1 << 0)) {
                        if (menu_selection == 0) current_state = STATE_GAME_BRICKS;
                        if (menu_selection == 1) current_state = STATE_PAD_SETTINGS;
                    }
                }

                // --- STAN: USTAWIENIA PADÓW ---
                if (current_state == STATE_PAD_SETTINGS) {
                    if (input_payload.buttons & (1 << 3)) {
                        current_state = STATE_MAIN_MENU;
                    } 
                    if (input_payload.buttons & (1 << 1)) {
                        leader_player_id = p_id;
                    }
                } 
            }
        } 

        // 3. Monitor rozłączeń (Timeout)
        uint32_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].is_active && (now - players[i].last_seen) * portTICK_PERIOD_MS > 2000) {
                players[i].is_active = false;
            }
        }

        // 4. Kompozytor warstwy graficznej UI
        if (current_state == STATE_MAIN_MENU) {
            canvas_draw_text(pCurrentDrawCanvas, 160, 40, "GLIITCH", NEON_PINK, 4);
            canvas_draw_text(pCurrentDrawCanvas, 120, 100, "SYSTEM MAIN MENU", TEXT_GRAY, 2);

            if (menu_selection == 0) {
                canvas_draw_text(pCurrentDrawCanvas, 100, 160, "> 1. BRICK MULTIPLAYER", NEON_CYAN, 2);
                canvas_draw_text(pCurrentDrawCanvas, 100, 200, "  2. SETTINGS", PURE_WHITE, 2);
            } else {
                canvas_draw_text(pCurrentDrawCanvas, 100, 160, "  1. BRICK MULTIPLAYER", PURE_WHITE, 2);
                canvas_draw_text(pCurrentDrawCanvas, 100, 200, "> 2. SETTINGS", NEON_CYAN, 2);
            }

            char leader_str[30];
            sprintf(leader_str, "CURRENT LEADER: PAD %d", leader_player_id);
            canvas_draw_text(pCurrentDrawCanvas, 110, 270, leader_str, NEON_PINK, 2);
        } 
        else if (current_state == STATE_PAD_SETTINGS) {
            canvas_draw_text(pCurrentDrawCanvas, 136, 20, "SETTINGS", NEON_CYAN, 3);
            
            char status_str[40];
            for (int i = 0; i < MAX_PLAYERS; i++) {
                char *state_lbl = players[i].is_active ? "ACTIVE" : "DISCONNECTED";
                uint16_t lbl_color = players[i].is_active ? NEON_CYAN : TEXT_GRAY;
                
                if ((i + 1) == leader_player_id) {
                    sprintf(status_str, "PAD %d: %s [LEADER]", i + 1, state_lbl);
                    lbl_color = NEON_PINK;
                } else {
                    sprintf(status_str, "PAD %d: %s", i + 1, state_lbl);
                }
                canvas_draw_text(pCurrentDrawCanvas, 60, 80 + (i * 35), status_str, lbl_color, 2);
            }

            canvas_draw_text(pCurrentDrawCanvas, 40, 250, "PRESS RED TO SET AS CONSOLE LEADER", PURE_WHITE, 1);
            canvas_draw_text(pCurrentDrawCanvas, 40, 280, "PRESS BLACK TO RETURN TO MENU", TEXT_GRAY, 1);
        } 
        else if (current_state == STATE_GAME_BRICKS) {
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (players[p].is_active) {
                    for (int row = players[p].y; row < (players[p].y + BOX_SIZE); row++) {
                        for (int col = players[p].x; col < (players[p].x + BOX_SIZE); col++) {
                            if (col >= 0 && col < LCD_H_RES && row >= 0 && row < LCD_V_RES) {
                                pCurrentDrawCanvas[row * LCD_H_RES + col] = players[p].color;
                            }
                        }
                    }
                    if ((p + 1) == leader_player_id) {
                        canvas_draw_text(pCurrentDrawCanvas, players[p].x + 11, players[p].y - 12, "L", PURE_WHITE, 1);
                    }
                }
            }
        }

        // 5. Wysłanie gotowej klatki do potoku renderowania Core 1
        xQueueSend(xFilledFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);
    }
}

// =============================================================================
// CORE 1: Dedicated Hardware Display Streamer
// =============================================================================
void vDisplayRenderTask(void *pvParameters) {
    ESP_LOGI(TAG, "Display DMA Renderer Pipeline activated on Core %d", xPortGetCoreID());
    
    uint16_t *pCanvasToRender = NULL;
    const int chunk_pixels = LCD_H_RES * CHUNK_LINES;
    const size_t chunk_bytes = chunk_pixels * sizeof(uint16_t);

    while (1) {
        if (xQueueReceive(xFilledFrameQueue, &pCanvasToRender, portMAX_DELAY) == pdTRUE) {
            
            for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
                uint16_t *pSourceLine = pCanvasToRender + (y * LCD_H_RES);
                
                // Fast execution block copy into Internal SRAM DMA Region
                memcpy(pDmaStreamBuffer, pSourceLine, chunk_bytes);
                
                display_set_window(display_io_handle, 0, y, LCD_H_RES - 1, y + CHUNK_LINES - 1);
                
                // Push data to display
                esp_lcd_panel_io_tx_color(display_io_handle, 0x2C, pDmaStreamBuffer, chunk_bytes);
                
                xSemaphoreTake(xDmaDoneSemaphore, portMAX_DELAY);
            }
            uint64_t frame_done_time = esp_timer_get_time();
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active && input_rx_timestamp[i] != 0) {
                    // Calculate total microseconds from radio interrupt to frame complete
                    final_latency_us[i] = frame_done_time - input_rx_timestamp[i];
                }
            }
            // Return the finished buffer back to the empty queue for reuse
            xQueueSend(xEmptyFrameQueue, &pCanvasToRender, portMAX_DELAY);
        }
    }
}

// =============================================================================
// HARDWARE & OPERATING SYSTEM ROOT SETUP
// =============================================================================

//DEBUG
void vSystemMonitorTask(void *pvParameters) {
    char *pcTaskListBuffer = malloc(512); 

    while (1) {
        ESP_LOGI("MONITOR", "--- SYSTEM VITAL SIGNS ---");
        
        // 1. Monitor Heap Memory Pools
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        
        ESP_LOGI("MONITOR", "Free PSRAM (Canvas Memory): %zu bytes", free_psram);
        ESP_LOGI("MONITOR", "Free Internal RAM: %zu bytes", free_internal);
        ESP_LOGI("MONITOR", "--- INPUT-TO-SCREEN LATENCY ---");
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].is_active) {
                float latency_ms = (float)final_latency_us[i] / 1000.0f;
                // Add 1.0ms to account for the physical ESP-NOW packet flight time
                ESP_LOGI("MONITOR", " Pad %d Turnaround Lag: %.2f ms", i + 1, latency_ms + 1.0f);
            } else {
                ESP_LOGI("MONITOR", " Pad %d: DISCONNECTED", i + 1);
            }
        }
        // 2. Monitor Core & Task States
        if (pcTaskListBuffer != NULL) {
            //vTaskList(pcTaskListBuffer);
            //ESP_LOGI("MONITOR", "\nTask Name\tState\tPrio\tStack\tNum\n%s", pcTaskListBuffer);
        }

        ESP_LOGI("MONITOR", "--------------------------\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}



void app_main(void) {
    ESP_LOGI(TAG, "System Architecture Bootstrapping Beginning...");

    // 1. Allocate Double Frame Buffers in PSRAM and DMA Streamer in Internal SRAM
    pGameCanvasA     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pGameCanvasB     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pDmaStreamBuffer = (uint16_t *)heap_caps_malloc(LCD_H_RES * CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (pGameCanvasA == NULL || pGameCanvasB == NULL || pDmaStreamBuffer == NULL) {
        ESP_LOGE(TAG, "Heap Exhaustion Core Allocation Failure!");
        return;
    }

    // 2. Hardware Line Display Reset Sequence
    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    // 3. SPI Hardware Bus Configuration
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

    // 4. Connect Native Abstract Display IO Layers
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

    // 5. Instantiate Synchronization Interlocks
    xDmaDoneSemaphore = xSemaphoreCreateBinary();
    xGamepadQueue   = xQueueCreate(32, sizeof(gamepad_state_t));
    xFilledFrameQueue = xQueueCreate(1, sizeof(uint16_t*));
    xEmptyFrameQueue  = xQueueCreate(2, sizeof(uint16_t*));

    // Register DMA Complete Tracking Callbacks directly inside panel IO driver configurations
    esp_lcd_panel_io_callbacks_t display_callbacks = {
        .on_color_trans_done = vSpiDmaDoneCallback
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(display_io_handle, &display_callbacks, NULL));

    xQueueSend(xEmptyFrameQueue, &pGameCanvasA, 0);
    xQueueSend(xEmptyFrameQueue, &pGameCanvasB, 0);

    // 6. Native Low-Level Startup Sequence 
    display_send_cmd(display_io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120)); 
   
    uint8_t rotation_param[] = {0xE8};
    display_send_cmd(display_io_handle, 0x36, rotation_param, 1);      
   
    uint8_t data_color_depth[] = {0x55};
    display_send_cmd(display_io_handle, 0x3A, data_color_depth, 1);      
   
    display_send_cmd(display_io_handle, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // 7. Initialize NVS (Required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 8. Initialize Wi-Fi and ESP-NOW
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));

    xTaskCreatePinnedToCore(vDisplayRenderTask, "GFX_Render", 4096, NULL, 6, NULL, 1);
    xTaskCreate(vGameLogicTask, "Game_Engine", 4096, NULL, 5, NULL);
    xTaskCreatePinnedToCore(vSystemMonitorTask, "Sys_Monitor", 4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "All processes deployed successfully.");
}
