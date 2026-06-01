// Copyright 2024 SM Boards (@sm_boards)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Background countdown timer for the 128x32 OLED.
//
// The timer runs in the BACKGROUND: once started it counts down while the
// macropad keeps working normally. The full-screen UI is only shown while
// the user is actively in the timer's setup/control screen, or when the
// alert fires on expiry.
//
// States:
//   SETUP    - user dials the duration with the encoder, F1 starts
//   RUNNING  - counts down in the background (keys/knob work normally)
//   PAUSED   - countdown frozen, can be resumed
//   EXPIRED  - reached zero; full-screen alert + LED flash until acknowledged
//
// Duration is stored in total seconds, displayed as HH:MM:SS. The encoder
// adjusts by 1 second per click while in SETUP.

#include "games.h"
#include "oled_custom.h"
#include "timer.h"

#include <stdio.h>

/* ---- State ---- */

typedef enum {
    TMR_SETUP,
    TMR_RUNNING,
    TMR_PAUSED,
    TMR_EXPIRED,
} tmr_state_t;

static tmr_state_t state       = TMR_SETUP;
static uint32_t    set_seconds = 25 * 60;   // default 25 minutes (remembered)
static uint32_t    remaining   = 0;         // seconds left
static uint32_t    last_tick   = 0;         // timer_read32() at last 1-second step

/* ---- Helpers ---- */

// Format `total` seconds as "HH:MM:SS" into `buf` (>= 9 bytes).
static void fmt_time(char *buf, uint8_t buf_len, uint32_t total) {
    uint32_t h = total / 3600;
    uint32_t m = (total % 3600) / 60;
    uint32_t s = total % 60;
    snprintf(buf, buf_len, "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

void timer_format_remaining(char *buf, uint8_t buf_len) {
    uint32_t h = remaining / 3600;
    uint32_t m = (remaining % 3600) / 60;
    uint32_t s = remaining % 60;
    if (h > 0) {
        snprintf(buf, buf_len, "%lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, buf_len, "%02lu:%02lu",
                 (unsigned long)m, (unsigned long)s);
    }
}

/* ---- Public API ---- */

void timer_open(void) {
    // Opening the UI never disturbs a live/expired timer; it just decides
    // which screen to show. Only when idle do we (re)enter SETUP.
    if (state == TMR_RUNNING || state == TMR_PAUSED || state == TMR_EXPIRED) {
        return;
    }
    state = TMR_SETUP;
}

void timer_start(void) {
    if (set_seconds == 0) return;   // can't start a 0-second timer
    remaining = set_seconds;
    last_tick = timer_read32();
    state     = TMR_RUNNING;
}

void timer_restart(void) {
    timer_start();
}

void timer_pause(void) {
    if (state == TMR_RUNNING) state = TMR_PAUSED;
}

void timer_resume(void) {
    if (state == TMR_PAUSED) {
        last_tick = timer_read32();
        state     = TMR_RUNNING;
    }
}

void timer_stop(void) {
    remaining = 0;
    state     = TMR_SETUP;   // duration (set_seconds) is remembered
}

void timer_tick(void) {
    if (state != TMR_RUNNING) return;
    if (timer_elapsed32(last_tick) < 1000) return;
    last_tick = timer_read32();

    if (remaining > 0) {
        remaining--;
    }
    if (remaining == 0) {
        state = TMR_EXPIRED;
    }
}

void timer_render(void) {
    char buf[16];

    switch (state) {
        case TMR_SETUP:
            oled_write("     SET TIMER\n");
            fmt_time(buf, sizeof(buf), set_seconds);
            oled_write_big(16, 1, buf);
            oled_write("\n\n");
            oled_write("F1:Start      F3:Exit");
            break;

        case TMR_RUNNING:
            oled_write("    TIMER RUNNING\n");
            fmt_time(buf, sizeof(buf), remaining);
            oled_write_big(16, 1, buf);
            oled_write("\n\n");
            oled_write("F1:Pause F2:Rst F3:Stp");
            break;

        case TMR_PAUSED:
            oled_write("    TIMER PAUSED\n");
            fmt_time(buf, sizeof(buf), remaining);
            oled_write_big(16, 1, buf);
            oled_write("\n\n");
            oled_write("F1:Resm F2:Rst F3:Stp");
            break;

        case TMR_EXPIRED:
            oled_write("       TIMER\n");
            oled_write_big(22, 1, "EXPIRED");
            oled_write("\n\n");
            oled_write("F1:Set F2:Rst F3:Exit");
            break;
    }
}

void timer_adjust(int16_t delta_seconds) {
    if (state != TMR_SETUP) return;

    int32_t new_val = (int32_t)set_seconds + delta_seconds;
    // Clamp to 1 second .. 99:59:59 (359999 seconds)
    if (new_val < 1)       new_val = 1;
    if (new_val > 359999)  new_val = 359999;
    set_seconds = (uint32_t)new_val;
}

bool timer_is_setup(void)   { return state == TMR_SETUP; }
bool timer_is_running(void) { return state == TMR_RUNNING; }
bool timer_is_paused(void)  { return state == TMR_PAUSED; }
bool timer_is_active(void)  { return state == TMR_RUNNING || state == TMR_PAUSED; }
bool timer_is_expired(void) { return state == TMR_EXPIRED; }

void timer_acknowledge(void) {
    if (state == TMR_EXPIRED) {
        state = TMR_SETUP;
    }
}
