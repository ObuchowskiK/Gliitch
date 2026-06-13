#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LCD_H_RES 480
#define LCD_V_RES 320
#define MAX_PLAYERS 4

// Wystawiamy funkcje rysujące z main.c, żeby moduł lekkoatletyki miał do nich dostęp
extern void canvas_draw_text(uint16_t *canvas, int x, int y, const char *text, uint16_t color, int scale);
extern void draw_circle(uint16_t *canvas, int cx, int cy, int r, uint16_t color, bool outline_only, uint16_t outline_color);

// Główne funkcje modułu
void athletics_init(int sub_game);
bool athletics_process_input(uint8_t p_id, uint16_t joy_x, uint16_t joy_y, uint8_t buttons, bool is_leader);
void athletics_render(uint16_t *canvas, uint32_t current_time_ms, bool *is_active, uint16_t *colors);