#include "display.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "OLED_GFX";

#define I2C_SCL 21     
#define I2C_SDA 22   
#define I2C_MASTER_NUM I2C_NUM_0 
#define I2C_HZ 400000
#define OLED_I2C_ADDRESS 0x3C    

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

static uint8_t frame_buffer[1024];

static void oled_send_command(uint8_t command) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x00, true);
    i2c_master_write_byte(cmd, command, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
}

static void oled_send_data(uint8_t *data, size_t length) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x40, true);
    i2c_master_write(cmd, data, length, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
}

void display_clear_buffer(void) {
    memset(frame_buffer, 0, sizeof(frame_buffer));
}

void display_draw_pixel(int x, int y, int color) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint16_t index = x + (y / 8) * OLED_WIDTH;
    uint8_t bit = y % 8;

    if (color) {
        frame_buffer[index] |= (1 << bit);  
    } else {
        frame_buffer[index] &= ~(1 << bit); 
    }
}

void display_draw_rect(int x, int y, int width, int height) {
    for (int i = x; i < x + width; i++) {
        for (int j = y; j < y + height; j++) {
            display_draw_pixel(i, j, 1);
        }
    }
}

void display_update_screen(void) {
    oled_send_command(0x21); 
    oled_send_command(0x00); 
    oled_send_command(127);  

    oled_send_command(0x22); 
    oled_send_command(0x00); 
    oled_send_command(0x07); 

    oled_send_data(frame_buffer, sizeof(frame_buffer));
}

void display_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);

    ESP_LOGI(TAG, "Initializing SSD1306");
    uint8_t init_cmds[] = {
        0xAE,       
        0x20, 0x00, 
        0xA8, 0x3F, 
        0xD3, 0x00,
        0x40,       
        0xA1,       
        0xC8,       
        0xDA, 0x12, 
        0x81, 0x7F, 
        0xA4,       
        0xA6,       
        0xD5, 0x80, 
        0x8D, 0x14, 
    };
    
    for (int i = 0; i < sizeof(init_cmds); i++) {
        oled_send_command(init_cmds[i]);
    }
    
    display_clear_buffer();
    display_update_screen();
    oled_send_command(0xAF); 
    ESP_LOGI(TAG, "OLED Ready");
}