#ifndef SELFTEST_H
#define SELFTEST_H
#include <stdint.h>

/*
 * Self-test mode: standalone hardware diagnostic (no host required).
 *
 * Entry points:
 *   - hardware : encoder 1 (code 0x37) + encoder 4 (code 0x36) held simultaneously for 5 s
 *                while on the logo/idle screen (g_host_state <= 0)
 *   - UART MSH : 'selftest' command
 *   - host     : CRT SELFT (dispatched by threads.c)
 * Exit: same L+R 5 s combo from any state, or reaching the end of a test.
 *
 * Encoder button physical order, left to right:
 *   1 = code 0x37 (pin PE.11)   2 = code 0x35 (pin PE.15)
 *   3 = code 0x33 (pin PB.8)    4 = code 0x36 (pin PB.11)
 */

extern volatile uint8_t g_selftest_mode;   /* 0 = off, 1 = active */

/* called by input.c (both threads), touch.c */
void selftest_key_event(uint8_t code, uint8_t state);
void selftest_touch_event(uint8_t code, uint8_t state, uint16_t x, uint16_t y);

/* called from the keyboard thread every 30 ms to drive the display-test sequence */
void selftest_tick(void);

/* combo watchdog: called from the EC11 thread with the raw button states */
void selftest_combo_poll(uint32_t ms_now, uint8_t left_down, uint8_t right_down);

/* activate / deactivate (also called by UART command and host command) */
void selftest_start(void);
void selftest_stop(void);

#endif /* SELFTEST_H */
