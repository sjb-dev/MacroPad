VIA_ENABLE = yes
ENCODER_MAP_ENABLE = yes

# Keymap-only extras (on-board games + Pomodoro timer).
# OLED driver + keycode_label helper are added at the keyboard level.
SRC += snake.c
SRC += dino.c
SRC += timer_pomodoro.c
