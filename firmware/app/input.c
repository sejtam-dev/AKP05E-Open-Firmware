/*
 * Buttons: 3 x 5 key matrix, 4 rotary encoders (EC11) with push buttons.
 * Rebuilt from stock 0x402429aa (pin setup), 0x40242af8 (column scan), 0x40242ea2 (matrix), 0x40242c54,
 * 0x40242b98 (encoder), 0x40242cd8 (encoder push buttons) and the thread bodies 0x4024321c / 0x40242e24.
 *
 * Reports to the host are 512 byte "ACK..OK" frames, byte 9 = code, byte 10 = state (1 down / 0 up).
 */
#include "akp05_app.h"
#include <string.h>
#include <aic_osal.h>

#define PIN_OUT  PIN_MODE_OUTPUT
#define PIN_IN   PIN_MODE_INPUT_PULLUP

static const char *const row_pins[3]  = { "PC.0", "PB.2", "PB.1" };
static const char *const col_pins[5]  = { "PC.5", "PC.4", "PC.3", "PC.2", "PC.1" };
static const char *const misc_pins[]  = { "PE.15", "PB.6", "PB.7", "PB.8", "PB.9", "PB.10", "PE.11",
                                           "PE.12", "PE.14", "PB.11", "PA.4", "PA.5" };

/* stock 0x402933cc: key code table, index = column index + row offset (10, 5, 0) */
static const uint8_t key_code_tbl[16] = { 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
/* stock 0x402933b8: column pattern (value - 15) -> column index 1..5 */
static const uint8_t col_index_tbl[16] = { 5, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 3, 0, 2, 1 };

/* ---- GPIO helpers: stock 0x40243e9e / 0x40243eb6 / 0x40243edc */
static void pin_set_mode(const char *name, int mode)
{
    rt_pin_mode(rt_pin_get(name), mode);
}

static void pin_drive(const char *name, int level)
{
    rt_base_t pin = rt_pin_get(name);

    rt_pin_mode(pin, PIN_OUT);
    rt_pin_write(pin, level);
}

static int pin_level(const char *name)
{
    return rt_pin_read(rt_pin_get(name)) & 0xff;
}

/* INIT_APP_EXPORT: stock 0x402429aa */
int pins_init(void)
{
    int i;

    for (i = 0; i < 3; i++)
        pin_set_mode(row_pins[i], PIN_OUT);
    for (i = 0; i < 3; i++)
        pin_drive(row_pins[i], 0);
    for (i = 0; i < 5; i++)
        pin_set_mode(col_pins[i], PIN_IN);
    for (i = 0; i < (int)(sizeof(misc_pins) / sizeof(misc_pins[0])); i++)
        pin_set_mode(misc_pins[i], PIN_IN);
    return 0;
}
INIT_APP_EXPORT(pins_init);

/* ---- key matrix -------------------------------------------------------------------------------- */
/* Column scan, stock 0x40242af8: returns the column index 1..5 of the pressed key, 0 = none. */
static int scan_columns(void)
{
    uint8_t v;

    v  = (uint8_t)(pin_level(col_pins[0]) << 4);
    v |= (uint8_t)(pin_level(col_pins[1]) << 3);
    v |= (uint8_t)(pin_level(col_pins[2]) << 2);
    v |= (uint8_t)(pin_level(col_pins[3]) << 1);
    v |= (uint8_t)pin_level(col_pins[4]);
    if (v == 0x1f)
        return 0;
    rt_thread_mdelay(10);
    if ((uint8_t)(v - 15) > 15)
        return 0;
    return col_index_tbl[v - 15];
}

static void wait_release(void)
{
    while (scan_columns() != 0)
        rt_thread_mdelay(10);
}

/* One pressed key of the matrix, the logic of stock 0x40242ea2 (identical for the three rows). */
static void matrix_key_pressed(int idx, int row_offset)
{
    uint8_t code = key_code_tbl[idx + row_offset];

    if (g_backlight_on == 1) {
        if (g_host_state == 1) {
            ack_key(code, 1);
            wait_release();
            ack_key(code, 0);
            return;
        }
        if (g_host_state + 1 > 1)
            return;
    } else if (g_backlight_on == 0) {
        if (g_host_state + 1 > 1) {
            if (g_host_state != 1)
                return;
            /* host connected, backlight off: wake up and report the key (released) */
            wait_release();
            if (g_backlight_on != 0)
                return;
            ack_key(code, 0);
            backlight_set(80);
            g_backlight_on = 1;
            return;
        }
    } else {
        return;
    }

    /* no host: the keys only toggle the backlight */
    wait_release();
    if (g_backlight_on == 0) {
        backlight_set(80);
        g_backlight_on = 1;
    } else {
        backlight_off();
        g_backlight_on = 0;
    }
}

static void key_matrix_scan(void)
{
    static const int row_offset[3] = { 10, 5, 0 };
    int r, idx;

    pin_drive(row_pins[0], 0);
    for (r = 0; r < 3; r++) {
        idx = scan_columns();
        if (idx)
            matrix_key_pressed(idx, row_offset[r]);
        pin_drive(row_pins[r], 1);
        if (r < 2)
            pin_drive(row_pins[r + 1], 0);
    }
}

void input_threads_entry_keyboard(void *p)
{
    (void)p;
    for (;;) {
        key_matrix_scan();
        if (g_host_state == 1)
            touch_poll();
        rt_thread_mdelay(30);
    }
}

/* ---- rotary encoders ---------------------------------------------------------------------------- */
struct knob {
    uint8_t  b_at_fall;      /* level of B when A fell                   (node byte 0) */
    uint8_t  b_low;          /* B low when A rose                        (node byte 1) */
    uint8_t  armed;          /* A fell, waiting for it to rise           (node byte 2) */
    uint8_t  pad;
    uint8_t  id;             /* report code: id = counter-clockwise, id + 1 = clockwise */
    const char *pin_a;
    const char *pin_b;
    struct knob *next;
};

/* stock 0x402933c8 and 0x402b3a90 */
static const uint8_t knob_ids[4] = { 0x50, 0x90, 0xa0, 0x70 };
static const char *const knob_pins[4][2] = {
    { "PB.6", "PB.7" }, { "PB.9", "PB.10" }, { "PE.12", "PE.14" }, { "PA.4", "PA.5" },
};

static struct knob *knob_list_create(int n)           /* stock 0x40242c54 */
{
    struct knob *head = RT_NULL, *tail = RT_NULL;
    int i;

    for (i = 0; i < n; i++) {
        struct knob *k = aicos_malloc(MEM_DEFAULT, sizeof(*k));

        if (k == RT_NULL)
            break;
        memset(k, 0, sizeof(*k));
        k->id = knob_ids[i];
        k->pin_a = knob_pins[i][0];
        k->pin_b = knob_pins[i][1];
        if (head == RT_NULL)
            head = k;
        else
            tail->next = k;
        tail = k;
    }
    return head;
}

/* stock 0x40242b98: A falling edge arms the decoder, A rising edge decides the direction from B. */
static void knob_process(struct knob *k, int a, int b)
{
    if (!k->armed) {
        if (a)
            return;
        k->armed = 1;
        k->b_at_fall = (uint8_t)b;
    }
    if (!a)
        return;

    k->b_low = (uint8_t)(b == 0);
    if (k->b_at_fall != 0 && b == 0)
        ack_key(k->id + 1, 0);          /* clockwise; state byte stays 0 as in the stock frame */
    if (k->b_at_fall == 0 && k->b_low == 0)
        ack_key(k->id, 0);              /* counter-clockwise */
    k->armed = 0;
}

/* Encoder push buttons. The stock code (0x40242cd8) reports only the press (state 1) and then blocks until all
 * buttons are released, so the host cannot tell a long press from a click and the rotation is not scanned in the
 * meantime. Here every button is tracked on its own and both edges are reported (state 1 = down, 0 = up, like the
 * matrix keys), so the host can time presses itself. Debounce: the level must be stable for 10 ms. */
#define KNOB_BTN_COUNT     4
#define KNOB_BTN_DEBOUNCE  10

static uint32_t ms_now(void)
{
    return (uint32_t)((uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
}

static void knob_buttons_scan(void)
{
    static const struct { const char *pin; uint8_t code; } btn[KNOB_BTN_COUNT] = {
        { "PE.15", 0x35 }, { "PB.8", 0x33 }, { "PE.11", 0x37 }, { "PB.11", 0x36 },
    };
    static uint8_t  reported[KNOB_BTN_COUNT];        /* state last sent to the host */
    static uint8_t  seen[KNOB_BTN_COUNT];            /* raw level seen at the last look */
    static uint32_t since[KNOB_BTN_COUNT];           /* when the raw level changed */
    uint32_t now = ms_now();
    int i;

    for (i = 0; i < KNOB_BTN_COUNT; i++) {
        uint8_t down = (pin_level(btn[i].pin) == 0);

        if (down != seen[i]) {
            seen[i] = down;
            since[i] = now;
        } else if (down != reported[i] && now - since[i] >= KNOB_BTN_DEBOUNCE) {
            reported[i] = down;
            ack_key(btn[i].code, down);
            app_log("knob button 0x%02x %s\n", btn[i].code, down ? "down" : "up");
        }
    }
}

void input_threads_entry_ec11(void *p)
{
    struct knob *list = knob_list_create(4);

    (void)p;
    for (;;) {
        if (g_host_state == 1) {
            struct knob *k = list;
            int n;

            for (n = 0; n < 4 && k; n++, k = k->next)
                knob_process(k, pin_level(k->pin_a) != 0, pin_level(k->pin_b) != 0);
            knob_buttons_scan();
        }
        rt_thread_mdelay(1);
    }
}
