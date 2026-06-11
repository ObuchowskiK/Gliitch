#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h> // NIEZBĘDNE DLA FIZYKI (sqrtf, sinf)
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
#include "esp_random.h"

// --- PEANUT-GB EMULATOR DEFINES ---
#define ENABLE_SOUND 0
#define ENABLE_LCD   1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0 
#include "peanut_gb.h"
#include "rom.h"

static const char *TAG = "MAIN_SYS";

// --- Hardware Layout ---
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO 13
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_DC   4
#define PIN_NUM_RST  9

// --- Display Boundaries & Config ---
#define LCD_H_RES       480
#define LCD_V_RES       320
#define CHUNK_LINES     40  
#define TARGET_FPS      60   
#define FRAME_DELAY_MS  (1000 / TARGET_FPS)
#define SPI_MHZ_SPEED   80 
#define MAX_PLAYERS     4
#define BOX_SIZE        30

// --- Kolory (RGB565) ---
#define SWAP16(val) (uint16_t)((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#define MATTE_CHARCOAL SWAP16(0x18C3)
#define NEON_CYAN      SWAP16(0x07FF)
#define NEON_PINK      SWAP16(0xF81F)
#define PURE_WHITE     SWAP16(0xFFFF)
#define TEXT_GRAY      SWAP16(0x7BEF)
#define GRASS_GREEN    SWAP16(0x1B44) 
#define FIELD_LINE     SWAP16(0x8C71) 
#define ICE_BLUE       SWAP16(0x867F)
#define TRAP_RED       SWAP16(0xF800)

// --- System State Machine ---
typedef enum {
    STATE_MAIN_MENU,
    STATE_GAME_BRICKS,   
    STATE_GAME_TRON,
    STATE_HAXBALL_LOBBY, 
    STATE_GAME_HAXBALL,  
    STATE_PAD_SETTINGS,
    STATE_GAME_GB,
    STATE_GAME_TOWER
} console_state_t;

console_state_t current_state = STATE_MAIN_MENU;
uint8_t leader_player_id = 1;

// --- Data Structures ---
typedef struct {
    uint8_t player_id;  
    uint16_t joy_x;
    uint16_t joy_y;
    uint8_t buttons;
} __attribute__((packed)) gamepad_state_t;

typedef struct {
    int16_t x, y;
    uint16_t color;
    bool is_active;
    uint32_t last_seen;
} player_state_t;

// TRON Data
#define TRON_SCALE 4 
#define TRON_W (LCD_H_RES / TRON_SCALE) 
#define TRON_H (LCD_V_RES / TRON_SCALE) 
#define TRON_MAX_TAIL 100

typedef struct {
    int16_t x, y;
    int8_t dir_x, dir_y;
    int8_t last_dir_x, last_dir_y;
    bool alive;
    uint16_t tail_x[TRON_MAX_TAIL];
    uint16_t tail_y[TRON_MAX_TAIL];
    uint16_t tail_head, tail_len;
} tron_player_data_t;

// HAXBALL Data
typedef struct {
    float x, y;
    float vx, vy;
    float radius;
    float mass;
    bool is_kicking;
    uint16_t color;
} hb_entity_t;

// ICY TOWER Data
#define TOWER_MAX_PLATFORMS 24
typedef struct {
    float x, y;
    float vx, vy;
    bool is_alive;
    bool is_jumping;
} tower_player_t;

typedef struct {
    float x, y;
    float width;
    int type; // 0 = Normal, 1 = Trap, 2 = Trampoline
} tower_platform_t;

uint8_t *pTronGrid = NULL;
tron_player_data_t tron_players[MAX_PLAYERS];
uint8_t tron_lives[MAX_PLAYERS] = {3, 3, 3, 3}; 
bool tron_game_over = true;
bool tron_tournament_over = false;
int tron_winner_id = -1;

int8_t hb_teams[MAX_PLAYERS] = {-1, -1, -1, -1};
hb_entity_t hb_players[MAX_PLAYERS];
hb_entity_t hb_ball;
int hb_score_left = 0;
int hb_score_right = 0;
int hb_match_state = 0; 
int hb_last_conceded = -1; 
uint32_t hb_timer = 0;

tower_player_t tw_players[MAX_PLAYERS];
tower_platform_t tw_platforms[TOWER_MAX_PLATFORMS];
float tw_camera_y = 0;
float tw_auto_scroll = 0.5f;
bool tw_game_over = true;
int tw_winner_id = -1;
int tw_highest_score = 0;

// --- Global Variables ---
QueueHandle_t xGamepadQueue = NULL;
QueueHandle_t xFilledFrameQueue = NULL;
QueueHandle_t xEmptyFrameQueue = NULL;
SemaphoreHandle_t xDmaDoneSemaphore = NULL;

uint16_t *pGameCanvasA = NULL;
uint16_t *pGameCanvasB = NULL;
uint16_t *pDmaStreamBuffer = NULL; 

uint64_t input_rx_timestamp[MAX_PLAYERS] = {0};
uint64_t final_latency_us[MAX_PLAYERS] = {0};
esp_lcd_panel_io_handle_t display_io_handle = NULL;

player_state_t players[MAX_PLAYERS] = {
    { .x = 50,  .y = 60,  .color = SWAP16(0xF800), .is_active = false, .last_seen = 0 },
    { .x = 400, .y = 60,  .color = SWAP16(0x07E0), .is_active = false, .last_seen = 0 },
    { .x = 220, .y = 20,  .color = SWAP16(0xFFE0), .is_active = false, .last_seen = 0 },
    { .x = 100, .y = 200, .color = SWAP16(0x001F), .is_active = false, .last_seen = 0 }
};

// --- Mock Font Character Map ---
const uint8_t font8x8[][8] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['>'] = {0x00,0x44,0x22,0x11,0x22,0x44,0x00,0x00},
    ['-'] = {0x00,0x00,0x00,0x3e,0x00,0x00,0x00,0x00},
    ['0'] = {0x3c,0x46,0x4a,0x52,0x62,0x42,0x3c,0x00},
    ['1'] = {0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00},
    ['2'] = {0x38,0x44,0x04,0x08,0x10,0x20,0x7c,0x00},
    ['3'] = {0x3c,0x04,0x04,0x1c,0x04,0x04,0x3c,0x00},
    ['4'] = {0x08,0x18,0x28,0x48,0x7c,0x08,0x08,0x00},
    ['5'] = {0x7c,0x40,0x78,0x04,0x04,0x44,0x38,0x00},
    ['6'] = {0x3c,0x40,0x7c,0x42,0x42,0x42,0x3c,0x00},
    ['7'] = {0x7e,0x02,0x04,0x08,0x10,0x20,0x40,0x00},
    ['8'] = {0x3c,0x42,0x3c,0x42,0x42,0x42,0x3c,0x00},
    ['9'] = {0x3c,0x42,0x42,0x42,0x3e,0x02,0x3c,0x00},
    ['A'] = {0x18,0x24,0x42,0x42,0x7e,0x42,0x42,0x00},
    ['B'] = {0x7c,0x42,0x42,0x7c,0x42,0x42,0x7c,0x00},
    ['C'] = {0x3c,0x42,0x40,0x40,0x40,0x42,0x3c,0x00},
    ['D'] = {0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00},
    ['E'] = {0x7e,0x40,0x40,0x7c,0x40,0x40,0x7e,0x00},
    ['F'] = {0x7e,0x40,0x40,0x7c,0x40,0x40,0x40,0x00},
    ['G'] = {0x3c,0x42,0x40,0x4e,0x42,0x42,0x3c,0x00},
    ['H'] = {0x42,0x42,0x42,0x7e,0x42,0x42,0x42,0x00},
    ['I'] = {0x3e,0x08,0x08,0x08,0x08,0x08,0x3e,0x00},
    ['K'] = {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00},
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
    [':'] = {0x00,0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00},
};

// --- Helper Functions ---
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

// Draw the GLIITCH logo letter-by-letter with per-letter glitch control.
// glitch_mask: bit N set = that letter is hidden (shows blank/noise instead)
// glitch_offset: array of per-letter Y pixel offsets for scanline-shift effect
void canvas_draw_logo_gliitch(uint16_t *canvas, int base_x, int base_y,
                               uint8_t glitch_mask, int8_t *glitch_offset,
                               uint16_t *glitch_colors) {
    const char *logo = "GLIITCH";
    int letter_scale = 4;
    int letter_w = 8 * letter_scale;

    for (int li = 0; li < 7; li++) {
        char c = logo[li];
        int lx = base_x + li * letter_w;
        int ly = base_y + (glitch_offset ? glitch_offset[li] : 0);
        uint16_t col = glitch_colors ? glitch_colors[li] : NEON_PINK;

        // If this letter is glitched out, draw noise bars instead
        if (glitch_mask & (1 << li)) {
            // Draw a short horizontal static bar in place of the letter
            for (int row = 0; row < 8 * letter_scale; row++) {
                for (int col_px = 0; col_px < letter_w; col_px++) {
                    int px = lx + col_px;
                    int py = ly + row;
                    if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES) {
                        // Alternating cyan/pink noise lines
                        uint16_t noise = ((row + col_px + li) % 3 == 0) ? NEON_CYAN :
                                         ((row + col_px) % 2 == 0)       ? NEON_PINK :
                                                                            MATTE_CHARCOAL;
                        canvas[py * LCD_H_RES + px] = noise;
                    }
                }
            }
        } else {
            // Draw the letter normally with its current color
            uint8_t ci = (uint8_t)c;
            if (ci < (sizeof(font8x8) / sizeof(font8x8[0]))) {
                for (int row = 0; row < 8; row++) {
                    for (int px_col = 0; px_col < 8; px_col++) {
                        if (font8x8[ci][row] & (1 << (7 - px_col))) {
                            for (int sy = 0; sy < letter_scale; sy++) {
                                for (int sx = 0; sx < letter_scale; sx++) {
                                    int out_x = lx + (px_col * letter_scale) + sx;
                                    int out_y = ly + (row   * letter_scale) + sy;
                                    if (out_x >= 0 && out_x < LCD_H_RES && out_y >= 0 && out_y < LCD_V_RES) {
                                        canvas[out_y * LCD_H_RES + out_x] = col;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void draw_circle(uint16_t *canvas, int cx, int cy, int r, uint16_t color, bool outline_only, uint16_t outline_color) {
    int r2 = r * r;
    int in_r2 = (r-2) * (r-2);
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int dist2 = x*x + y*y;
            if (dist2 <= r2) {
                int px = cx + x;
                int py = cy + y;
                if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES) {
                    if (outline_only && dist2 < in_r2) continue; 
                    if (!outline_only && dist2 > in_r2) canvas[py * LCD_H_RES + px] = outline_color; 
                    else canvas[py * LCD_H_RES + px] = color; 
                }
            }
        }
    }
}

// =============================================================================
// EMULATOR SETUP (Peanut-GB)
// =============================================================================
#define GB_LCD_WIDTH  160
#define GB_LCD_HEIGHT 144
#define GB_SCALE      2   

static struct gb_s gb;
static uint8_t  *gb_rom_data  = NULL;   
static uint8_t  *gb_cart_ram  = NULL;   
static uint16_t *gb_framebuf  = NULL;   

static const uint16_t gb_palette[3][4] = {
    { SWAP16(0xE7FC), SWAP16(0xA534), SWAP16(0x528A), SWAP16(0x0000) },
    { SWAP16(0xE7FC), SWAP16(0xA534), SWAP16(0x528A), SWAP16(0x0000) },
    { SWAP16(0xE7FC), SWAP16(0xA534), SWAP16(0x528A), SWAP16(0x0000) },
};

uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) { return gb_rom_data[addr]; }
uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) { return gb_cart_ram ? gb_cart_ram[addr] : 0xFF; }
void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) { if (gb_cart_ram) gb_cart_ram[addr] = val; }

void gb_lcd_draw_line_cb(struct gb_s *gb, const uint8_t pixels[160], const uint_fast8_t line) {
    if (!gb_framebuf) return;
    for (int x = 0; x < GB_LCD_WIDTH; x++) {
        uint8_t palette_idx = (pixels[x] & 0x03);
        uint8_t palette_num = (pixels[x] >> 4) & 0x03;
        uint16_t color = gb_palette[palette_num < 3 ? palette_num : 0][palette_idx];
        int dst_y = line * GB_SCALE;
        int dst_x = x * GB_SCALE;
        int buf_w = GB_LCD_WIDTH * GB_SCALE;
        gb_framebuf[dst_y       * buf_w + dst_x    ] = color;
        gb_framebuf[dst_y       * buf_w + dst_x + 1] = color;
        gb_framebuf[(dst_y + 1) * buf_w + dst_x    ] = color;
        gb_framebuf[(dst_y + 1) * buf_w + dst_x + 1] = color;
    }
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val) { ESP_LOGE(TAG, "Peanut-GB error %d val=0x%04X", gb_err, val); }

void save_gb_ram_to_nvs() {
    if (!gb_cart_ram) return;
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("gb_save", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(my_handle, "cart_ram", gb_cart_ram, 0x8000);
        if (err == ESP_OK) nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void load_gb_ram_from_nvs() {
    if (!gb_cart_ram) return;
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("gb_save", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        size_t required_size = 0;
        err = nvs_get_blob(my_handle, "cart_ram", NULL, &required_size);
        if (err == ESP_OK && required_size == 0x8000) {
            nvs_get_blob(my_handle, "cart_ram", gb_cart_ram, &required_size);
        }
        nvs_close(my_handle);
    }
}

bool gb_init_emulator(void) {
    gb_rom_data = (uint8_t *)heap_caps_malloc(pokemon_gb_len, MALLOC_CAP_SPIRAM);
    gb_cart_ram = (uint8_t *)heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM);
    gb_framebuf = (uint16_t *)heap_caps_malloc(GB_LCD_WIDTH * GB_SCALE * GB_LCD_HEIGHT * GB_SCALE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!gb_rom_data || !gb_cart_ram || !gb_framebuf) return false;
    
    memcpy(gb_rom_data, pokemon_gb, pokemon_gb_len);
    memset(gb_cart_ram, 0xFF, 0x8000);
    load_gb_ram_from_nvs();
    memset(gb_framebuf, 0, GB_LCD_WIDTH * GB_SCALE * GB_LCD_HEIGHT * GB_SCALE * sizeof(uint16_t));
    
    if (gb_init(&gb, gb_rom_read_cb, gb_cart_ram_read_cb, gb_cart_ram_write_cb, gb_error, NULL) != GB_INIT_NO_ERROR) return false;
    gb_init_lcd(&gb, gb_lcd_draw_line_cb);
    return true;
}

void gb_free_emulator(void) {
    if (gb_rom_data) { heap_caps_free(gb_rom_data); gb_rom_data = NULL; }
    if (gb_cart_ram) { heap_caps_free(gb_cart_ram); gb_cart_ram = NULL; }
    if (gb_framebuf) { heap_caps_free(gb_framebuf); gb_framebuf = NULL; }
}

// =============================================================================
// HAXBALL PHYSICS
// =============================================================================
void resolve_circle_collision(hb_entity_t *a, hb_entity_t *b, float bounciness) {
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float dist = sqrtf(dx*dx + dy*dy);
    float minDist = a->radius + b->radius;

    if (dist < minDist && dist > 0.0f) {
        float overlap = minDist - dist;
        float nx = dx / dist;
        float ny = dy / dist;
        
        float total_mass = a->mass + b->mass;
        a->x -= nx * overlap * (b->mass / total_mass);
        a->y -= ny * overlap * (b->mass / total_mass);
        b->x += nx * overlap * (a->mass / total_mass);
        b->y += ny * overlap * (a->mass / total_mass);

        float rvx = b->vx - a->vx;
        float rvy = b->vy - a->vy;
        float velAlongNormal = rvx * nx + rvy * ny;

        if (velAlongNormal > 0) return;

        float j = -(1.0f + bounciness) * velAlongNormal;
        j /= (1.0f / a->mass + 1.0f / b->mass);

        if (a->mass > b->mass && a->is_kicking) j *= 2.0f; 
        if (b->mass > a->mass && b->is_kicking) j *= 2.0f;

        float impulseX = j * nx;
        float impulseY = j * ny;

        a->vx -= impulseX / a->mass;
        a->vy -= impulseY / a->mass;
        b->vx += impulseX / b->mass;
        b->vy += impulseY / b->mass;
        
        float max_speed = 14.0f;
        if (a->mass < 1.0f) { 
            float speed = sqrtf(a->vx*a->vx + a->vy*a->vy);
            if(speed > max_speed) { a->vx = (a->vx/speed)*max_speed; a->vy = (a->vy/speed)*max_speed; }
        }
        if (b->mass < 1.0f) { 
            float speed = sqrtf(b->vx*b->vx + b->vy*b->vy);
            if(speed > max_speed) { b->vx = (b->vx/speed)*max_speed; b->vy = (b->vy/speed)*max_speed; }
        }
    }
}

void reset_haxball_positions() {
    if (hb_last_conceded == 0) hb_ball.x = LCD_H_RES/2 - 70;      
    else if (hb_last_conceded == 1) hb_ball.x = LCD_H_RES/2 + 70; 
    else hb_ball.x = LCD_H_RES/2; 
    
    hb_ball.y = LCD_V_RES/2;
    hb_ball.vx = 0; hb_ball.vy = 0;
    hb_ball.radius = 8;
    hb_ball.mass = 0.5f;
    hb_ball.color = PURE_WHITE;

    int l_idx = 0, r_idx = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        hb_players[i].vx = 0; hb_players[i].vy = 0;
        hb_players[i].radius = 14; hb_players[i].mass = 2.0f;
        hb_players[i].color = players[i].color;
        hb_players[i].is_kicking = false;
        
        if (hb_teams[i] == 0) { 
            hb_players[i].x = 70;
            hb_players[i].y = 120 + (l_idx * 80);
            l_idx++;
        } else if (hb_teams[i] == 1) { 
            hb_players[i].x = LCD_H_RES - 70;
            hb_players[i].y = 120 + (r_idx * 80);
            r_idx++;
        } else {
            hb_players[i].x = -100; 
        }
    }
    
    hb_match_state = 3; 
    hb_timer = xTaskGetTickCount();
}

void reset_haxball_match() {
    hb_score_left = 0; 
    hb_score_right = 0;
    hb_last_conceded = -1; 
    reset_haxball_positions();
}

// =============================================================================
// TRON & ICY TOWER SETUP
// =============================================================================
void reset_tron_round(void) {
    if (pTronGrid != NULL) memset(pTronGrid, 0, TRON_W * TRON_H);
    int16_t spawn_x[MAX_PLAYERS] = {15, TRON_W - 15, TRON_W / 2, TRON_W / 2};
    int16_t spawn_y[MAX_PLAYERS] = {TRON_H / 2, TRON_H / 2, 15, TRON_H - 15};
    int8_t spawn_dx[MAX_PLAYERS] = {1, -1, 0, 0};
    int8_t spawn_dy[MAX_PLAYERS] = {0, 0, 1, -1};

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].is_active && tron_lives[i] > 0) {
            tron_players[i].x = spawn_x[i]; tron_players[i].y = spawn_y[i];
            tron_players[i].dir_x = spawn_dx[i]; tron_players[i].dir_y = spawn_dy[i];
            tron_players[i].last_dir_x = spawn_dx[i]; tron_players[i].last_dir_y = spawn_dy[i];
            tron_players[i].alive = true;
            memset(tron_players[i].tail_x, 0, sizeof(tron_players[i].tail_x));
            memset(tron_players[i].tail_y, 0, sizeof(tron_players[i].tail_y));
            tron_players[i].tail_x[0] = spawn_x[i]; tron_players[i].tail_y[0] = spawn_y[i];
            tron_players[i].tail_head = 1; tron_players[i].tail_len = 1;
            if (pTronGrid != NULL) pTronGrid[spawn_y[i] * TRON_W + spawn_x[i]] = (i + 1);
        } else {
            tron_players[i].alive = false;
        }
    }
    tron_game_over = false;
}

void reset_tower_round(void) {
    tw_camera_y = 0;
    tw_auto_scroll = 0.5f;
    tw_highest_score = 0;
    
    for (int p = 0; p < TOWER_MAX_PLATFORMS; p++) {
        tw_platforms[p].x = esp_random() % (LCD_H_RES - 80) + 10;
        tw_platforms[p].y = LCD_V_RES - (p * 45);
        tw_platforms[p].width = 60 + (esp_random() % 40);
        tw_platforms[p].type = 0;
    }
    tw_platforms[0].x = 0;
    tw_platforms[0].y = LCD_V_RES - 20;
    tw_platforms[0].width = LCD_H_RES;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].is_active) {
            tw_players[i].x = 40 + (i * 100);
            tw_players[i].y = LCD_V_RES - 40;
            tw_players[i].vx = 0;
            tw_players[i].vy = 0;
            tw_players[i].is_alive = true;
            tw_players[i].is_jumping = false;
        } else {
            tw_players[i].is_alive = false;
        }
    }
    tw_game_over = false;
    tw_winner_id = -1;
}

static bool IRAM_ATTR vSpiDmaDoneCallback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    BaseType_t xHighPriorityTaskWoken = pdFALSE; xSemaphoreGiveFromISR(xDmaDoneSemaphore, &xHighPriorityTaskWoken); return xHighPriorityTaskWoken == pdTRUE;
}
void display_send_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd, const uint8_t *params, size_t param_len) { esp_lcd_panel_io_tx_param(io, cmd, params, param_len); }
void display_set_window(esp_lcd_panel_io_handle_t io, uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end) {
    uint8_t col_bounds[] = {x_start >> 8, x_start & 0xFF, x_end >> 8, x_end & 0xFF}; uint8_t row_bounds[] = {y_start >> 8, y_start & 0xFF, y_end >> 8, y_end & 0xFF};
    display_send_cmd(io, 0x2A, col_bounds, 4); display_send_cmd(io, 0x2B, row_bounds, 4);
}

void on_esp_now_recv(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
    if (len == sizeof(gamepad_state_t)) {
        gamepad_state_t incoming_input; memcpy(&incoming_input, data, sizeof(gamepad_state_t));
        uint8_t p_id = incoming_input.player_id;
        if (p_id >= 1 && p_id <= MAX_PLAYERS) input_rx_timestamp[p_id - 1] = esp_timer_get_time();
        xQueueSend(xGamepadQueue, &incoming_input, (TickType_t)0);
    }
}

// =============================================================================
// MAIN ENGINE TASK
// =============================================================================
void vGameLogicTask(void *pvParameters) {
    TickType_t xFrameDeadline = xTaskGetTickCount();
    gamepad_state_t input_payload;
    uint16_t *pCurrentDrawCanvas = NULL;

    // --- Menu State ---
    int menu_selection  = 0;    // root: 0=GAMES, 1=SETTINGS
    int menu_mode       = 0;    // 0 = root, 1 = games submenu
    int games_selection = 0;    // 0..4 inside games submenu
    uint32_t last_menu_move = 0;

    // --- GLIITCH Logo Glitch State ---
    // The logo "GLIITCH" has 7 characters (indices 0-6).
    uint8_t  glitch_mask      = 0;       // bitmask: bit N = letter N is glitched
    int8_t   glitch_offset[7] = {0};     // per-letter Y shift in pixels
    uint16_t glitch_colors[7];           // per-letter color
    uint32_t glitch_next_fire = 0;       // tick when next glitch fires
    uint32_t glitch_clear_at  = 0;       // tick when glitch clears
    bool     glitch_active    = false;
    bool btn_last_state = false;

    // Init logo colors to NEON_PINK
    for (int i = 0; i < 7; i++) glitch_colors[i] = NEON_PINK;

    uint32_t clear_val32 = ((uint32_t)MATTE_CHARCOAL << 16) | MATTE_CHARCOAL;

    while (1) {
        vTaskDelayUntil(&xFrameDeadline, pdMS_TO_TICKS(FRAME_DELAY_MS));
        xQueueReceive(xEmptyFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);

        // Background Clear
        uint32_t *pCanvas32 = (uint32_t *)pCurrentDrawCanvas;
        uint32_t bg_val32 = ((uint32_t)MATTE_CHARCOAL << 16) | MATTE_CHARCOAL;
        if (current_state == STATE_GAME_HAXBALL || current_state == STATE_HAXBALL_LOBBY)
            bg_val32 = (((uint32_t)GRASS_GREEN << 16) | GRASS_GREEN);
        for (int i = 0; i < (LCD_H_RES * LCD_V_RES) / 2; i++) pCanvas32[i] = bg_val32;

        while (xQueueReceive(xGamepadQueue, &input_payload, 0) == pdTRUE) {
            uint8_t p_id = input_payload.player_id;
            if (p_id >= 1 && p_id <= MAX_PLAYERS) {
                int idx = p_id - 1;
                players[idx].is_active = true;
                players[idx].last_seen = xTaskGetTickCount();

                if (current_state == STATE_MAIN_MENU && p_id == leader_player_id) {
                    bool current_btn_state = (input_payload.buttons & (1 << 0));

                    if (xTaskGetTickCount() - last_menu_move > pdMS_TO_TICKS(200)) {
                        if (input_payload.joy_y > 3000) {
                            if (menu_mode == 0) menu_selection = (menu_selection + 1) % 2;
                            else games_selection = (games_selection + 1) % 5;
                            last_menu_move = xTaskGetTickCount();
                        }
                        if (input_payload.joy_y < 1000) {
                            if (menu_mode == 0) menu_selection = (menu_selection - 1 + 2) % 2;
                            else games_selection = (games_selection - 1 + 5) % 5;
                            last_menu_move = xTaskGetTickCount();
                        }
                    }

                    if (current_btn_state && !btn_last_state) {
                        if (menu_mode == 0) {
                            if (menu_selection == 0) { menu_mode = 1; games_selection = 0; }
                            else { current_state = STATE_PAD_SETTINGS; }
                        } else {
                            if (games_selection == 0) current_state = STATE_GAME_BRICKS;
                            else if (games_selection == 1) { 
                                pTronGrid = heap_caps_malloc(TRON_W * TRON_H, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                                reset_tron_round(); current_state = STATE_GAME_TRON; 
                            }
                            else if (games_selection == 2) { for (int i = 0; i < MAX_PLAYERS; i++) hb_teams[i] = -1; current_state = STATE_HAXBALL_LOBBY; }
                            else if (games_selection == 3) { reset_tower_round(); current_state = STATE_GAME_TOWER; }
                            else if (games_selection == 4) { if (gb_init_emulator()) current_state = STATE_GAME_GB; }
                        }
                    }
                    if (input_payload.buttons & (1 << 3)) { menu_mode = 0; }
                    btn_last_state = current_btn_state;
                }
                // ... (Place your HAXBALL, TRON, etc. logic here) ...
            
        

                // ---- HAXBALL LOBBY ----
                else if (current_state == STATE_HAXBALL_LOBBY) {
                    if (input_payload.joy_x < 1000) { hb_teams[idx] = 0; }
                    else if (input_payload.joy_x > 3000) { hb_teams[idx] = 1; }
                    else if (input_payload.joy_y < 1000 || input_payload.joy_y > 3000) { hb_teams[idx] = -1; }
                    
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3)))
                        current_state = STATE_MAIN_MENU;
                    
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 0))) {
                        bool teams_ready = false;
                        for (int i = 0; i < MAX_PLAYERS; i++) {
                            if (players[i].is_active && hb_teams[i] != -1) { teams_ready = true; break; }
                        }
                        if (teams_ready) {
                            reset_haxball_match();
                            current_state = STATE_GAME_HAXBALL;
                        }
                    }
                }

                // ---- HAXBALL MATCH ----
                else if (current_state == STATE_GAME_HAXBALL) {
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3)))
                        current_state = STATE_MAIN_MENU;
                    
                    if (hb_match_state == 0 || hb_match_state == 3) {
                        float accel = 0.35f;
                        if (input_payload.joy_x < 1500) hb_players[idx].vx -= accel;
                        if (input_payload.joy_x > 2500) hb_players[idx].vx += accel;
                        if (input_payload.joy_y < 1500) hb_players[idx].vy -= accel;
                        if (input_payload.joy_y > 2500) hb_players[idx].vy += accel;
                        hb_players[idx].is_kicking = (input_payload.buttons & (1 << 0)) ? true : false;
                    }
                    if (hb_match_state == 2 && (input_payload.buttons & (1 << 0)))
                        reset_haxball_match();
                }

                // ---- ICY TOWER INPUT ----
                else if (current_state == STATE_GAME_TOWER) {
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3)))
                        current_state = STATE_MAIN_MENU;
                    
                    if (tw_game_over && (input_payload.buttons & (1 << 0)))
                        reset_tower_round();
                    
                    if (!tw_game_over && tw_players[idx].is_alive) {
                        float tw_accel = 0.8f;
                        if (input_payload.joy_x < 1500)      tw_players[idx].vx -= tw_accel;
                        else if (input_payload.joy_x > 2500)  tw_players[idx].vx += tw_accel;
                        else                                   tw_players[idx].vx *= 0.8f;

                        if ((input_payload.buttons & (1 << 0)) && !tw_players[idx].is_jumping) {
                            tw_players[idx].vy = -11.0f;
                            tw_players[idx].is_jumping = true;
                        }
                    }
                }

                // ---- TRON ----
                else if (current_state == STATE_GAME_TRON) {
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3))) {
                        if (pTronGrid) { heap_caps_free(pTronGrid); pTronGrid = NULL; }
                        current_state = STATE_MAIN_MENU;
                    }
                    if (input_payload.buttons & (1 << 0)) {
                        if (tron_tournament_over) {
                            for (int i = 0; i < MAX_PLAYERS; i++) tron_lives[i] = 3;
                            tron_tournament_over = false; tron_winner_id = -1;
                            reset_tron_round();
                        } else if (tron_game_over) {
                            reset_tron_round();
                        }
                    }
                    if (tron_players[idx].alive && !tron_game_over && !tron_tournament_over) {
                        if (input_payload.joy_x < 1200 && tron_players[idx].last_dir_x == 0)      { tron_players[idx].dir_x = -1; tron_players[idx].dir_y = 0; }
                        else if (input_payload.joy_x > 2800 && tron_players[idx].last_dir_x == 0) { tron_players[idx].dir_x = 1;  tron_players[idx].dir_y = 0; }
                        else if (input_payload.joy_y < 1200 && tron_players[idx].last_dir_y == 0) { tron_players[idx].dir_x = 0;  tron_players[idx].dir_y = -1; }
                        else if (input_payload.joy_y > 2800 && tron_players[idx].last_dir_y == 0) { tron_players[idx].dir_x = 0;  tron_players[idx].dir_y = 1; }
                    }
                }

                // ---- BRICKS ----
                else if (current_state == STATE_GAME_BRICKS) {
                    if (input_payload.joy_x < 1500) players[idx].x -= 3;
                    if (input_payload.joy_x > 2500) players[idx].x += 3;
                    if (input_payload.joy_y < 1500) players[idx].y -= 3;
                    if (input_payload.joy_y > 2500) players[idx].y += 3;
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3)))
                        current_state = STATE_MAIN_MENU;
                }

                // ---- POKEMON GB ----
                else if (current_state == STATE_GAME_GB) {
                    if (p_id == leader_player_id && (input_payload.buttons & (1 << 3))) {
                        save_gb_ram_to_nvs();
                        gb_free_emulator();
                        current_state = STATE_MAIN_MENU;
                    }
                    if (p_id == leader_player_id) {
                        gb.direct.joypad_bits.a      = !(input_payload.buttons & (1 << 0));
                        gb.direct.joypad_bits.b      = !(input_payload.buttons & (1 << 1));
                        gb.direct.joypad_bits.select = !(input_payload.buttons & (1 << 2));
                        gb.direct.joypad_bits.start  = !(input_payload.buttons & (1 << 4));
                        gb.direct.joypad_bits.right  = !(input_payload.joy_x > 2500);
                        gb.direct.joypad_bits.left   = !(input_payload.joy_x < 1500);
                        gb.direct.joypad_bits.up     = !(input_payload.joy_y < 1500);
                        gb.direct.joypad_bits.down   = !(input_payload.joy_y > 2500);
                    }
                }

                // ---- PAD SETTINGS ----
                else if (current_state == STATE_PAD_SETTINGS) {
                    if (input_payload.buttons & (1 << 3)) current_state = STATE_MAIN_MENU;
                    if (input_payload.buttons & (1 << 1)) leader_player_id = p_id;
                }
            }
        } // end queue receive loop

        // Timeout inactive players
        uint32_t now = xTaskGetTickCount();
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].is_active && (now - players[i].last_seen) * portTICK_PERIOD_MS > 2000)
                players[i].is_active = false;
        }

        // =====================================================================
        // PHYSICS AND RENDER BLOCK
        // =====================================================================

        // ---- MAIN MENU ----
        if (current_state == STATE_MAIN_MENU) {

            // --- GLIITCH LOGO GLITCH ENGINE ---
            // Schedule random glitch bursts every 1-4 seconds
            if (!glitch_active && now >= glitch_next_fire) {
                glitch_active = true;

                // Pick 1-3 random letters to glitch
                glitch_mask = 0;
                int num_glitch = 1 + (esp_random() % 3);
                for (int g = 0; g < num_glitch; g++) {
                    int letter = esp_random() % 7;
                    glitch_mask |= (1 << letter);
                }

                // Random scanline-shift offsets: -8 to +8 pixels
                for (int g = 0; g < 7; g++) {
                    if (glitch_mask & (1 << g)) {
                        int8_t shift = (int8_t)((esp_random() % 17) - 8);
                        glitch_offset[g] = shift;
                        // Flash color between cyan and pink randomly
                        glitch_colors[g] = (esp_random() % 2) ? NEON_CYAN : PURE_WHITE;
                    } else {
                        glitch_offset[g] = 0;
                        glitch_colors[g] = NEON_PINK;
                    }
                }

                // Glitch lasts 50-200ms
                uint32_t dur_ticks = pdMS_TO_TICKS(50 + (esp_random() % 150));
                glitch_clear_at = now + dur_ticks;

                // Next glitch fires 1-4 seconds after this one ends
                glitch_next_fire = glitch_clear_at + pdMS_TO_TICKS(1000 + (esp_random() % 3000));
            }

            // Clear glitch when time is up
            if (glitch_active && now >= glitch_clear_at) {
                glitch_active = false;
                glitch_mask   = 0;
                for (int g = 0; g < 7; g++) {
                    glitch_offset[g] = 0;
                    glitch_colors[g] = NEON_PINK;
                }
            }

            // Sometimes during an active glitch, rapidly re-randomise for flicker effect
            if (glitch_active && (now % 4 == 0)) {
                for (int g = 0; g < 7; g++) {
                    if (glitch_mask & (1 << g)) {
                        glitch_colors[g] = (esp_random() % 2) ? NEON_CYAN : PURE_WHITE;
                    }
                }
            }

            // Draw the glitching GLIITCH logo (centred at x=130 so 7 letters * 32px = 224px)
            canvas_draw_logo_gliitch(pCurrentDrawCanvas, 128, 18,
                                     glitch_mask, glitch_offset, glitch_colors);

            // --- MENU BODY ---
            if (menu_mode == 0) {
                // Root menu
                canvas_draw_text(pCurrentDrawCanvas, 115, 80, "SYSTEM MAIN MENU", TEXT_GRAY, 2);

                canvas_draw_text(pCurrentDrawCanvas,
                    120, 145,
                    (menu_selection == 0) ? "> GAMES" : "  GAMES",
                    (menu_selection == 0) ? NEON_CYAN : PURE_WHITE,
                    3);

                canvas_draw_text(pCurrentDrawCanvas,
                    120, 200,
                    (menu_selection == 1) ? "> SETTINGS" : "  SETTINGS",
                    (menu_selection == 1) ? NEON_CYAN : PURE_WHITE,
                    3);

                // Subtle hint at the bottom
                canvas_draw_text(pCurrentDrawCanvas, 140, 290, "WHITE = SELECT", TEXT_GRAY, 1);

            } else {
                // Games submenu
                canvas_draw_text(pCurrentDrawCanvas, 160, 80, "GAMES", NEON_PINK, 3);

                const char *games[] = {
                    "PABLO BRICKS",
                    "SQUARE TRON",
                    "GLIITCH BALL",
                    "ICY TOWER 4P",
                    "POKEMON GB"
                };

                for (int i = 0; i < 5; i++) {
                    bool sel = (games_selection == i);
                    uint16_t item_color = sel ? NEON_CYAN : PURE_WHITE;

                    // Arrow cursor
                    canvas_draw_text(pCurrentDrawCanvas,
                        60, 135 + (i * 30),
                        sel ? "> " : "  ",
                        NEON_CYAN, 2);

                    // Game name
                    canvas_draw_text(pCurrentDrawCanvas,
                        82, 135 + (i * 30),
                        games[i],
                        item_color, 2);
                }

                // Navigation hints
                canvas_draw_text(pCurrentDrawCanvas, 40, 290, "WHITE=SELECT  BTN4=BACK", TEXT_GRAY, 1);
            }

            // Leader pad indicator (bottom-right)
            char leader_str[8];
            sprintf(leader_str, "P%d", leader_player_id);
            canvas_draw_text(pCurrentDrawCanvas, 430, 290, leader_str, NEON_PINK, 2);
        }

        // ---- ICY TOWER ----
        else if (current_state == STATE_GAME_TOWER) {
            if (!tw_game_over) {
                tw_camera_y -= tw_auto_scroll;
                tw_auto_scroll += 0.001f;

                int active_count = 0;
                int alive_count  = 0;
                int last_alive_idx = -1;

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (!players[i].is_active) continue;
                    active_count++;
                    if (!tw_players[i].is_alive) continue;
                    alive_count++;
                    last_alive_idx = i;

                    tw_players[i].vy += 0.5f;
                    if (tw_players[i].vy >  15.0f) tw_players[i].vy =  15.0f;
                    if (tw_players[i].vx >   8.0f) tw_players[i].vx =   8.0f;
                    if (tw_players[i].vx <  -8.0f) tw_players[i].vx =  -8.0f;

                    tw_players[i].x += tw_players[i].vx;
                    tw_players[i].y += tw_players[i].vy;

                    if (tw_players[i].x < -10)       tw_players[i].x = LCD_H_RES;
                    if (tw_players[i].x > LCD_H_RES)  tw_players[i].x = -10;

                    if (tw_players[i].vy > 0) {
                        for (int p = 0; p < TOWER_MAX_PLATFORMS; p++) {
                            bool within_x = (tw_players[i].x + 10 >= tw_platforms[p].x &&
                                             tw_players[i].x - 10 <= tw_platforms[p].x + tw_platforms[p].width);
                            if (within_x) {
                                if (tw_players[i].vy > 0 &&
                                    tw_players[i].y + 10 >= tw_platforms[p].y &&
                                    tw_players[i].y - tw_players[i].vy <= tw_platforms[p].y) {

                                    if (tw_platforms[p].type == 1) {
                                        tw_players[i].is_alive = false;
                                    } else if (tw_platforms[p].type == 2) {
                                        tw_players[i].y  = tw_platforms[p].y - 10;
                                        tw_players[i].vy = -16.0f;
                                        tw_players[i].is_jumping = true;
                                    } else {
                                        tw_players[i].y  = tw_platforms[p].y - 10;
                                        tw_players[i].vy = 0;
                                        tw_players[i].is_jumping = false;
                                    }
                                } else if (tw_players[i].vy < 0 &&
                                           tw_players[i].y - 10 <= tw_platforms[p].y + 6 &&
                                           tw_players[i].y - tw_players[i].vy >= tw_platforms[p].y + 6) {
                                    tw_players[i].y  = tw_platforms[p].y + 6 + 10;
                                    tw_players[i].vy = 0.5f;
                                }
                            }
                        }
                    }

                    if (tw_players[i].y < tw_camera_y + 100)
                        tw_camera_y = tw_players[i].y - 100;

                    int p_score = (int)(-tw_players[i].y / 10);
                    if (p_score > tw_highest_score) tw_highest_score = p_score;

                    if (tw_players[i].y > tw_camera_y + LCD_V_RES + 20)
                        tw_players[i].is_alive = false;
                }

                if ((active_count > 1 && alive_count <= 1) || (active_count == 1 && alive_count == 0)) {
                    tw_game_over  = true;
                    tw_winner_id  = last_alive_idx;
                }

                for (int p = 0; p < TOWER_MAX_PLATFORMS; p++) {
                    if (tw_platforms[p].y > tw_camera_y + LCD_V_RES + 20) {
                        float highest_y = tw_camera_y + LCD_V_RES;
                        for (int k = 0; k < TOWER_MAX_PLATFORMS; k++) {
                            if (tw_platforms[k].y < highest_y) highest_y = tw_platforms[k].y;
                        }
                        tw_platforms[p].y     = highest_y - 45 - (esp_random() % 20);
                        tw_platforms[p].x     = esp_random() % (LCD_H_RES - 100) + 10;
                        tw_platforms[p].width = 40 + (esp_random() % 60);
                        int r = esp_random() % 100;
                        if      (r < 25) tw_platforms[p].type = 1;
                        else if (r < 40) tw_platforms[p].type = 2;
                        else             tw_platforms[p].type = 0;
                    }
                }
            }

            // Render platforms
            for (int p = 0; p < TOWER_MAX_PLATFORMS; p++) {
                int draw_y = (int)(tw_platforms[p].y - tw_camera_y);
                if (draw_y > 0 && draw_y < LCD_V_RES) {
                    if (tw_platforms[p].type == 0) {
                        for (int h = 0; h < 6; h++) {
                            for (int w = 0; w < tw_platforms[p].width; w++) {
                                int px = (int)tw_platforms[p].x + w;
                                int py = draw_y + h;
                                if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES)
                                    pCurrentDrawCanvas[py * LCD_H_RES + px] = ICE_BLUE;
                            }
                        }
                    } else if (tw_platforms[p].type == 1) {
                        for (int w = 0; w < tw_platforms[p].width; w++) {
                            int px = (int)tw_platforms[p].x + w;
                            if (px < 0 || px >= LCD_H_RES) continue;
                            if (draw_y + 4 >= 0 && draw_y + 4 < LCD_V_RES)
                                pCurrentDrawCanvas[(draw_y + 4) * LCD_H_RES + px] = TRAP_RED;
                            if (draw_y + 5 >= 0 && draw_y + 5 < LCD_V_RES)
                                pCurrentDrawCanvas[(draw_y + 5) * LCD_H_RES + px] = TRAP_RED;
                            int mod = w % 8;
                            if (mod >= 1 && mod <= 7) {
                                int tip_y = draw_y + 4 - (mod <= 4 ? mod : 8 - mod);
                                for (int y = tip_y; y < draw_y + 4; y++) {
                                    if (y >= 0 && y < LCD_V_RES)
                                        pCurrentDrawCanvas[y * LCD_H_RES + px] = TRAP_RED;
                                }
                            }
                        }
                    } else if (tw_platforms[p].type == 2) {
                        for (int h = 0; h < 6; h++) {
                            for (int w = 0; w < tw_platforms[p].width; w++) {
                                int px = (int)tw_platforms[p].x + w;
                                int py = draw_y + h;
                                if (px >= 0 && px < LCD_H_RES && py >= 0 && py < LCD_V_RES)
                                    pCurrentDrawCanvas[py * LCD_H_RES + px] = ((w + h) % 6 < 3) ? NEON_PINK : PURE_WHITE;
                            }
                        }
                    }
                }
            }

            // Render players
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active && tw_players[i].is_alive) {
                    int draw_y = (int)(tw_players[i].y - tw_camera_y);
                    int draw_x = (int)tw_players[i].x;
                    draw_circle(pCurrentDrawCanvas, draw_x, draw_y, 10, players[i].color, false, PURE_WHITE);
                }
            }

            char score_str[20];
            sprintf(score_str, "SCORE: %d", tw_highest_score);
            canvas_draw_text(pCurrentDrawCanvas, 10, 10, score_str, PURE_WHITE, 2);

            if (tw_game_over) {
                if (tw_winner_id >= 0) {
                    char w[40]; sprintf(w, "WINNER: PAD %d", tw_winner_id + 1);
                    canvas_draw_text(pCurrentDrawCanvas, 120, 100, w, players[tw_winner_id].color, 3);
                } else {
                    canvas_draw_text(pCurrentDrawCanvas, 140, 100, "GAME OVER", TRAP_RED, 3);
                }
                canvas_draw_text(pCurrentDrawCanvas, 60, 180, "PRESS WHITE TO RESTART", PURE_WHITE, 2);
            }
        }

        // ---- HAXBALL LOBBY ----
        else if (current_state == STATE_HAXBALL_LOBBY) {
            canvas_draw_text(pCurrentDrawCanvas, 120, 20, "TEAM LOBBY", PURE_WHITE, 3);
            canvas_draw_text(pCurrentDrawCanvas, 50, 280, "JOY L/R TO JOIN  |  WHITE TO START", TEXT_GRAY, 1);
            for (int y = 60; y < 260; y++) pCurrentDrawCanvas[y * LCD_H_RES + (LCD_H_RES/2)] = PURE_WHITE;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active) {
                    int draw_x = (LCD_H_RES/2);
                    if (hb_teams[i] == 0)      draw_x = 100;
                    else if (hb_teams[i] == 1) draw_x = 380;
                    draw_circle(pCurrentDrawCanvas, draw_x, 100 + (i*40), 16, players[i].color, false, PURE_WHITE);
                    char num[5]; sprintf(num, "P%d", i+1);
                    canvas_draw_text(pCurrentDrawCanvas, draw_x - 8, 100 + (i*40) - 4, num, PURE_WHITE, 1);
                }
            }
        }

        // ---- HAXBALL MATCH ----
        else if (current_state == STATE_GAME_HAXBALL) {
            char tl_str[30] = "TEAM L: ";
            char tr_str[30] = "TEAM R: ";
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active && hb_teams[i] == 0) { char buf[5]; sprintf(buf, "P%d ", i+1); strcat(tl_str, buf); }
                if (players[i].is_active && hb_teams[i] == 1) { char buf[5]; sprintf(buf, "P%d ", i+1); strcat(tr_str, buf); }
            }
            canvas_draw_text(pCurrentDrawCanvas, 10, 10, tl_str, NEON_CYAN, 1);
            canvas_draw_text(pCurrentDrawCanvas, 380, 10, tr_str, NEON_PINK, 1);

            for (int y = 0; y < LCD_V_RES; y++) {
                pCurrentDrawCanvas[y * LCD_H_RES + (LCD_H_RES/2)] = FIELD_LINE;
                if (y > 90 && y < 230) {
                    pCurrentDrawCanvas[y * LCD_H_RES + 20]  = PURE_WHITE;
                    pCurrentDrawCanvas[y * LCD_H_RES + 460] = PURE_WHITE;
                }
            }
            draw_circle(pCurrentDrawCanvas, LCD_H_RES/2, LCD_V_RES/2, 40, FIELD_LINE, true, FIELD_LINE);

            if (hb_match_state == 0 || hb_match_state == 3) {
                float friction = 0.90f;
                if (hb_match_state == 3) {
                    float elapsed = (now - hb_timer) * portTICK_PERIOD_MS;
                    if (elapsed < 1500.0f) {
                        hb_ball.y = (LCD_V_RES/2);
                        hb_ball.vx = 0; hb_ball.vy = 0;
                    } else {
                        hb_ball.y = LCD_V_RES/2;
                        hb_match_state = 0;
                    }
                } else {
                    hb_ball.vx *= 0.98f; hb_ball.vy *= 0.98f;
                    hb_ball.x  += hb_ball.vx; hb_ball.y += hb_ball.vy;
                }

                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (players[i].is_active) {
                        hb_players[i].vx *= friction; hb_players[i].vy *= friction;
                        hb_players[i].x  += hb_players[i].vx; hb_players[i].y += hb_players[i].vy;

                        if (hb_players[i].x < hb_players[i].radius) { hb_players[i].x = hb_players[i].radius; hb_players[i].vx *= -0.5f; }
                        if (hb_players[i].x > LCD_H_RES - hb_players[i].radius) { hb_players[i].x = LCD_H_RES - hb_players[i].radius; hb_players[i].vx *= -0.5f; }
                        if (hb_players[i].y < hb_players[i].radius) { hb_players[i].y = hb_players[i].radius; hb_players[i].vy *= -0.5f; }
                        if (hb_players[i].y > LCD_V_RES - hb_players[i].radius) { hb_players[i].y = LCD_V_RES - hb_players[i].radius; hb_players[i].vy *= -0.5f; }

                        for (int j = i + 1; j < MAX_PLAYERS; j++) {
                            if (players[j].is_active) resolve_circle_collision(&hb_players[i], &hb_players[j], 0.4f);
                        }
                        if (hb_match_state == 0) resolve_circle_collision(&hb_players[i], &hb_ball, 0.85f);
                    }
                }

                if (hb_match_state == 0) {
                    if (hb_ball.y < hb_ball.radius) { hb_ball.y = hb_ball.radius; hb_ball.vy *= -0.8f; }
                    if (hb_ball.y > LCD_V_RES - hb_ball.radius) { hb_ball.y = LCD_V_RES - hb_ball.radius; hb_ball.vy *= -0.8f; }
                    if (hb_ball.x < 20) {
                        if (hb_ball.y > 90 && hb_ball.y < 230) { hb_score_right++; hb_match_state = 1; hb_timer = now; hb_last_conceded = 0; }
                        else { hb_ball.x = 20; hb_ball.vx *= -0.8f; }
                    }
                    if (hb_ball.x > 460) {
                        if (hb_ball.y > 90 && hb_ball.y < 230) { hb_score_left++; hb_match_state = 1; hb_timer = now; hb_last_conceded = 1; }
                        else { hb_ball.x = 460; hb_ball.vx *= -0.8f; }
                    }
                }
            } else if (hb_match_state == 1) {
                canvas_draw_text(pCurrentDrawCanvas, 180, 140, "GOAL!!!", PURE_WHITE, 4);
                if (now - hb_timer > pdMS_TO_TICKS(2000)) {
                    if (hb_score_left >= 3 || hb_score_right >= 3) hb_match_state = 2;
                    else reset_haxball_positions();
                }
            } else if (hb_match_state == 2) {
                char w[30];
                if (hb_score_left >= 3) sprintf(w, "TEAM L WINS!");
                else sprintf(w, "TEAM R WINS!");
                canvas_draw_text(pCurrentDrawCanvas, 140, 120, w, PURE_WHITE, 3);
                canvas_draw_text(pCurrentDrawCanvas, 100, 180, "PRESS WHITE TO RESTART", TEXT_GRAY, 2);
            }

            char score_str[10];
            sprintf(score_str, "%d - %d", hb_score_left, hb_score_right);
            canvas_draw_text(pCurrentDrawCanvas, 180, 10, score_str, PURE_WHITE, 3);
            draw_circle(pCurrentDrawCanvas, (int)hb_ball.x, (int)hb_ball.y, (int)hb_ball.radius, PURE_WHITE, false, PURE_WHITE);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active && hb_teams[i] != -1) {
                    uint16_t outline = hb_players[i].is_kicking ? PURE_WHITE : MATTE_CHARCOAL;
                    draw_circle(pCurrentDrawCanvas, (int)hb_players[i].x, (int)hb_players[i].y, (int)hb_players[i].radius, hb_players[i].color, false, outline);
                }
            }
        }

        // ---- TRON ----
        else if (current_state == STATE_GAME_TRON) {
            if (!tron_game_over && !tron_tournament_over) {
                int alive_count = 0; int total_playing = 0;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (players[i].is_active && tron_lives[i] > 0) total_playing++;
                }
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (!tron_players[i].alive) continue;
                    if (tron_players[i].tail_len >= TRON_MAX_TAIL) {
                        uint16_t tail_idx = tron_players[i].tail_head;
                        uint16_t ox = tron_players[i].tail_x[tail_idx];
                        uint16_t oy = tron_players[i].tail_y[tail_idx];
                        if (pTronGrid && pTronGrid[oy * TRON_W + ox] == (i + 1))
                            pTronGrid[oy * TRON_W + ox] = 0;
                    } else {
                        tron_players[i].tail_len++;
                    }

                    tron_players[i].x += tron_players[i].dir_x;
                    tron_players[i].y += tron_players[i].dir_y;
                    if (tron_players[i].x >= TRON_W) tron_players[i].x = 0;
                    else if (tron_players[i].x < 0)  tron_players[i].x = TRON_W - 1;
                    if (tron_players[i].y >= TRON_H) tron_players[i].y = 0;
                    else if (tron_players[i].y < 0)  tron_players[i].y = TRON_H - 1;

                    if (pTronGrid[tron_players[i].y * TRON_W + tron_players[i].x] != 0) {
                        tron_players[i].alive = false;
                        tron_lives[i]--;
                        if (pTronGrid) {
                            for (int k = 0; k < TRON_MAX_TAIL; k++) {
                                uint16_t cx = tron_players[i].tail_x[k];
                                uint16_t cy = tron_players[i].tail_y[k];
                                if (pTronGrid[cy * TRON_W + cx] == (i + 1))
                                    pTronGrid[cy * TRON_W + cx] = 0;
                            }
                        }
                        continue;
                    }
                    pTronGrid[tron_players[i].y * TRON_W + tron_players[i].x] = (i + 1);
                    tron_players[i].tail_x[tron_players[i].tail_head] = tron_players[i].x;
                    tron_players[i].tail_y[tron_players[i].tail_head] = tron_players[i].y;
                    tron_players[i].tail_head = (tron_players[i].tail_head + 1) % TRON_MAX_TAIL;
                    tron_players[i].last_dir_x = tron_players[i].dir_x;
                    tron_players[i].last_dir_y = tron_players[i].dir_y;
                    alive_count++;
                }
                if ((total_playing > 1 && alive_count <= 1) ||
                    (total_playing == 1 && alive_count == 0) ||
                     total_playing == 0) {
                    tron_game_over = true;
                    int p_lives = 0, p_winner = -1;
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (players[i].is_active && tron_lives[i] > 0) { p_lives++; p_winner = i; }
                    }
                    if (p_lives <= 1 && total_playing > 1) {
                        tron_tournament_over = true;
                        tron_winner_id = (p_winner != -1) ? (p_winner + 1) : -1;
                    } else if (p_lives == 0 && total_playing == 1) {
                        tron_tournament_over = true;
                        tron_winner_id = 0;
                    }
                }
            }

            if (pTronGrid) {
                for (int gy = 0; gy < TRON_H; gy++) {
                    for (int gx = 0; gx < TRON_W; gx++) {
                        uint8_t cell = pTronGrid[gy * TRON_W + gx];
                        if (cell != 0) {
                            uint16_t color = players[cell - 1].color;
                            for (int dy = 0; dy < TRON_SCALE; dy++)
                                for (int dx = 0; dx < TRON_SCALE; dx++)
                                    pCurrentDrawCanvas[(gy * TRON_SCALE + dy) * LCD_H_RES + (gx * TRON_SCALE + dx)] = color;
                        }
                    }
                }
            }

            if (tron_tournament_over) {
                if (tron_winner_id > 0) {
                    char w[40]; sprintf(w, "TOURNAMENT WINNER: PAD %d", tron_winner_id);
                    canvas_draw_text(pCurrentDrawCanvas, 40, 110, w, NEON_CYAN, 2);
                } else {
                    canvas_draw_text(pCurrentDrawCanvas, 130, 110, "GAME OVER (TRAINING)", NEON_PINK, 2);
                }
                canvas_draw_text(pCurrentDrawCanvas, 60, 170, "PRESS WHITE TO RESTART", PURE_WHITE, 2);
            } else if (tron_game_over) {
                canvas_draw_text(pCurrentDrawCanvas, 140, 30, "ROUND OVER", NEON_PINK, 3);
                int dy = 90;
                char ls[30];
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    if (players[i].is_active) {
                        sprintf(ls, "PAD %d REMAINING LIVES: %d", i+1, tron_lives[i]);
                        canvas_draw_text(pCurrentDrawCanvas, 60, dy, ls, players[i].color, 2);
                        dy += 30;
                    }
                }
                canvas_draw_text(pCurrentDrawCanvas, 50, dy + 20, "PRESS WHITE FOR NEXT ROUND", PURE_WHITE, 2);
            }
        }

        // ---- BRICKS ----
        else if (current_state == STATE_GAME_BRICKS) {
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (players[p].is_active) {
                    for (int row = players[p].y; row < (players[p].y + BOX_SIZE); row++) {
                        for (int col = players[p].x; col < (players[p].x + BOX_SIZE); col++) {
                            if (col >= 0 && col < LCD_H_RES && row >= 0 && row < LCD_V_RES)
                                pCurrentDrawCanvas[row * LCD_H_RES + col] = players[p].color;
                        }
                    }
                }
            }
        }

        // ---- POKEMON GB ----
        else if (current_state == STATE_GAME_GB) {
            gb_run_frame(&gb);
            const int blit_x = (LCD_H_RES - GB_LCD_WIDTH  * GB_SCALE) / 2;
            const int blit_y = (LCD_V_RES - GB_LCD_HEIGHT * GB_SCALE) / 2;
            const int buf_w  = GB_LCD_WIDTH * GB_SCALE;
            for (int y = 0; y < GB_LCD_HEIGHT * GB_SCALE; y++) {
                memcpy(pCurrentDrawCanvas + (blit_y + y) * LCD_H_RES + blit_x,
                       gb_framebuf + y * buf_w,
                       buf_w * sizeof(uint16_t));
            }
        }

        // ---- PAD SETTINGS ----
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
        }

        xQueueSend(xFilledFrameQueue, &pCurrentDrawCanvas, portMAX_DELAY);
    }
}

// =============================================================================
// HARDWARE OUTPUT TASK & SYSTEM ROOT SETUP
// =============================================================================
void vDisplayRenderTask(void *pvParameters) {
    uint16_t *pCanvasToRender = NULL;
    const int chunk_pixels = LCD_H_RES * CHUNK_LINES;
    const size_t chunk_bytes = chunk_pixels * sizeof(uint16_t);

    while (1) {
        if (xQueueReceive(xFilledFrameQueue, &pCanvasToRender, portMAX_DELAY) == pdTRUE) {
            for (int y = 0; y < LCD_V_RES; y += CHUNK_LINES) {
                memcpy(pDmaStreamBuffer, pCanvasToRender + (y * LCD_H_RES), chunk_bytes);
                display_set_window(display_io_handle, 0, y, LCD_H_RES - 1, y + CHUNK_LINES - 1);
                esp_lcd_panel_io_tx_color(display_io_handle, 0x2C, pDmaStreamBuffer, chunk_bytes);
                xSemaphoreTake(xDmaDoneSemaphore, portMAX_DELAY);
            }
            uint64_t frame_done_time = esp_timer_get_time();
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].is_active && input_rx_timestamp[i] != 0)
                    final_latency_us[i] = frame_done_time - input_rx_timestamp[i];
            }
            xQueueSend(xEmptyFrameQueue, &pCanvasToRender, portMAX_DELAY);
        }
    }
}

// =============================================================================
// SYSTEM MONITORING
// =============================================================================
void vSystemMonitorTask(void *pvParameters) {
    char *pcTaskListBuffer = malloc(512);
    while (1) {
        ESP_LOGI("MONITOR", "--- SYSTEM VITAL SIGNS ---");
        size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI("MONITOR", "Free PSRAM (Canvas Memory): %zu bytes", free_psram);
        ESP_LOGI("MONITOR", "Free Internal RAM: %zu bytes", free_internal);
        if (pcTaskListBuffer != NULL) {
            vTaskList(pcTaskListBuffer);
            ESP_LOGI("MONITOR", "\nTask Name\tState\tPrio\tStack\tNum\n%s", pcTaskListBuffer);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    pGameCanvasA     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pGameCanvasB     = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pDmaStreamBuffer = (uint16_t *)heap_caps_malloc(LCD_H_RES * CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (pGameCanvasA == NULL || pGameCanvasB == NULL || pDmaStreamBuffer == NULL) {
        ESP_LOGE(TAG, "Heap Exhaustion Core Allocation Failure!");
        return;
    }

    gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RST, 0); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NUM_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));

    spi_bus_config_t bus_configuration = {
        .sclk_io_num   = PIN_NUM_CLK,
        .mosi_io_num   = PIN_NUM_MOSI,
        .miso_io_num   = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * CHUNK_LINES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_configuration, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_bus_config = {
        .dc_gpio_num      = PIN_NUM_DC,
        .cs_gpio_num      = PIN_NUM_CS,
        .pclk_hz          = SPI_MHZ_SPEED * 1000 * 1000,
        .lcd_cmd_bits     = 8,
        .lcd_param_bits   = 8,
        .spi_mode         = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_bus_config, &display_io_handle));

    xDmaDoneSemaphore = xSemaphoreCreateBinary();
    xGamepadQueue     = xQueueCreate(32, sizeof(gamepad_state_t));
    xFilledFrameQueue = xQueueCreate(1,  sizeof(uint16_t*));
    xEmptyFrameQueue  = xQueueCreate(2,  sizeof(uint16_t*));

    esp_lcd_panel_io_callbacks_t display_callbacks = { .on_color_trans_done = vSpiDmaDoneCallback };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(display_io_handle, &display_callbacks, NULL));

    xQueueSend(xEmptyFrameQueue, &pGameCanvasA, 0);
    xQueueSend(xEmptyFrameQueue, &pGameCanvasB, 0);

    display_send_cmd(display_io_handle, 0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));
    uint8_t rotation_param[]    = {0xE8}; display_send_cmd(display_io_handle, 0x36, rotation_param, 1);
    uint8_t data_color_depth[]  = {0x55}; display_send_cmd(display_io_handle, 0x3A, data_color_depth, 1);
    display_send_cmd(display_io_handle, 0x29, NULL, 0); vTaskDelay(pdMS_TO_TICKS(20));

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
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));

    xTaskCreatePinnedToCore(vDisplayRenderTask, "GFX_Render",    4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(vGameLogicTask,     "Game_Engine",   8192, NULL, 5, NULL, 0);
    xTaskCreate(vSystemMonitorTask,             "System_Monitor", 2048, NULL, 4, NULL);
    ESP_LOGI(TAG, "All processes deployed successfully.");
}