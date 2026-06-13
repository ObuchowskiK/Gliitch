
#include <stdint.h>
typedef struct {
    uint8_t player_id;
    uint16_t joy_x;  
    uint16_t joy_y;     
    uint8_t buttons;  
} __attribute__((packed)) gamepad_state_t;