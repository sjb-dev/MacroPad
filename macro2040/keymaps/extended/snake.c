// Copyright 2024 SM Boards (@sm_boards)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Snake game for the 128x32 OLED.
//
// Grid: 31 columns x 7 rows of 4 x 4 px cells, offset 2 px in from the
// top-left so there is room for a 1 px border around the playfield.
// The border makes it obvious where the wall is — without it players
// run off the edge without realising they were about to hit the wall.
//
// Controls (routed from keymap.c when app_mode == APP_GAME_SNAKE):
//   F4 (SW4) → snake_input_left()
//   F2 (SW5) → snake_input_up()
//   F5 (SW6) → snake_input_down()
//   F6 (SW7) → snake_input_right()
//
// Reversing directly into the body is silently ignored — the canonical
// snake rule.

#include "games.h"
#include "oled_custom.h"
#include "timer.h"

#include <string.h>

#define GRID_W       31
#define GRID_H       7
#define CELL_PX      4              // 31 cells * 4 px + 2 px inset + 2 px right = 128
#define GRID_OFFSET_X 2
#define GRID_OFFSET_Y 2
#define MAX_LEN      (GRID_W * GRID_H)
#define STEP_MS      220            // game speed (lower = faster)

typedef struct {
    int8_t x;
    int8_t y;
} cell_t;

static cell_t   body[MAX_LEN];
static uint16_t length;
static int8_t   dir_x;          // current heading (-1, 0, +1)
static int8_t   dir_y;
static int8_t   pending_dx;     // queued heading from latest input
static int8_t   pending_dy;
static cell_t   food;
static uint32_t next_step_ms;
static bool     game_over;
static uint32_t score;
static uint32_t rng_state;      // simple xorshift seed

/* ---- Helpers ---- */

// Tiny xorshift PRNG so food placement isn't deterministic each game.
static uint32_t game_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static bool cell_in_body(int8_t x, int8_t y) {
    for (uint16_t i = 0; i < length; i++) {
        if (body[i].x == x && body[i].y == y) return true;
    }
    return false;
}

static void place_food(void) {
    // Pick a random empty cell. Bounded retries are fine for a 256-cell board.
    for (uint16_t tries = 0; tries < 200; tries++) {
        int8_t x = (int8_t)(game_rand() % GRID_W);
        int8_t y = (int8_t)(game_rand() % GRID_H);
        if (!cell_in_body(x, y)) {
            food.x = x;
            food.y = y;
            return;
        }
    }
    // Board is essentially full → win condition. Just place anywhere.
    food.x = 0;
    food.y = 0;
}

/* ---- Public API ---- */

void snake_start(void) {
    // Seed the PRNG with the millis() counter so each new game differs.
    rng_state = timer_read32() ^ 0xA5A5A5A5u;
    if (rng_state == 0) rng_state = 1;

    length = 3;
    body[0] = (cell_t){10, 4};   // head
    body[1] = (cell_t){ 9, 4};
    body[2] = (cell_t){ 8, 4};

    dir_x = 1; dir_y = 0;        // start moving right
    pending_dx = 1; pending_dy = 0;

    score = 0;
    game_over = false;
    next_step_ms = timer_read32() + STEP_MS;
    place_food();
}

void snake_tick(void) {
    if (game_over) return;
    if (timer_read32() < next_step_ms) return;
    next_step_ms = timer_read32() + STEP_MS;

    // Apply queued direction change (already filtered in snake_input_*).
    dir_x = pending_dx;
    dir_y = pending_dy;

    cell_t new_head = { body[0].x + dir_x, body[0].y + dir_y };

    // Wall collision → death.
    if (new_head.x < 0 || new_head.x >= GRID_W ||
        new_head.y < 0 || new_head.y >= GRID_H) {
        game_over = true;
        return;
    }

    // Self collision → death.  We allow walking onto the very last tail
    // cell because it's about to move out of the way this same tick.
    for (uint16_t i = 0; i < length - 1; i++) {
        if (body[i].x == new_head.x && body[i].y == new_head.y) {
            game_over = true;
            return;
        }
    }

    bool ate_food = (new_head.x == food.x && new_head.y == food.y);

    if (ate_food) {
        // Grow: keep the tail, prepend the new head.
        if (length < MAX_LEN) length++;
        for (uint16_t i = length - 1; i > 0; i--) body[i] = body[i - 1];
        body[0] = new_head;
        score++;
        place_food();
    } else {
        // Move: shift body backwards by one, head becomes new_head.
        for (uint16_t i = length - 1; i > 0; i--) body[i] = body[i - 1];
        body[0] = new_head;
    }
}

void snake_render(void) {
    // 1 px border frame around the full display so the player can see
    // where the wall is.
    oled_hline(0, 0, 128);          // top
    oled_hline(0, 31, 128);         // bottom
    // Vertical edges (1 px wide × 32 tall)
    oled_fill_rect(0, 0, 1, 32);    // left
    oled_fill_rect(127, 0, 1, 32);  // right

    // Snake body — 3 px filled blocks (1 px gap between cells for clarity).
    for (uint16_t i = 0; i < length; i++) {
        oled_fill_rect((uint8_t)(GRID_OFFSET_X + body[i].x * CELL_PX),
                       (uint8_t)(GRID_OFFSET_Y + body[i].y * CELL_PX),
                       3, 3);
    }
    // Food — smaller 2 px block so it visually stands out from the body.
    oled_fill_rect((uint8_t)(GRID_OFFSET_X + food.x * CELL_PX) + 1,
                   (uint8_t)(GRID_OFFSET_Y + food.y * CELL_PX) + 1,
                   2, 2);
}

/* ---- Input ---- */

// Filter: never accept a heading that's a 180° reverse of the CURRENT
// heading. Otherwise the snake would instantly self-collide.
static void try_set_dir(int8_t dx, int8_t dy) {
    if (dx == -dir_x && dy == -dir_y) return;
    pending_dx = dx;
    pending_dy = dy;
}

void snake_input_up(void)    { try_set_dir( 0, -1); }
void snake_input_down(void)  { try_set_dir( 0,  1); }
void snake_input_left(void)  { try_set_dir(-1,  0); }
void snake_input_right(void) { try_set_dir( 1,  0); }

bool     snake_is_game_over(void) { return game_over; }
uint32_t snake_get_score(void)    { return score; }
