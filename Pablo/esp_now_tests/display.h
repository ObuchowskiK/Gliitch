#ifndef DISPLAY_H
#define DISPLAY_H
void display_init(void);
void display_clear_buffer(void);
void display_draw_pixel(int x, int y, int color);
void display_draw_rect(int x, int y, int width, int height);
void display_update_screen(void);
#endif