#include "athletics.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SWAP16(val) (uint16_t)((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#define PURE_WHITE  SWAP16(0xFFFF)
#define NEON_CYAN   SWAP16(0x07FF)
#define NEON_PINK   SWAP16(0xF81F)
#define TEXT_GRAY   SWAP16(0x7BEF)
#define NEON_YELLOW SWAP16(0xFFE0)
#define TRACK_COLOR SWAP16(0x39C7)

// ---------------------------------------------------------------------------
// Parametry biegania
// ---------------------------------------------------------------------------
#define SPEED_PER_PRESS  2.2f
#define SPEED_DECAY      0.96f
#define SPEED_MAX        11.0f

// Progi joysticka
#define JOY_UP_THRESHOLD   1000
#define JOY_NEUTRAL_LOW    1400

// Grawitacja (units/frame^2) – stosowana w skoku w dal i płotkach
#define GRAVITY            0.32f

// Skok w dal – 3 próby
#define LONG_JUMP_ROUNDS   3

// ---------------------------------------------------------------------------
// Stan gry
// ---------------------------------------------------------------------------
typedef struct {
    int      sub_game;
    int      match_state;   // 0=odliczanie(tylko sprint/hurdles), 1=gra, 2=koniec
    uint32_t timer;

    float    x[MAX_PLAYERS], y[MAX_PLAYERS];
    float    vx[MAX_PLAYERS], vy[MAX_PLAYERS];
    float    speed[MAX_PLAYERS];

    float    score[MAX_PLAYERS];
    float    best_score[MAX_PLAYERS];

    bool     finished[MAX_PLAYERS];
    bool     false_start[MAX_PLAYERS];
    bool     in_air[MAX_PLAYERS];      // w powietrzu (skok w dal / płotki)

    uint8_t  last_btn[MAX_PLAYERS];
    uint16_t last_joy_y[MAX_PLAYERS];

    // Skok w dal
    bool     lj_round_done[MAX_PLAYERS];
    int      lj_rounds_done[MAX_PLAYERS];
    bool     lj_all_done[MAX_PLAYERS];

    bool     btn_ready[MAX_PLAYERS];   // anti-ghost dla false-start
} ath_state_t;

static ath_state_t *pAthState = NULL;

// ---------------------------------------------------------------------------
static void lj_reset_player(int i) {
    pAthState->x[i]            = 0.0f;
    pAthState->y[i]            = 0.0f;
    pAthState->vx[i]           = 0.0f;
    pAthState->vy[i]           = 0.0f;
    pAthState->speed[i]        = 0.0f;
    pAthState->in_air[i]       = false;
    pAthState->lj_round_done[i]= false;
    pAthState->finished[i]     = false;
}

// ---------------------------------------------------------------------------
void athletics_init(int sub_game) {
    if (pAthState) { heap_caps_free(pAthState); pAthState = NULL; }
    pAthState = heap_caps_malloc(sizeof(ath_state_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pAthState) return;
    memset(pAthState, 0, sizeof(ath_state_t));
    pAthState->sub_game    = sub_game;
    pAthState->timer       = xTaskGetTickCount();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        pAthState->last_joy_y[i] = 2048;
        // Skok w dal startuje od razu w grze (brak odliczania)
        pAthState->match_state = (sub_game == 1) ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
bool athletics_process_input(uint8_t p_id, uint16_t joy_x, uint16_t joy_y,
                             uint8_t buttons, bool is_leader) {
    if (p_id < 1 || p_id > MAX_PLAYERS) return false;

    // Zbocze czarnego – wyjście do menu athletics
    bool btn_black_press = false;
    if (pAthState) {
        btn_black_press = is_leader && (buttons & (1 << 3)) && !(pAthState->last_btn[p_id-1] & (1 << 3));
    }
    if (btn_black_press) {
        if (pAthState) { heap_caps_free(pAthState); pAthState = NULL; }
        return true;
    }
    if (!pAthState) return false;

    int idx = p_id - 1;

    // Zbocze białego (bit0)
    bool btn_white = (buttons & (1 << 0)) && !(pAthState->last_btn[idx] & (1 << 0));
    // Zbocze żółtego (bit1) – używany w płotkach
    bool btn_yellow = (buttons & (1 << 1)) && !(pAthState->last_btn[idx] & (1 << 1));

    pAthState->last_btn[idx]   = buttons;
    pAthState->last_joy_y[idx] = joy_y;

    // Restart po zakończeniu – biały
    if (pAthState->match_state == 2) {
        if (btn_white) { athletics_init(pAthState->sub_game); }
        return false;
    }

    // Odliczanie (tylko sprint i hurdles)
    if (pAthState->match_state == 0) {
        if ((buttons & 0xFF) == 0) pAthState->btn_ready[idx] = true;
        if (pAthState->btn_ready[idx] && btn_white)
            pAthState->false_start[idx] = true;
        return false;
    }

    if (pAthState->match_state != 1) return false;
    if (pAthState->false_start[idx]) return false;

    // ---- SPRINT (0) ----
    if (pAthState->sub_game == 0) {
        if (!pAthState->finished[idx] && btn_white) {
            pAthState->speed[idx] += SPEED_PER_PRESS;
            if (pAthState->speed[idx] > SPEED_MAX) pAthState->speed[idx] = SPEED_MAX;
        }
    }

    // ---- SKOK W DAL (1) ----
    else if (pAthState->sub_game == 1) {
        if (pAthState->lj_all_done[idx]) return false;
        if (pAthState->lj_round_done[idx]) return false;

        // Biały = bieg
        if (!pAthState->in_air[idx] && btn_white) {
            pAthState->speed[idx] += SPEED_PER_PRESS;
            if (pAthState->speed[idx] > SPEED_MAX) pAthState->speed[idx] = SPEED_MAX;
        }

        // Joystick góra = wyskok – wynik liczony z kąta NATYCHMIAST
        bool joy_was_neutral = (pAthState->last_joy_y[idx] >= JOY_NEUTRAL_LOW);
        bool joy_now_up      = (joy_y < JOY_UP_THRESHOLD);
        if (!pAthState->in_air[idx] && joy_now_up && joy_was_neutral) {
            pAthState->in_air[idx] = true;

            if (pAthState->x[idx] > 1600.0f) {
                // Foul – za daleko przed skokiem
                pAthState->score[idx]         = -1.0f;
                pAthState->lj_round_done[idx] = true;
            } else {
                // Kąt z wychylenia joysticka, wynik ze wzoru rzutu ukośnego
                float jy  = 1.0f - (float)joy_y / 4095.0f;  // 0..1
                float ang = 0.2f + jy * 0.9f;
                if (ang > 1.1f) ang = 1.1f;

                float dist = (pAthState->speed[idx] * pAthState->speed[idx])
                             * sinf(2.0f * ang) / 28.0f;
                if (dist < 0.0f) dist = 0.0f;
                pAthState->score[idx] = dist;
                if (dist > pAthState->best_score[idx])
                    pAthState->best_score[idx] = dist;

                // Prędkości tylko dla animacji łuku
                pAthState->vx[idx] = pAthState->speed[idx] * cosf(ang) * 1.2f;
                pAthState->vy[idx] = pAthState->speed[idx] * sinf(ang) * 1.2f;
            }
        }
    }

    // ---- PŁOTKI (2) – żółty przycisk = instant skok ----
    else if (pAthState->sub_game == 2) {
        if (pAthState->finished[idx]) return false;

        // Biały = bieg
        if (btn_white) {
            pAthState->speed[idx] += SPEED_PER_PRESS;
            if (pAthState->speed[idx] > SPEED_MAX) pAthState->speed[idx] = SPEED_MAX;
        }

        // Żółty = skok (instant, jak dino Google) – tylko gdy na ziemi
        if (btn_yellow && !pAthState->in_air[idx]) {
            pAthState->in_air[idx] = true;
            pAthState->vy[idx]     = 8.0f;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
void athletics_render(uint16_t *canvas, uint32_t current_time_ms,
                      bool *is_active, uint16_t *colors) {
    if (!pAthState) return;

    uint32_t now     = xTaskGetTickCount();
    float    elapsed = (float)(current_time_ms - (pAthState->timer * portTICK_PERIOD_MS));

    // Policz aktywnych graczy – do obliczenia wymiarów torów
    int active_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) if (is_active[i]) active_count++;
    if (active_count == 0) active_count = 1;

    // Wymiary toru dopasowane do ekranu i liczby graczy
    // Ekran: 480x320. Zostawiamy 20px góry (HUD) i 20px dołu (podpowiedź).
    int usable_h    = LCD_V_RES - 40;          // 260px
    int track_h     = usable_h / active_count; // wysokość jednego toru
    int player_r    = (track_h / 2) - 4;       // promień gracza, min sensowny
    if (player_r < 4)  player_r = 4;
    if (player_r > 14) player_r = 14;

    // =========================================================================
    // FIZYKA
    // =========================================================================
    if (pAthState->match_state == 0) {
        // Odliczanie 3s (sprint i hurdles)
        if (elapsed > 3000.0f) {
            pAthState->match_state = 1;
            pAthState->timer       = now;
        }
    }
    else if (pAthState->match_state == 1) {
        // Timecap 15s
        if (elapsed > 15000.0f) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (is_active[i] && !pAthState->finished[i] && !pAthState->lj_all_done[i]) {
                    pAthState->finished[i] = true;
                    pAthState->score[i]    = 15.0f;
                }
            }
            pAthState->match_state = 2;
        }

        int finished_count = 0, playing_count = 0;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!is_active[i]) continue;
            playing_count++;
            if (pAthState->false_start[i]) pAthState->finished[i] = true;

            // --- SPRINT ---
            if (pAthState->sub_game == 0) {
                if (!pAthState->finished[i]) {
                    pAthState->speed[i] *= SPEED_DECAY;
                    pAthState->x[i]     += pAthState->speed[i];
                    if (pAthState->x[i] > 1000.0f) {
                        pAthState->finished[i] = true;
                        pAthState->score[i]    = elapsed / 1000.0f;
                    }
                }
                if (pAthState->finished[i]) finished_count++;
            }

            // --- SKOK W DAL ---
            else if (pAthState->sub_game == 1) {
                if (pAthState->lj_all_done[i]) { finished_count++; continue; }

                if (pAthState->lj_round_done[i]) {
                    pAthState->lj_round_done[i] = false;
                    pAthState->lj_rounds_done[i]++;
                    if (pAthState->lj_rounds_done[i] >= LONG_JUMP_ROUNDS) {
                        pAthState->lj_all_done[i] = true;
                        pAthState->score[i]       = pAthState->best_score[i];
                        finished_count++;
                    } else {
                        lj_reset_player(i);
                    }
                    continue;
                }

                pAthState->speed[i] *= SPEED_DECAY;

                if (!pAthState->in_air[i]) {
                    pAthState->x[i] += pAthState->speed[i];
                    if (pAthState->x[i] > 1650.0f) {
                        pAthState->score[i]         = -1.0f;
                        pAthState->lj_round_done[i] = true;
                    }
                } else {
                    // Animacja paraboliczna z prawdziwą grawitacją
                    pAthState->x[i]  += pAthState->vx[i];
                    pAthState->y[i]  += pAthState->vy[i];
                    pAthState->vy[i] -= GRAVITY;
                    if (pAthState->y[i] <= 0.0f && pAthState->vy[i] < 0.0f) {
                        pAthState->y[i]             = 0.0f;
                        pAthState->lj_round_done[i] = true;
                    }
                }
            }

            // --- PŁOTKI ---
            else if (pAthState->sub_game == 2) {
                if (!pAthState->finished[i]) {
                    pAthState->speed[i] *= SPEED_DECAY;
                    pAthState->x[i]     += pAthState->speed[i];

                    // Grawitacja – zawsze gdy w powietrzu
                    if (pAthState->in_air[i]) {
                        pAthState->y[i]  += pAthState->vy[i];
                        pAthState->vy[i] -= GRAVITY;
                        if (pAthState->y[i] <= 0.0f) {
                            pAthState->y[i]     = 0.0f;
                            pAthState->vy[i]    = 0.0f;
                            pAthState->in_air[i]= false;
                        }
                    }

                    // Kolizja z płotkami – tylko gdy nisko
                    for (int h = 200; h <= 800; h += 200) {
                        if (pAthState->x[i] > h - 12 && pAthState->x[i] < h + 12
                            && pAthState->y[i] < 20.0f) {
                            pAthState->speed[i] *= 0.15f;
                            pAthState->x[i]      = (float)(h + 13);
                        }
                    }

                    if (pAthState->x[i] > 1000.0f) {
                        pAthState->finished[i] = true;
                        pAthState->score[i]    = elapsed / 1000.0f;
                    }
                }
                if (pAthState->finished[i]) finished_count++;
            }
        }

        if (playing_count > 0 && finished_count >= playing_count)
            pAthState->match_state = 2;
    }

    // =========================================================================
    // RENDEROWANIE TORÓW – dynamicznie wg active_count
    // =========================================================================
    int slot = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!is_active[i]) continue;

        // Pozycja Y toru
        int track_y = 20 + slot * track_h;
        int track_bottom = track_y + track_h - 2;  // dolna krawędź toru
        int ground_y = track_bottom - player_r - 2; // poziom gruntu dla gracza
        slot++;

        // Tło toru
        for (int ty = track_y; ty < track_bottom; ty++)
            for (int tx = 0; tx < LCD_H_RES; tx++)
                canvas[ty * LCD_H_RES + tx] = TRACK_COLOR;

        // Linia gruntu
        for (int tx = 0; tx < LCD_H_RES; tx++)
            canvas[track_bottom * LCD_H_RES + tx] = SWAP16(0x2104);

        int draw_x = 20;

        // ---- Sprint / Płotki ----
        if (pAthState->sub_game == 0 || pAthState->sub_game == 2) {
            draw_x = 20 + (int)((pAthState->x[i] / 1000.0f) * 420.0f);
            if (draw_x > 450) draw_x = 450;

            // Linia mety
            for (int ty = track_y; ty <= track_bottom; ty++)
                canvas[ty * LCD_H_RES + 455] = NEON_CYAN;

            // Płotki
            if (pAthState->sub_game == 2) {
                for (int h = 200; h <= 800; h += 200) {
                    int hx = 20 + (int)((h / 1000.0f) * 420.0f);
                    int hurdle_top = ground_y - player_r - 2;
                    for (int ty = hurdle_top; ty <= track_bottom; ty++)
                        canvas[ty * LCD_H_RES + hx] = PURE_WHITE;
                }
            }
        }
        // ---- Skok w dal ----
        else if (pAthState->sub_game == 1) {
            draw_x = 20 + (int)((pAthState->x[i] / 2500.0f) * 420.0f);
            if (draw_x > 458) draw_x = 458;

            int board_x = 20 + (int)((1500.0f / 2500.0f) * 420.0f);
            // Belka odskoku
            for (int tx = board_x; tx < board_x + 8; tx++)
                canvas[track_bottom * LCD_H_RES + tx] = NEON_CYAN;
            // Piaskownica
            for (int ty = ground_y; ty <= track_bottom; ty++)
                for (int tx = board_x + 8; tx < 460; tx++)
                    canvas[ty * LCD_H_RES + tx] = NEON_YELLOW;
        }

        // Pozycja pionowa gracza: y[i] to wysokość w jednostkach gry
        // Skalujemy: max wysokość skoku ~20 jednostek → max pół toru w górę
        int jump_px = (int)(pAthState->y[i] * (track_h / 2) / 20.0f);
        int draw_y  = ground_y - jump_px;
        if (draw_y < track_y + player_r) draw_y = track_y + player_r;

        draw_circle(canvas, draw_x, draw_y, player_r, colors[i], false, PURE_WHITE);

        // Pasek prędkości
        if (pAthState->match_state == 1) {
            int bar = (int)((pAthState->speed[i] / SPEED_MAX) * 60.0f);
            if (bar > 60) bar = 60;
            for (int bx = 0; bx < bar; bx++)
                canvas[(track_y + 1) * LCD_H_RES + (LCD_H_RES - 65 + bx)] = NEON_CYAN;
        }

        // Status gracza
        char stat[32] = "";
        if (pAthState->false_start[i]) {
            sprintf(stat, "P%d FOUL", i + 1);
        } else if (pAthState->sub_game == 1) {
            if (pAthState->lj_all_done[i]) {
                sprintf(stat, "P%d BEST:%.2fM", i + 1, pAthState->best_score[i]);
            } else if (pAthState->in_air[i] || pAthState->lj_round_done[i]) {
                if (pAthState->score[i] < 0)
                    sprintf(stat, "P%d R%d FOUL", i + 1, pAthState->lj_rounds_done[i] + 1);
                else
                    sprintf(stat, "P%d R%d %.2fM", i + 1, pAthState->lj_rounds_done[i] + 1, pAthState->score[i]);
            } else {
                sprintf(stat, "P%d TRY %d/3", i + 1, pAthState->lj_rounds_done[i] + 1);
            }
        } else if (pAthState->finished[i]) {
            sprintf(stat, "P%d %.2fS", i + 1, pAthState->score[i]);
        }
        if (stat[0]) canvas_draw_text(canvas, 8, track_y + 2, stat, PURE_WHITE, 1);
    }

    // =========================================================================
    // HUD GLOBALNY
    // =========================================================================
    if (pAthState->match_state == 0) {
        int sec = 3 - (int)(elapsed / 1000.0f);
        char s[12];
        if (sec > 0) { sprintf(s, "%d", sec); canvas_draw_text(canvas, 224, 140, s, PURE_WHITE, 4); }
        else          canvas_draw_text(canvas, 190, 140, "GO!", NEON_CYAN, 4);
        canvas_draw_text(canvas, 88, 305, "DONT PRESS WHITE YET!", TEXT_GRAY, 1);
    }
    else if (pAthState->match_state == 1) {
        // Dla skoku w dal – brak timera
        if (pAthState->sub_game != 1) {
            char t[28]; sprintf(t, "TIME: %.2f", elapsed / 1000.0f);
            canvas_draw_text(canvas, 180, 4, t, PURE_WHITE, 2);
        }
        if (pAthState->sub_game == 0)
            canvas_draw_text(canvas, 80, 305, "WHITE=RUN FASTER", TEXT_GRAY, 1);
        else if (pAthState->sub_game == 1)
            canvas_draw_text(canvas, 30, 305, "WHITE=RUN  JOY-UP=JUMP", TEXT_GRAY, 1);
        else
            canvas_draw_text(canvas, 10, 305, "WHITE=RUN  YELLOW=JUMP", TEXT_GRAY, 1);
    }
    else if (pAthState->match_state == 2) {
        const char *event_name = (pAthState->sub_game == 0) ? "100M SPRINT" :
                                 (pAthState->sub_game == 1) ? "LONG JUMP"   : "110M HURDLES";
        canvas_draw_text(canvas, 160, 4, event_name, NEON_PINK, 2);
        canvas_draw_text(canvas, 148, 28, "RESULTS", PURE_WHITE, 3);

        int order[MAX_PLAYERS];
        int cnt = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) if (is_active[i]) order[cnt++] = i;

        for (int a = 0; a < cnt - 1; a++) {
            for (int b = a + 1; b < cnt; b++) {
                float sa = pAthState->score[order[a]];
                float sb = pAthState->score[order[b]];
                bool sw;
                if (pAthState->sub_game == 1) {
                    if (sa < 0 && sb >= 0) sw = true;
                    else if (sa >= 0 && sb < 0) sw = false;
                    else sw = (sa < sb);
                } else {
                    sw = (sa > sb);
                }
                if (sw) { int tmp = order[a]; order[a] = order[b]; order[b] = tmp; }
            }
        }

        const char *medals[]      = { "1ST", "2ND", "3RD", "4TH" };
        uint16_t    medal_colors[] = { NEON_YELLOW, TEXT_GRAY, SWAP16(0xC980), TEXT_GRAY };

        for (int r = 0; r < cnt; r++) {
            int i     = order[r];
            int row_y = 80 + r * 50;
            for (int ty = row_y - 2; ty < row_y + 20; ty++)
                for (int tx = 40; tx < LCD_H_RES - 40; tx++)
                    canvas[ty * LCD_H_RES + tx] = SWAP16(0x2104);

            canvas_draw_text(canvas, 50,  row_y, medals[r < 4 ? r : 3], medal_colors[r < 4 ? r : 3], 2);
            for (int ty = row_y; ty < row_y + 14; ty++)
                for (int tx = 100; tx < 116; tx++)
                    canvas[ty * LCD_H_RES + tx] = colors[i];

            char pname[16]; sprintf(pname, "PAD%d", i + 1);
            canvas_draw_text(canvas, 122, row_y, pname, colors[i], 2);

            char result[24];
            if (pAthState->false_start[i]) {
                sprintf(result, "FOUL");
            } else if (pAthState->sub_game == 1) {
                if (pAthState->score[i] < 0) sprintf(result, "FOUL");
                else sprintf(result, "%.2f M", pAthState->score[i]);
            } else {
                if (pAthState->score[i] >= 14.9f) sprintf(result, "DNF");
                else sprintf(result, "%.2f S", pAthState->score[i]);
            }
            canvas_draw_text(canvas, 290, row_y, result, PURE_WHITE, 2);
        }

        canvas_draw_text(canvas, 50,  308, "WHITE=PLAY AGAIN", NEON_CYAN, 1);
        canvas_draw_text(canvas, 270, 308, "BLACK=MENU", TEXT_GRAY, 1);
    }
}