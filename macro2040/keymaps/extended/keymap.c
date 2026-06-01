// Copyright 2024 SM Boards (@sm_boards)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Extended keymap for the SM Boards Macro2040 — games, Pomodoro timer, and
// the rich OLED UI on top of VIA. Not submitted upstream; this is the
// product firmware. The PR-bound simple VIA keymap lives in keymaps/via/.
//
// Hardware highlights this keymap deals with:
//   - 6 mechanical key matrix (rows 0..2, cols 0..1) plus a 7th tactile
//     switch SW9 at matrix [3,1] used as a layer-cycle button.
//   - AS5600 magnetic rotary encoder over I2C0. Reading + virtual quadrature
//     synthesis lives at the keyboard level in macro2040.c; here we only
//     react to encoder events in process_record_user (KEYLOC_ENCODER_CW/CCW).
//   - WS2812 RGB matrix on GP17 (via level shifter). All effects/animations
//     and colour are configured through Via.
//   - 6 blue switch indicator LEDs driven via 2 NPN transistors on GP12/GP13.
//   - 1 user LED (D6) on GP25, used as a "device alive" + layer-change blink.
//   - SSD1306 0.91" 128x32 OLED on I2C1, driven by oled_custom.c.

#include QMK_KEYBOARD_H
#include "oled_custom.h"
#include "hal.h"
#include "dynamic_keymap.h"
#include "keycode_label.h"
#include "games.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
   Forward declarations (kept up here so init can call helpers
   that are defined further down).
   ============================================================ */

static inline void d6_on(void);
static inline void d6_off(void);

/* ============================================================
   Layers and custom keycodes
   ============================================================ */

enum layers {
    _LAYER1,    // L:1  Volume & media controls (default)
    _LAYER2,    // L:2  Editing shortcuts (Copy/Paste/Find/Undo/Redo/Save)
    _LAYER3,    // L:3  Zoom / find / save / print
    _LAYER4     // L:4  Vertical scroll / navigation
};

enum custom_keycodes {
    LAYER_CYCLE = QK_USER_0   // bound to SW9: cycles through L:1..L:4
};

/* ============================================================
   Keymaps
   ------------------------------------------------------------
   Each LAYOUT() takes 7 arguments — the 6 keys in the matrix
   plus SW9 (LAYER_CYCLE) at matrix [3,1].
     SW3   SW5   SW7
     SW4   SW6   SW8   + SW9 (cycle layer)
   ============================================================ */

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    // L:1 — Volume / media (default layer on power-up)
    [_LAYER1] = LAYOUT(
        KC_MUTE, KC_VOLU, KC_MPLY,
        KC_VOLD, KC_MPRV, KC_MNXT, LAYER_CYCLE
    ),
    // L:2 — Editing shortcuts
    //   Copy(Ctl+C)  Paste(Ctl+V)  Find(Ctl+F)
    //   Undo(Ctl+Z)  Redo(Ctl+Y)   Save(Ctl+S)
    [_LAYER2] = LAYOUT(
        LCTL(KC_C),     LCTL(KC_V), LCTL(KC_F),
        LCTL(KC_Z),     LCTL(KC_Y), LCTL(KC_S), LAYER_CYCLE
    ),
    // L:3 — Zoom / find / print / save shortcuts
    [_LAYER3] = LAYOUT(
        LCTL(KC_0),     LCTL(KC_EQUAL), LCTL(KC_P),
        LCTL(KC_MINUS), LCTL(KC_F),     LCTL(KC_S), LAYER_CYCLE
    ),
    // L:4 — Vertical navigation
    [_LAYER4] = LAYOUT(
        KC_PGUP, KC_UP,   KC_HOME,
        KC_PGDN, KC_DOWN, KC_END, LAYER_CYCLE
    ),
};

#if defined(ENCODER_MAP_ENABLE)
// Encoder bindings per layer. ENCODER_CCW_CW(ccw, cw) — first arg is the
// counter-clockwise action, second is clockwise.
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [_LAYER1] = { ENCODER_CCW_CW(KC_VOLD,         KC_VOLU)         }, // volume
    [_LAYER2] = { ENCODER_CCW_CW(LCTL(KC_Z),      LCTL(KC_Y))      }, // undo/redo
    [_LAYER3] = { ENCODER_CCW_CW(LCTL(KC_MINUS),  LCTL(KC_EQUAL))  }, // zoom
    [_LAYER4] = { ENCODER_CCW_CW(KC_WH_D,         KC_WH_U)         }, // wheel
};
#endif

// Short labels shown on the OLED top line for each layer.
static const char *layer_names[] = {
    "L:1", "L:2", "L:3", "L:4"
};

/* ============================================================
   Application mode
   ------------------------------------------------------------
   Top-level state machine for what the macropad is currently
   doing. APP_NORMAL is the regular keyboard/macropad behavior;
   the three APP_GAME_* modes take over the OLED and intercept
   keys for on-board mini-games (see games.h / snake.c / dino.c).

   Transitions:
     - LONG PRESS SW9  (APP_NORMAL)          -> APP_GAME_MENU
     - short SW9       (APP_NORMAL)          -> cycle layer
     - SW3 press       (APP_GAME_MENU)       -> APP_GAME_SNAKE
     - SW5 press       (APP_GAME_MENU)       -> APP_GAME_DINO
     - LONG PRESS SW9  (any APP_GAME_*)      -> APP_NORMAL
     - any key press   (game over screen)    -> restart same game
   ============================================================ */

typedef enum {
    APP_NORMAL,
    APP_GAME_MENU,
    APP_GAME_SNAKE,
    APP_GAME_DINO,
    APP_TIMER,          // countdown timer (setup / running / expired)
} app_mode_t;

static app_mode_t app_mode = APP_NORMAL;

#define SW9_LONG_PRESS_MS 500
static uint32_t sw9_press_ts  = 0;      // timer_read32() when SW9 went down
static bool     sw9_held      = false;
static bool     sw9_long_fired = false; // long-press action already consumed

static void enter_normal_mode(void) {
    app_mode = APP_NORMAL;
}
static void enter_game_menu(void) {
    app_mode = APP_GAME_MENU;
}

/* ============================================================
   OLED state machine
   ------------------------------------------------------------
   The OLED has two display modes:
     - IDLE   : scroll the SamSteve / Macro2040 banner
     - ACTION : show the last key / encoder / layer label in
                big font for ACTION_DISPLAY_MS milliseconds
   show_action() is called from key, encoder and layer handlers.
   render_oled() flips back to IDLE once action_until expires.
   ============================================================ */

#define ACTION_DISPLAY_MS 2500   // ms to show a key/encoder/layer label

typedef enum {
    OLED_MODE_IDLE,    // scrolling banner
    OLED_MODE_ACTION   // big-font label of the most recent action
} oled_mode_t;

static oled_mode_t oled_mode = OLED_MODE_IDLE;
static char        action_text[16] = "";
static uint32_t    action_until    = 0;

// Display `text` in big font for ACTION_DISPLAY_MS milliseconds, then
// fall back to the idle banner.
static void show_action(const char *text) {
    strncpy(action_text, text, sizeof(action_text) - 1);
    action_text[sizeof(action_text) - 1] = '\0';
    action_until = timer_read32() + ACTION_DISPLAY_MS;
    oled_mode    = OLED_MODE_ACTION;
}

/* ============================================================
   Keycode → human-readable label
   ------------------------------------------------------------
   The shared lookup lives in `keycode_label.c` at the keyboard
   level (used by macro2040.c's basic OLED too). This file only
   provides the custom-keycode hook for LAYER_CYCLE.
   ============================================================ */

const char *keycode_label_user(uint16_t kc) {
    if (kc == LAYER_CYCLE) return "Lyr+";
    return NULL;   // fall back to the standard table
}

// Look up the keycode currently bound at [row,col] on the active layer.
// Goes through the dynamic keymap so Via remappings are reflected.
static uint16_t current_keycode(uint8_t row, uint8_t col) {
    uint8_t layer = get_highest_layer(layer_state);
    return dynamic_keymap_get_keycode(layer, row, col);
}

/* ---- Physical key → matrix position macros ---- */
// F1..F6 are the six main keys, row-major (top row left-to-right, then
// bottom row left-to-right). F7 = SW9, the small tactile button.
// These correspond to the physical F1..F6 labels on the keycaps.
#define IS_F1(r, c) ((r) == 0 && (c) == 0)   // SW3 — top-left
#define IS_F2(r, c) ((r) == 1 && (c) == 0)   // SW5 — top-middle
#define IS_F3(r, c) ((r) == 2 && (c) == 0)   // SW7 — top-right
#define IS_F4(r, c) ((r) == 0 && (c) == 1)   // SW4 — bottom-left (game-over Back)
#define IS_F5(r, c) ((r) == 1 && (c) == 1)   // SW6 — bottom-middle
#define IS_F6(r, c) ((r) == 2 && (c) == 1)   // SW8 — bottom-right (game-over Restart)

// Route a keypress in APP_GAME_SNAKE to the snake input API (live gameplay).
//   [ F1 ]  [ F2 ↑]  [ F3 ]
//   [ F4 ←] [ F5 ↓]  [ F6 →]
static void route_key_to_snake(uint8_t row, uint8_t col) {
    if (IS_F4(row, col)) snake_input_left();
    if (IS_F2(row, col)) snake_input_up();
    if (IS_F5(row, col)) snake_input_down();
    if (IS_F6(row, col)) snake_input_right();
}

// Route a keypress in APP_GAME_DINO to the dino jump input.
static void route_key_to_dino(uint8_t row, uint8_t col) {
    // SW5 (1,0) = Jump
    if (row == 1 && col == 0) dino_input_jump();
}

// On game-over: F4 returns to the menu, F6 restarts the same game.
// Any other key is ignored so players can't accidentally restart.
static void handle_game_over_input(uint8_t row, uint8_t col) {
    if (IS_F4(row, col)) {
        enter_game_menu();
    } else if (IS_F6(row, col)) {
        if (app_mode == APP_GAME_SNAKE) snake_start();
        if (app_mode == APP_GAME_DINO)  dino_start();
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    // Encoder rotation arrives here as KEYLOC_ENCODER_CW/CCW (col = encoder
    // index). With ENCODER_MAP_ENABLE on, this is the ONLY hook QMK calls for
    // encoder events — encoder_update_user is BYPASSED — so the per-mode
    // reactions all live here.
    if (record->event.key.row == KEYLOC_ENCODER_CW
        || record->event.key.row == KEYLOC_ENCODER_CCW) {
        bool clockwise = (record->event.key.row == KEYLOC_ENCODER_CW);
        // Each step fires both a press and a release; only act on the press.
        if (record->event.pressed) {
            // Timer SETUP: knob dials the duration (1 sec/click), no HID.
            if (app_mode == APP_TIMER && timer_is_setup()) {
                timer_adjust(clockwise ? +1 : -1);
                return false;
            }
            // Normal use: flash the bound encoder label on the OLED.
            if (app_mode == APP_NORMAL && !timer_is_expired()) {
                uint8_t  lyr = get_highest_layer(layer_state);
                uint16_t kc  = dynamic_keymap_get_encoder(lyr, record->event.key.col, clockwise);
                show_action(keycode_label(kc));
            }
        }
        // Let HID through only in normal use; block in games / timer UI / alert.
        return (app_mode == APP_NORMAL && !timer_is_expired());
    }

    /* ---- Timer expiry: a global modal alert that overrides every mode ---- */
    if (timer_is_expired()) {
        if (!record->event.pressed) return false;
        if (IS_F1(record->event.key.row, record->event.key.col)) {
            timer_acknowledge();      // Set: back to the setup screen
            app_mode = APP_TIMER;
        } else if (IS_F2(record->event.key.row, record->event.key.col)) {
            timer_acknowledge();
            timer_start();            // Restart: same duration, in background
            enter_normal_mode();
        } else if (IS_F3(record->event.key.row, record->event.key.col)) {
            timer_acknowledge();      // Dismiss
            enter_normal_mode();
        }
        return false;
    }

    /* ---- SW9 press/release: detect short vs long press ---- */
    if (keycode == LAYER_CYCLE) {
        if (record->event.pressed) {
            sw9_press_ts   = timer_read32();
            sw9_held       = true;
            sw9_long_fired = false;
            return false;
        }

        // Released.
        sw9_held = false;
        if (sw9_long_fired) {
            // The long-press action was already fired by matrix_scan_user
            // the moment the threshold was crossed.
            return false;
        }
        // Short press: what it does depends on the current app mode.
        if (app_mode == APP_NORMAL) {
            uint8_t current = get_highest_layer(layer_state);
            uint8_t next    = (current + 1) % 4;
            layer_move(next);
            show_action(layer_names[next]);
        }
        // In game menu or any game, a short SW9 does nothing.
        return false;
    }

    /* ---- Non-SW9 key ---- */
    if (!record->event.pressed) return true;

    switch (app_mode) {
        case APP_NORMAL: {
            uint16_t kc = current_keycode(record->event.key.row,
                                          record->event.key.col);
            show_action(keycode_label(kc));
            return true;   // let the HID event through
        }
        case APP_GAME_MENU: {
            // F1 = Snake, F2 = Dino, F3 = Timer
            if (IS_F1(record->event.key.row, record->event.key.col)) {
                snake_start();
                app_mode = APP_GAME_SNAKE;
            } else if (IS_F2(record->event.key.row, record->event.key.col)) {
                dino_start();
                app_mode = APP_GAME_DINO;
            } else if (IS_F3(record->event.key.row, record->event.key.col)) {
                timer_open();
                app_mode = APP_TIMER;
            }
            return false;
        }
        case APP_GAME_SNAKE: {
            if (snake_is_game_over()) {
                handle_game_over_input(record->event.key.row,
                                       record->event.key.col);
            } else {
                route_key_to_snake(record->event.key.row, record->event.key.col);
            }
            return false;
        }
        case APP_GAME_DINO: {
            if (dino_is_game_over()) {
                handle_game_over_input(record->event.key.row,
                                       record->event.key.col);
            } else {
                route_key_to_dino(record->event.key.row, record->event.key.col);
            }
            return false;
        }
        case APP_TIMER: {
            // Expiry is handled by the global alert above, so here the timer
            // is SETUP / RUNNING / PAUSED — the setup or control screen.
            if (timer_is_setup()) {
                // SETUP: F1 = Start (then run in background), F3 = back to menu.
                if (IS_F1(record->event.key.row, record->event.key.col)) {
                    timer_start();
                    enter_normal_mode();
                } else if (IS_F3(record->event.key.row, record->event.key.col)) {
                    enter_game_menu();
                }
                return false;
            }
            // CONTROL screen for a live timer:
            //   F1 = Pause/Resume, F2 = Restart, F3 = Stop (cancel) -> menu.
            // After Pause/Restart/Resume we drop back to background use.
            if (IS_F1(record->event.key.row, record->event.key.col)) {
                if (timer_is_running())     timer_pause();
                else if (timer_is_paused()) timer_resume();
                enter_normal_mode();
            } else if (IS_F2(record->event.key.row, record->event.key.col)) {
                timer_restart();
                enter_normal_mode();
            } else if (IS_F3(record->event.key.row, record->event.key.col)) {
                timer_stop();
                enter_game_menu();
            }
            return false;
        }
    }
    return true;
}

/* ============================================================
   Blue switch indicator LEDs
   ------------------------------------------------------------
   The 6 blue LEDs are wired in two columns of 3, each column
   driven through an NPN transistor whose base is one GPIO:
     GP13 (LED_COL0_PIN) → SW3B, SW5B, SW7B (left column)
     GP12 (LED_COL1_PIN) → SW4B, SW6B, SW8B (right column)
   We just keep both columns ON all the time; the WS2812 RGB
   LEDs are the ones that react to layer/encoder activity.
   ============================================================ */

void update_layer_leds(void) {
    palSetPad(PAL_PORT(LED_COL0_PIN), PAL_PAD(LED_COL0_PIN));
    palSetPad(PAL_PORT(LED_COL1_PIN), PAL_PAD(LED_COL1_PIN));
}

/* ============================================================
   One-time initialisation called by QMK after the core is up
   ============================================================ */

void keyboard_post_init_user(void) {
    // The AS5600 encoder (I2C init + quadrature output/input pins) is set up
    // at the keyboard level in macro2040.c's keyboard_post_init_kb(), which
    // runs before this. Here we only init the keymap-owned peripherals.

    // Blue switch LED columns — push-pull outputs with maximum drive
    // strength because the transistors need a good base current.
    palSetPadMode(PAL_PORT(LED_COL0_PIN), PAL_PAD(LED_COL0_PIN),
                  PAL_MODE_OUTPUT_PUSHPULL | PAL_RP_PAD_DRIVE12);
    palSetPadMode(PAL_PORT(LED_COL1_PIN), PAL_PAD(LED_COL1_PIN),
                  PAL_MODE_OUTPUT_PUSHPULL | PAL_RP_PAD_DRIVE12);
    update_layer_leds();

    // D6 user LED on GP25.
    // Wiring: +3V3 → R33 → LED anode → cathode → GP25.
    // The LED is therefore active-LOW: GP25 LOW = LED ON.
    palSetPadMode(PAL_PORT(USER_LED_PIN), PAL_PAD(USER_LED_PIN),
                  PAL_MODE_OUTPUT_PUSHPULL);
    d6_on();

    // OLED init runs at the keyboard level (macro2040.c::keyboard_post_init_kb)
    // before this, so we no longer call oled_init() here.
}

/* ============================================================
   D6 user LED (GP25) layer-change blink
   ------------------------------------------------------------
   On every layer change we blink D6 N times (N = layer index +
   1, so L:1 → 1 blink, L:4 → 4 blinks) and then leave it solid
   ON as a "device is alive" indicator.
   The blinker is non-blocking — it stores how many half-cycles
   remain and a timestamp for the next state flip, and is
   ticked from housekeeping_task_user() once per main loop.
   ============================================================ */

#define BLINK_ON_MS  150
#define BLINK_OFF_MS 150

static uint8_t  blink_remaining = 0;  // half-cycles left (1 ON + 1 OFF per blink)
static uint32_t blink_next_ms   = 0;  // when to flip the LED state next
static bool     blink_led_state = false;

// D6 is active-low: GP25 LOW = LED on, GP25 HIGH = LED off.
static inline void d6_on(void)  { palClearPad(PAL_PORT(USER_LED_PIN), PAL_PAD(USER_LED_PIN)); }
static inline void d6_off(void) { palSetPad(PAL_PORT(USER_LED_PIN),   PAL_PAD(USER_LED_PIN)); }

// Kick off a (layer_index + 1)-blink sequence on D6.
static void start_layer_blink(uint8_t layer_index) {
    uint8_t blinks  = layer_index + 1;
    blink_remaining = blinks * 2;  // each visible blink = ON edge + OFF edge
    blink_next_ms   = timer_read32();
    blink_led_state = false;
    d6_off();
}

// Drive the blink state machine. Called every main-loop iteration.
static void update_blink(void) {
    if (blink_remaining == 0) return;
    if (timer_read32() < blink_next_ms) return;

    blink_led_state = !blink_led_state;
    if (blink_led_state) {
        d6_on();
        blink_next_ms = timer_read32() + BLINK_ON_MS;
    } else {
        d6_off();
        blink_next_ms = timer_read32() + BLINK_OFF_MS;
    }
    blink_remaining--;

    // Sequence complete → leave D6 solid ON as the "device alive" indicator.
    if (blink_remaining == 0) {
        d6_on();
    }
}

/* ============================================================
   Layer state hook — runs whenever the active layer changes
   ============================================================ */

layer_state_t layer_state_set_user(layer_state_t state) {
    update_layer_leds();
    uint8_t layer = get_highest_layer(state);
    if (layer > 3) layer = 0;
    start_layer_blink(layer);
    return state;
}

/* ============================================================
   Per-scan keymap housekeeping
   ------------------------------------------------------------
   The AS5600 magnetic encoder is read and converted to quadrature
   pulses at the KEYBOARD level (macro2040.c). QMK turns those into
   encoder events that arrive at process_record_user() above (since
   ENCODER_MAP_ENABLE bypasses encoder_update_user). All that remains
   here is the SW9 long-press detection.
   ============================================================ */

void matrix_scan_user(void) {
    // SW9 long-press: toggle in/out of the games & tools menu.
    if (sw9_held && !sw9_long_fired &&
        timer_elapsed32(sw9_press_ts) >= SW9_LONG_PRESS_MS) {
        sw9_long_fired = true;
        if (app_mode == APP_NORMAL) enter_game_menu();
        else                        enter_normal_mode();
    }
}

/* ============================================================
   OLED render (128 x 32)
   ------------------------------------------------------------
   Layout:
     Page 0 (top, small font) : "SM Macro2040 L:N"
     Page 2-3 (bottom, big)   : action label  OR  scrolling banner
   render_oled() runs at 10 Hz from housekeeping_task_user(),
   which gives a ~10 px/sec smooth scroll for the idle banner.
   ============================================================ */

// Compute the X coordinate that horizontally centers a big-font string of
// `nchars` characters on the 128 px display (12 px per character cell).
static uint8_t big_center_x(uint8_t nchars) {
    uint16_t w = nchars * 12;
    if (w >= OLED_DISP_WIDTH) return 0;
    return (OLED_DISP_WIDTH - w) / 2;
}

// Idle banner: scrolls horizontally across the bottom big-font region.
// Asterisks act as separators so the loop visibly repeats.
#define BANNER_TEXT     "SamSteve  *  Macro2040  *  "
#define BANNER_SPEED_PX 1   // px per render tick (10 px/s at 10 Hz refresh)

// Current X position of the banner (signed; can go negative as it scrolls).
static int16_t banner_x = OLED_DISP_WIDTH;  // start just off the right edge

// Render the normal keyboard display (board name + layer + action/banner).
static void render_normal(void) {
    // ACTION mode automatically expires after ACTION_DISPLAY_MS.
    if (oled_mode == OLED_MODE_ACTION && timer_read32() > action_until) {
        oled_mode = OLED_MODE_IDLE;
    }

    uint8_t layer = get_highest_layer(layer_state);
    if (layer > 3) layer = 0;

    // Top line: normally the board name + layer. While a background timer is
    // active, show the layer + a compact countdown (a " P " marks paused).
    if (timer_is_active()) {
        char tbuf[10];
        timer_format_remaining(tbuf, sizeof(tbuf));
        oled_write(layer_names[layer]);
        oled_write(timer_is_paused() ? " P " : "   ");
        oled_write(tbuf);
    } else {
        oled_write("SM Macro2040 ");
        oled_write(layer_names[layer]);
    }

    if (oled_mode == OLED_MODE_ACTION) {
        // Bottom region: centered big-font label of the most recent action.
        uint8_t len = (uint8_t)strlen(action_text);
        if (len > 10) len = 10;
        uint8_t x = big_center_x(len);
        oled_write_big(x, 2, action_text);
    } else {
        // Bottom region: scrolling SamSteve banner.
        oled_write_big_scroll(banner_x, 2, BANNER_TEXT);
        banner_x -= BANNER_SPEED_PX;
        // When the whole banner has scrolled off the left edge, restart
        // it from just off the right edge.
        int16_t banner_w = (int16_t)oled_big_text_width(BANNER_TEXT);
        if (banner_x + banner_w < 0) {
            banner_x = OLED_DISP_WIDTH;
        }
    }
}

// Game/tools menu: 4 small-font lines with three options + exit hint.
static void render_game_menu(void) {
    oled_write("    GAMES / TOOLS\n");
    oled_write(" F1:Snake    F2:Dino\n");
    oled_write(" F3:Timer\n");
    oled_write(" Long F7: Exit");
}

// Game-over overlay. Four small-font lines: title, centered score,
// back key, restart key. Long-press F7 (SW9) still exits to normal mode
// — the hint is omitted here because it's documented in the game menu.
static void render_game_over(uint32_t score) {
    char buf[24];
    oled_write("     GAME OVER\n");
    // "Score: N" centered on line 2. At 6 px per char, 21 columns fit.
    uint8_t n_digits = 1;
    uint32_t tmp = score;
    while (tmp >= 10) { n_digits++; tmp /= 10; }
    uint8_t text_len = 7 + n_digits;   // "Score: " + digits
    uint8_t pad = (21 > text_len) ? (21 - text_len) / 2 : 0;
    for (uint8_t i = 0; i < pad; i++) oled_write(" ");
    snprintf(buf, sizeof(buf), "Score: %lu\n", (unsigned long)score);
    oled_write(buf);
    oled_write(" F4: Back\n");
    oled_write(" F6: Restart");
}

static void render_oled(void) {
    oled_clear();

    // Timer expiry is a global full-screen alert that overrides every mode.
    if (timer_is_expired()) {
        timer_render();
        oled_flush();
        return;
    }

    switch (app_mode) {
        case APP_NORMAL:
            render_normal();
            break;

        case APP_GAME_MENU:
            render_game_menu();
            break;

        case APP_GAME_SNAKE:
            if (snake_is_game_over()) {
                render_game_over(snake_get_score());
            } else {
                snake_render();
            }
            break;

        case APP_GAME_DINO:
            if (dino_is_game_over()) {
                render_game_over(dino_get_score());
            } else {
                dino_render();
            }
            break;

        case APP_TIMER:
            // The timer's setup / control screen owns the whole display.
            timer_render();
            break;
    }

    oled_flush();
}

/* ============================================================
   Per-loop housekeeping — drives the OLED refresh & D6 blinker
   ============================================================ */

void housekeeping_task_user(void) {
    update_blink();

    // Tick the active game; the countdown timer ticks in EVERY mode because
    // it now runs in the background.
    if (app_mode == APP_GAME_SNAKE) snake_tick();
    if (app_mode == APP_GAME_DINO)  dino_tick();
    timer_tick();

    // Timer expiry flashes the blue LEDs regardless of the current mode.
    // (RGB matrix stays on its normal Via-controlled animation.)
    if (timer_is_expired()) {
        static uint32_t last_flash = 0;
        static bool     flash_on   = false;
        if (timer_elapsed32(last_flash) >= 200) {
            last_flash = timer_read32();
            flash_on   = !flash_on;
            if (flash_on) {
                palSetPad(PAL_PORT(LED_COL0_PIN), PAL_PAD(LED_COL0_PIN));
                palSetPad(PAL_PORT(LED_COL1_PIN), PAL_PAD(LED_COL1_PIN));
            } else {
                palClearPad(PAL_PORT(LED_COL0_PIN), PAL_PAD(LED_COL0_PIN));
                palClearPad(PAL_PORT(LED_COL1_PIN), PAL_PAD(LED_COL1_PIN));
            }
        }
    } else {
        update_layer_leds();   // solid ON whenever not flashing
    }

    // OLED refresh: faster while a full-screen UI (games / timer screen /
    // expiry alert) is up; slower during normal background use.
    static uint32_t last_oled_update = 0;
    bool full_ui = (app_mode != APP_NORMAL) || timer_is_expired();
    uint32_t refresh_ms = full_ui ? 33 : 100;
    if (timer_elapsed32(last_oled_update) >= refresh_ms) {
        last_oled_update = timer_read32();
        render_oled();
    }
}
