// Copyright 2024 SM Boards (@sm_boards)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Dino runner for the 128x32 OLED.
//
// Layout:
//   - Ground line at y = 28 (4 px from the bottom)
//   - Dino is a 6x8 pixel sprite fixed at x = 8
//   - Obstacles (cacti) are 4x8 px rectangles that scroll from the right
//     edge of the screen toward the dino at `obstacle_speed` px per tick.
//   - Score = number of obstacles successfully dodged
//
// Controls (routed from keymap.c when app_mode == APP_GAME_DINO):
//   SW5 → dino_input_jump()   — bounce off the ground if currently grounded
//
// Gravity is integer, speed increases very slightly every 10 obstacles
// to keep the game interesting.

#include "games.h"
#include "oled_custom.h"
#include "timer.h"

#include <stdlib.h>

#define GROUND_Y        28
#define DINO_X          8
#define DINO_W          6
#define DINO_H          8
// Jump tuning — 12-pixel apex over ~800 ms total airtime, which clears
// the 8 px obstacles comfortably and is forgiving on timing.
// Adjusted from the original -4/1 (8 px apex, 480 ms) which was too tight.
#define JUMP_VELOCITY   -6     // negative = upward in screen coords
#define GRAVITY         1      // px per tick added to velocity
#define STEP_MS         60     // ~16 FPS
#define OBSTACLE_W      4
#define OBSTACLE_H      8
#define BASE_SPEED      2      // pixels per tick
#define MIN_GAP         24     // minimum distance between consecutive obstacles
#define MAX_GAP_EXTRA   48     // random extra gap (0..MAX_GAP_EXTRA)

static int16_t  dino_y;           // top of sprite, integer px
static int16_t  dino_v;           // vertical velocity (px per tick)
static bool     dino_grounded;

static int16_t  obstacle_x;       // current x position (sprite left edge)
static uint8_t  obstacle_speed;
static bool     scored_this_obstacle;

static uint32_t next_step_ms;
static bool     game_over;
static uint32_t score;
static uint32_t rng_state;

/* ---- Dino sprite (6x8, 1 bit per pixel, column-major) ----

   Rows (top→bottom) →
   .##...
   ####..
   ##.#..
   ####..
   .#....
   ##.#..
   ##.#..
   .#.#..

   Each byte below represents one 8-row column; bit 0 is the top pixel.
*/
static const uint8_t dino_sprite[DINO_W] = {
    0b11111110,   // col 0
    0b11101010,   // col 1
    0b11111110,   // col 2
    0b11101011,   // col 3
    0b00011000,   // col 4
    0b00000000,   // col 5
};

/* ---- Helpers ---- */

static uint32_t game_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void spawn_obstacle(void) {
    // Place the new obstacle just past the right edge, with a random gap.
    obstacle_x = 128 + (int16_t)(game_rand() % MAX_GAP_EXTRA);
    scored_this_obstacle = false;
}

static void draw_dino_sprite(int16_t x, int16_t y) {
    // Plot each set pixel of the sprite.
    for (uint8_t col = 0; col < DINO_W; col++) {
        uint8_t bits = dino_sprite[col];
        for (uint8_t row = 0; row < DINO_H; row++) {
            if (bits & (1u << row)) {
                int16_t px = x + col;
                int16_t py = y + row;
                if (px >= 0 && py >= 0) {
                    oled_set_pixel((uint8_t)px, (uint8_t)py);
                }
            }
        }
    }
}

static bool collides_with_obstacle(void) {
    if (obstacle_x >= 128) return false;
    if (obstacle_x + OBSTACLE_W <= 0) return false;

    // Dino bbox
    int16_t dx1 = DINO_X;
    int16_t dx2 = DINO_X + DINO_W;
    int16_t dy1 = dino_y;
    int16_t dy2 = dino_y + DINO_H;

    // Obstacle bbox — sitting on the ground line, OBSTACLE_H tall.
    int16_t ox1 = obstacle_x;
    int16_t ox2 = obstacle_x + OBSTACLE_W;
    int16_t oy1 = GROUND_Y - OBSTACLE_H;
    int16_t oy2 = GROUND_Y;

    return !(dx2 <= ox1 || dx1 >= ox2 || dy2 <= oy1 || dy1 >= oy2);
}

/* ---- Public API ---- */

void dino_start(void) {
    rng_state = timer_read32() ^ 0xDEADBEEFu;
    if (rng_state == 0) rng_state = 1;

    dino_y        = GROUND_Y - DINO_H;
    dino_v        = 0;
    dino_grounded = true;
    obstacle_speed = BASE_SPEED;
    spawn_obstacle();
    score = 0;
    game_over = false;
    next_step_ms = timer_read32() + STEP_MS;
}

void dino_tick(void) {
    if (game_over) return;
    if (timer_read32() < next_step_ms) return;
    next_step_ms = timer_read32() + STEP_MS;

    // Vertical motion (jumping / falling).
    if (!dino_grounded) {
        dino_y += dino_v;
        dino_v += GRAVITY;
        if (dino_y >= GROUND_Y - DINO_H) {
            dino_y = GROUND_Y - DINO_H;
            dino_v = 0;
            dino_grounded = true;
        }
    }

    // Move obstacle toward the dino.
    obstacle_x -= obstacle_speed;

    // Did we just pass the obstacle without crashing?
    if (!scored_this_obstacle && obstacle_x + OBSTACLE_W < DINO_X) {
        score++;
        scored_this_obstacle = true;
        // Slightly faster every 10 dodges (capped so it stays playable).
        if (score % 10 == 0 && obstacle_speed < 5) obstacle_speed++;
    }

    // Off the left edge → spawn the next one.
    if (obstacle_x + OBSTACLE_W < 0) {
        spawn_obstacle();
    }

    // Collision check.
    if (collides_with_obstacle()) {
        game_over = true;
    }
}

void dino_render(void) {
    // Ground line spans the full width.
    oled_hline(0, GROUND_Y, 128);

    // Dino sprite.
    draw_dino_sprite(DINO_X, dino_y);

    // Obstacle (if on-screen).
    if (obstacle_x < 128 && obstacle_x + OBSTACLE_W > 0) {
        int16_t x0 = obstacle_x < 0 ? 0 : obstacle_x;
        uint8_t w  = (uint8_t)(OBSTACLE_W - (x0 - obstacle_x));
        oled_fill_rect((uint8_t)x0, GROUND_Y - OBSTACLE_H, w, OBSTACLE_H);
    }

    // Game over banner is rendered by the keymap render_oled() dispatcher
    // because it mixes small + big fonts.
}

/* ---- Input ---- */

void dino_input_jump(void) {
    if (game_over) return;
    if (!dino_grounded) return;     // no mid-air double jumps
    dino_v        = JUMP_VELOCITY;
    dino_grounded = false;
}

bool     dino_is_game_over(void) { return game_over; }
uint32_t dino_get_score(void)    { return score; }
