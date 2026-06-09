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
} ble_packet_t;

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t color;
} player_state_t;

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
// CORE 0: Bluetooth Network Stack Driver
// ============================================================================
void vBluetoothTask(void *pvParameters) {
    ESP_LOGI(TAG, "Multiplayer Bluetooth Parser Online on Core %d", xPortGetCoreID());
    ble_packet_t packet;

    while (1) {
        for (uint8_t id = 0; id < MAX_PLAYERS; id++) {
            packet.player_id = id;
            if (id == 0) { packet.joystick_x = 2;  packet.joystick_y = 0;  } 
            if (id == 1) { packet.joystick_x = -2; packet.joystick_y = 0;  } 
            if (id == 2) { packet.joystick_x = 0;  packet.joystick_y = 2;  } 
            if (id == 3) { packet.joystick_x = 1;  packet.joystick_y = 1;  } 
            xQueueSend(xBluetoothQueue, &packet, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(30)); 
    }
}

// =============================================================================
// UNPINNED: Game Logic Core Engine (Asynchronous Frame Coordinator)
// =============================================================================
void vGameLogicTask(void *pvParameters) {
    ESP_LOGI(TAG, "Game Engine Task online (Unpinned). Working Core: %d", xPortGetCoreID());
    TickType_t xFrameDeadline = xTaskGetTickCount();
    ble_packet_t input_payload;
    uint16_t *pCurrentDrawCanvas = NULL;

    player_state_t players[MAX_PLAYERS] = {
        { .x = 50,  .y = 60,  .color = SWAP16(0xF800) }, // Red
        { .x = 400, .y = 60,  .color = SWAP16(0x07E0) }, // Green
        { .x = 220, .y = 20,  .color = SWAP16(0x001F) }, // Blue
        { .x = 100, .y = 200, .color = SWAP16(0xFFE0) }  // Yellow
    };

    uint32_t clear_val32 = ((uint32_t)MATTE_CHARCOAL << 16) | MATTE_CHARCOAL;

    while (1) {
        vTaskDelayUntil(&xFrameDeadline, pdMS_TO_TICKS(FRAME_DELAY_MS));

        // 1. Pull an available safely recycled canvas buffer
        xQueueReceive(xEmptyFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);

        // 2. Flush incoming Bluetooth inputs
        while (xQueueReceive(xBluetoothQueue, &input_payload, 0) == pdTRUE) {
            uint8_t p_id = input_payload.player_id;
            if (p_id < MAX_PLAYERS) {
                players[p_id].x += input_payload.joystick_x;
                players[p_id].y += input_payload.joystick_y;
                
                if (players[p_id].x >= LCD_H_RES) players[p_id].x = 0;
                if (players[p_id].x < 0) players[p_id].x = LCD_H_RES - BOX_SIZE;
                if (players[p_id].y >= LCD_V_RES) players[p_id].y = 0;
                if (players[p_id].y < 0) players[p_id].y = LCD_V_RES - BOX_SIZE;
            }
        }

        // 3. Clear Background Canvas using optimized 32-bit width memory steps
        uint32_t *pCanvas32 = (uint32_t *)pCurrentDrawCanvas;
        int half_total_pixels = (LCD_H_RES * LCD_V_RES) / 2;
        for (int i = 0; i < half_total_pixels; i++) {
            pCanvas32[i] = clear_val32;
        }

        // 4. Render all active player positions directly into the canvas
        for (int p = 0; p < MAX_PLAYERS; p++) {
            for (int row = players[p].y; row < (players[p].y + BOX_SIZE); row++) {
                for (int col = players[p].x; col < (players[p].x + BOX_SIZE); col++) {
                    if (col >= 0 && col < LCD_H_RES && row >= 0 && row < LCD_V_RES) {
                        pCurrentDrawCanvas[row * LCD_H_RES + col] = players[p].color;
                    }
                }
            }
        }

        // 5. Dispatch filled canvas to the Core 1 renderer
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

        // 2. Monitor Core & Task States
        if (pcTaskListBuffer != NULL) {
            vTaskList(pcTaskListBuffer);
            ESP_LOGI("MONITOR", "\nTask Name\tState\tPrio\tStack\tNum\n%s", pcTaskListBuffer);
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
    xBluetoothQueue   = xQueueCreate(16, sizeof(ble_packet_t));
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

    // 7. System Thread Spawning
    xTaskCreatePinnedToCore(vBluetoothTask, "BLE_Stack", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(vDisplayRenderTask, "GFX_Render", 4096, NULL, 6, NULL, 1);
    xTaskCreate(vGameLogicTask, "Game_Engine", 4096, NULL, 5, NULL);
    xTaskCreatePinnedToCore(vSystemMonitorTask, "Sys_Monitor", 4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "All processes deployed successfully.");
}