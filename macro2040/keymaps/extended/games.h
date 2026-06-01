// Copyright 2024 SM Boards (@sm_boards)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tiny OLED games for the Macro2040.
//
// Two games are provided:
//   - Snake (4-direction key control)
//   - Dino runner (single-key jump)
//
// Each game exposes the same three-function interface:
//   <game>_start()  - reset state and begin a new game
//   <game>_tick()   - advance one timestep (called from housekeeping_task_user)
//   <game>_render() - draw the current frame into the OLED framebuffer
//                     (caller is responsible for oled_clear() before
//                     and oled_flush() after)
//
// Plus a small set of input helpers — the keymap routes key/encoder events
// here when the application is in the matching mode.
//
// All games run entirely on the OLED framebuffer using the primitives
// declared in oled_custom.h. They never touch QMK's HID stack.

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ---- Snake ---- */

void snake_start(void);
void snake_tick(void);
void snake_render(void);

// Direction input (called from process_record_user when in snake mode).
// Each function queues a heading change for the next tick. Reversing
// directly into the snake's body is silently ignored.
void snake_input_up(void);
void snake_input_down(void);
void snake_input_left(void);
void snake_input_right(void);

// Returns true if the player has died and the game is waiting for any
// key press to restart.
bool snake_is_game_over(void);

// Current snake score (number of food items eaten this run).
uint32_t snake_get_score(void);

/* ---- Dino runner ---- */

void dino_start(void);
void dino_tick(void);
void dino_render(void);

// Single-key jump input.
void dino_input_jump(void);

// Returns true if the player has crashed and the game is waiting for any
// key press to restart.
bool dino_is_game_over(void);

// Current dino score (number of obstacles dodged this run).
uint32_t dino_get_score(void);

/* ---- Countdown Timer ---- */
//
// The timer runs in the BACKGROUND: once started it counts down while the
// macropad is used normally. The full-screen timer UI is only shown while
// the user is in the setup / control screen (reached via the games menu) or
// when the alert fires on expiry. See keymap.c for how these are wired.

// Open the timer UI. If a timer is already running/paused/expired that state
// is kept (control / alert screen shown); otherwise the setup screen opens.
void timer_open(void);

// Start counting down from the set duration (SETUP -> RUNNING).
void timer_start(void);

// Restart: reset to the set duration and start again.
void timer_restart(void);

// Pause / resume the running countdown.
void timer_pause(void);
void timer_resume(void);

// Cancel the timer entirely and return to the setup state.
void timer_stop(void);

// Tick the countdown. Safe to call every main-loop iteration; only counts
// down while RUNNING, and triggers EXPIRED at zero.
void timer_tick(void);

// Render the full-screen timer UI (setup / running / paused / expired) into
// the OLED framebuffer. Caller handles oled_clear() / oled_flush().
void timer_render(void);

// Write the remaining time as a compact "MM:SS" (or "H:MM:SS") string for the
// small corner indicator shown during normal use.
void timer_format_remaining(char *buf, uint8_t buf_len);

// Adjust the set duration by `delta_seconds`. Only effective in SETUP.
// Clamped to 00:00:01 .. 99:59:59.
void timer_adjust(int16_t delta_seconds);

// State queries.
bool timer_is_setup(void);    // dialing a new duration
bool timer_is_running(void);  // counting down
bool timer_is_paused(void);   // paused mid-countdown
bool timer_is_active(void);   // running OR paused (a timer "exists")
bool timer_is_expired(void);  // reached zero, alert showing

// Acknowledge the "TIMER EXPIRED" alert (return to SETUP, stop LED flash).
void timer_acknowledge(void);
