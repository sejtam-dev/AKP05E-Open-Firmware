/*
 * Touch strip below the key matrix: an I2C touch controller (address 0x38 on bus i2c2) polled by the keyboard
 * thread while the host is connected. Rebuilt from stock 0x402432cc (register read) and 0x402433d4 (events).
 *
 * Fidelity: the register decoding and the tile hit table follow the stock disassembly. The swipe classification uses
 * the stock thresholds (14 px horizontal, 29 px vertical) but is simplified compared to the stock state machine.
 *
 * Host events (512 byte "ACK" frames, see docs/host-protocol.md): byte 9 = code, byte 10 = state, bytes 11/12 = x and
 * bytes 13/14 = y of the point where the finger landed (big endian).
 *
 *   tap            DOWN frame (state 1) and UP frame (state 0) one after the other when the finger is lifted;
 *                  code = tile (0x40..0x43) or 0x38 outside the four tiles
 *   long press     DOWN frame as soon as the finger has rested TOUCH_HOLD_MS, UP frame when it is lifted
 *   swipe          one frame only (state 1): 0x39 horizontal, 0xb2 down, 0xb1 up, no tile press; bytes 11..15 carry
 *                  the direction and the movement (not in stock, harmless for hosts that ignore them)
 *
 * Deviation from stock: stock sent a tap only after the finger had left (a fake down, 50 ms later the up) and sent
 * swipes and touches outside the tiles as a single frame without an up. Here a resting finger gives a real DOWN edge
 * (long presses can be timed, exactly as for the keys), a tap gives the pair at once, a swipe the single frame.
 */
#include "akp05_app.h"
#include <string.h>
#include <aic_core.h>

#define TOUCH_I2C_ADDR_W   0x38
#define TOUCH_I2C_ADDR_R   0xb8
#define TOUCH_MAX_X        800
#define TOUCH_MAX_Y        120
#define TILE_W             176
#define TILE_H             112
#define RETRY_WINDOW_US    49           /* stock retries a busy IN endpoint for 49 us before it drops the frame */

#define CODE_OUTSIDE       0x38
#define CODE_SWIPE_H       0x39
#define CODE_SWIPE_DOWN    0xb2
#define CODE_SWIPE_UP      0xb1
/* direction byte (11) of the swipe frames: left / right = towards lower / higher touch x, i.e. towards tile 0x40 /
 * 0x43; up / down = towards lower / higher y */
#define SWIPE_DIR_LEFT     1
#define SWIPE_DIR_RIGHT    2
#define SWIPE_DIR_UP       3
#define SWIPE_DIR_DOWN     4
#define TOUCH_HOLD_MS      100          /* a finger resting this long sends the tile DOWN frame before it is lifted */
#define SWIPE_H_MIN        14
#define SWIPE_V_MIN        29

/* stock 0x402b3ab0: codes of the four touch tiles and their left / top coordinates */
static const uint8_t  tile_code[4] = { 0x40, 0x41, 0x42, 0x43 };
static const uint16_t tile_xy[4][2] = { { 5, 3 }, { 204, 3 }, { 403, 3 }, { 602, 3 } };

rt_device_t g_knob_i2c;

/* INIT_APP_EXPORT: stock 0x4024324e */
int knob_i2c_init(void)
{
    g_knob_i2c = rt_device_find("i2c2");
    if (g_knob_i2c == RT_NULL) {
        rt_kprintf("[E/aic_mpp] Can't find %s device\n", "i2c2");
        return -1;
    }
    if (rt_device_open(g_knob_i2c, RT_DEVICE_OFLAG_RDWR) != 0) {
        rt_kprintf("[E/aic_mpp] open %s device failed\n", "i2c2");
        return -1;
    }
    rt_kputs("knob i2c2 device init success");
    return 1;
}
INIT_APP_EXPORT(knob_i2c_init);

static uint32_t ms_now_touch(void)
{
    return (uint32_t)((uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
}

struct touch_pt {
    uint8_t  down;           /* a contact is present                */
    uint16_t x, y;           /* current position                    */
    uint16_t x0, y0;         /* position when the contact started   */
    uint8_t  started;
};

static struct touch_pt tp;
static uint8_t touch_active;     /* a contact is being tracked */
static uint8_t touch_code;       /* code of the DOWN frame that was sent */
static uint8_t touch_sent;       /* the DOWN frame went out (host connected and display on) */
static uint8_t touch_armed;      /* host connected and display on when the finger landed: events are sent */
static uint32_t touch_t0;        /* tick (ms) of the landing */

/* stock 0x402432cc: register 2, 7 bytes. Byte 1 bit 7 = valid touch, bit 6 = release marker. */
static int touch_read(void)
{
    uint8_t reg = 2;
    uint8_t buf[7] = { 0 };
    struct rt_i2c_msg msgs[2];
    uint8_t st;

    if (g_knob_i2c == RT_NULL)
        return 0;
    msgs[0].addr  = TOUCH_I2C_ADDR_W;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;
    msgs[1].addr  = TOUCH_I2C_ADDR_R;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].len   = 7;
    msgs[1].buf   = buf;
    rt_i2c_transfer((struct rt_i2c_bus_device *)g_knob_i2c, msgs, 2);

    st = buf[1];
    if ((int8_t)st >= 0) {                  /* no touch */
        if (st & 0x40)
            tp.started = 0;
        tp.down = 0;
        return 0;
    }
    tp.x = (uint16_t)(((buf[1] & 0x0f) << 8) + buf[2]);
    tp.y = (uint16_t)(((buf[3] & 0x0f) << 8) + buf[4]);
    tp.down = 1;
    if (tp.x > TOUCH_MAX_X)
        tp.x = TOUCH_MAX_X;
    if (tp.y > TOUCH_MAX_Y)
        tp.y = TOUCH_MAX_Y;
    if (!tp.started) {
        tp.started = 1;
        tp.x0 = tp.x;
        tp.y0 = tp.y;
    }
    return 1;
}

/* Hand one ACK frame to the IN queue; a full queue is retried for a short window like in stock, then the frame is
 * dropped (see usb_hid.c, the queue normally never fills). */
static void deliver(const uint8_t *frame, uint8_t code, uint8_t state)
{
    uint64_t t0;

    if (hid_report_send(frame, PKT_IN_SIZE) == 0)
        return;
    t0 = aic_get_time_us();
    while (aic_get_time_us() - t0 <= RETRY_WINDOW_US)
        if (hid_report_send(frame, PKT_IN_SIZE) == 0)
            return;
    app_log("touch frame 0x%02x/%d dropped (event queue full)\n", code, state);
}

static void send_event(uint8_t code, uint8_t state, int with_position)
{
    uint8_t frame[PKT_IN_SIZE];

    ack_init(frame);
    frame[9] = code;
    frame[10] = state;
    if (with_position) {
        frame[11] = (uint8_t)(tp.x0 >> 8);
        frame[12] = (uint8_t)tp.x0;
        frame[13] = (uint8_t)(tp.y0 >> 8);
        frame[14] = (uint8_t)tp.y0;
    }
    deliver(frame, code, state);
}

/* Swipe frame: the stock bytes (code, state 1) plus, in bytes that are zero in the stock frame, the direction and the
 * movement from the landing point to the last point: byte 11 direction, 12/13 dx and 14/15 dy (signed, big endian). */
static void send_swipe(uint8_t code, uint8_t dir, int dx, int dy)
{
    uint8_t frame[PKT_IN_SIZE];

    ack_init(frame);
    frame[9] = code;
    frame[10] = 1;
    frame[11] = dir;
    frame[12] = (uint8_t)((uint16_t)dx >> 8);
    frame[13] = (uint8_t)dx;
    frame[14] = (uint8_t)((uint16_t)dy >> 8);
    frame[15] = (uint8_t)dy;
    deliver(frame, code, 1);
}

static int tile_at(uint16_t x, uint16_t y)
{
    int i;

    for (i = 0; i < 4; i++)
        if (x >= tile_xy[i][0] && x <= tile_xy[i][0] + TILE_W &&
            y >= tile_xy[i][1] && y <= tile_xy[i][1] + TILE_H)
            return i;
    return -1;
}

/* Swipe classification of the movement from the landing point to the last point (stock thresholds). Returns the swipe
 * code (0 = no swipe) and the direction byte. */
static uint8_t classify_swipe(int dx, int dy, uint8_t *dir)
{
    if (dx > SWIPE_H_MIN || dx < -SWIPE_H_MIN) {
        *dir = dx > 0 ? SWIPE_DIR_RIGHT : SWIPE_DIR_LEFT;
        return CODE_SWIPE_H;
    }
    if (dy > SWIPE_V_MIN && dx >= -SWIPE_V_MIN && dx <= SWIPE_V_MIN) {
        *dir = SWIPE_DIR_DOWN;
        return CODE_SWIPE_DOWN;
    }
    if (dy < -SWIPE_V_MIN && dx >= -SWIPE_V_MIN && dx <= SWIPE_V_MIN) {
        *dir = SWIPE_DIR_UP;
        return CODE_SWIPE_UP;
    }
    return 0;
}

/* Called by the keyboard thread every 30 ms while the host is connected.
 *
 * The tile DOWN frame is not sent at the moment the finger lands, otherwise every swipe that starts on a tile would
 * press that tile first. It is sent
 *   - when the finger has rested on the same spot for TOUCH_HOLD_MS (long press: the host can time it), or
 *   - together with the UP frame when the finger is lifted without having made a swipe (a tap),
 * and not at all for a swipe, which is then a single frame as in the stock firmware. */
void touch_poll(void)
{
    int dx, dy;
    uint8_t swipe, dir;

    touch_read();

    if (tp.down) {
        dx = (int)tp.x - (int)tp.x0;
        dy = (int)tp.y - (int)tp.y0;
        if (!touch_active) {                                   /* finger landed */
            int t = tile_at(tp.x0, tp.y0);

            touch_active = 1;
            touch_sent = 0;
            touch_armed = (g_host_state == 1 && g_backlight_on == 1);
            touch_code = (t >= 0) ? tile_code[t] : CODE_OUTSIDE;
            touch_t0 = ms_now_touch();
        } else if (touch_armed && !touch_sent && ms_now_touch() - touch_t0 >= TOUCH_HOLD_MS &&
                   !classify_swipe(dx, dy, &dir) && dx >= -SWIPE_H_MIN && dx <= SWIPE_H_MIN &&
                   dy >= -SWIPE_V_MIN && dy <= SWIPE_V_MIN) {
            send_event(touch_code, 1, 1);                      /* resting finger: report the press now */
            touch_sent = 1;
            app_log("touch down %d,%d code 0x%02x (held)\n", tp.x0, tp.y0, touch_code);
        }
        return;
    }
    if (!touch_active)
        return;

    /* finger left */
    touch_active = 0;
    tp.started = 0;
    if (!touch_armed)
        return;
    dx = (int)tp.x - (int)tp.x0;
    dy = (int)tp.y - (int)tp.y0;
    swipe = classify_swipe(dx, dy, &dir);

    if (!touch_sent) {
        if (swipe) {                                           /* swipe: no tile press at all */
            send_swipe(swipe, dir, dx, dy);
            app_log("swipe code 0x%02x dir %d dx %d dy %d\n", swipe, dir, dx, dy);
            return;
        }
        send_event(touch_code, 1, 1);                          /* tap: press and release together */
    }
    send_event(touch_code, 0, 1);
    app_log("touch up code 0x%02x dx %d dy %d\n", touch_code, dx, dy);
    if (swipe)                                                 /* a long press that became a swipe */
        send_swipe(swipe, dir, dx, dy);
}
