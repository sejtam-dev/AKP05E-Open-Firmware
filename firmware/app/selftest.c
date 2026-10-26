/*
 * Self-test mode: standalone hardware diagnostic that runs without a host.
 *
 * Entry: encoder 1 (0x37) + encoder 4 (0x36) held 3 s on the logo/idle screen,
 *        UART MSH command 'selftest', or host command CRT SELFT.
 * Exit:  same L+R 5 s combo, or key 10 (EXIT/BACK), or key 10 in the main menu.
 *
 * Physical layout (established by user observation):
 *   TOP row    (visual y = 5):    key codes 1–5   left → right
 *   MIDDLE row (visual y = 179):  key codes 6–10  left → right
 *   BOTTOM row (visual y = 353):  encoders 1–4    left → right
 *
 * Screen coordinates are VISUAL: (0,0) = top-left of the visible screen.
 * display_fill / display_text convert to buffer coordinates internally (180° rotation).
 *
 * Encoder button physical order (left to right):
 *   1 = code 0x37 (PE.11)   2 = code 0x35 (PE.15)
 *   3 = code 0x33 (PB.8)    4 = code 0x36 (PB.11)
 * Encoder rotation CCW/CW codes:
 *   1: CCW 0xa0 / CW 0xa1   2: CCW 0x50 / CW 0x51
 *   3: CCW 0x90 / CW 0x91   4: CCW 0x70 / CW 0x71
 */
#include "akp05_app.h"
#include "selftest.h"
#include <string.h>
#include <finsh.h>

volatile uint8_t g_selftest_mode;

/* ---- visual constants -------------------------------------------------------------------- */
#define ST_TILE_W  112
#define ST_TILE_H  112

#define ST_TOP_Y   5    /* visual y: top physical row    (key codes 1–5)  */
#define ST_MID_Y   179  /* visual y: middle physical row (key codes 6–10) */
#define ST_BOT_Y   353  /* visual y: bottom row          (encoders)        */

/* All three rows share the same five x-positions. */
static const int16_t ROW_X[5] = { 7, 176, 345, 514, 683 };

/* EXIT tile: physical key 10 is at the rightmost slot of the middle row. */
#define ST_EXIT_X  683
#define ST_EXIT_Y  ST_MID_Y

/* Title y: centred in the 62 px gap between top row bottom (5+112=117) and mid row top (179).
 * Scale-2 text is 14 px tall:  117 + (62-14)/2 = 141. */
#define ST_TITLE_Y  141
/* x to centre a scale-2 string of n chars (each char = 12 px incl inter-char gap, last -1) */
#define ST_TITLE_X(n)  ((LCD_W - ((n) * 12 - 2)) / 2)

/* ---- colours ----------------------------------------------------------------------------- */
#define COL_BG         0xff101010u
#define COL_MENU       0xff002288u
#define COL_IDLE       0xff282828u
#define COL_PRESSED    0xff00aa00u
#define COL_DONE       0xff005500u
#define COL_ENC_BG     0xff222200u
#define COL_ENC_ACTIVE 0xff886600u
#define COL_EXIT       0xff330000u  /* EXIT / BACK tile */

#define RGB_WHITE  0xffffu
#define RGB_YELLOW 0xffe0u
#define RGB_GREEN  0x07e0u
#define RGB_GREY   0x8410u

/* ---- state machine ----------------------------------------------------------------------- */
enum st_mode { ST_MENU = 0, ST_BUTTONS, ST_DISPLAY, ST_TOUCH, ST_ENCODERS };
static enum st_mode st_mode;

/* BUTTONS test ----- */
/* All 10 matrix keys + 4 encoder buttons are tested; exit via L+R 3 s combo. */
static const uint8_t btn_codes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                     0x37, 0x35, 0x33, 0x36 };
#define BTN_CODE_COUNT  ((int)(sizeof(btn_codes) / sizeof(btn_codes[0])))
static uint8_t btn_tested[256];
static uint8_t btn_held[256];
static int btn_done_count;

/* ENCODERS test ---- */
static const struct {
    uint8_t btn; uint8_t ccw; uint8_t cw;
} enc_info[4] = {
    { 0x37, 0xa0, 0xa1 },   /* encoder 1 (PE.11) */
    { 0x35, 0x50, 0x51 },   /* encoder 2 (PE.15) */
    { 0x33, 0x90, 0x91 },   /* encoder 3 (PB.8)  */
    { 0x36, 0x70, 0x71 },   /* encoder 4 (PB.11) */
};
static int     enc_rot[4];
static uint8_t enc_btn[4];
static uint8_t enc_btn_seen[4];

/* DISPLAY / TOUCH test */
#define DISP_PHASE_TICKS  67   /* ~2 s at 30 ms/tick */
static int disp_phase;
static int disp_ticks;
static uint8_t touch_live_code;
static uint8_t touch_live_state;
static uint8_t touch_live_dir;   /* dir byte from swipe events (SWIPE_DIR_LEFT/RIGHT/UP/DOWN) */

/* ---- helpers ----------------------------------------------------------------------------- */
static int enc_idx_from_btn(uint8_t code)
{
    int i;
    for (i = 0; i < 4; i++)
        if (enc_info[i].btn == code)
            return i;
    return -1;
}

static int enc_idx_from_rot(uint8_t code)
{
    int i;
    for (i = 0; i < 4; i++)
        if (enc_info[i].ccw == code || enc_info[i].cw == code)
            return i;
    return -1;
}

/* Centre text in a tile of given width at visual (tx, ty). */
static void draw_tile_text_w(int tx, int ty, int tw,
                              const char *s, int scale, uint16_t rgb565)
{
    int slen = 0;
    const char *p;
    int sw, sx, sy;

    for (p = s; *p; p++)
        slen++;
    sw = slen * 6 * scale - scale;
    sx = tx + (tw - sw) / 2;
    sy = ty + (ST_TILE_H - 7 * scale) / 2;
    if (sx < tx)
        sx = tx + 2;
    display_text(sx, sy, s, scale, rgb565);
}

static void draw_tile_text(int tx, int ty, const char *s, int scale, uint16_t rgb565)
{
    draw_tile_text_w(tx, ty, ST_TILE_W, s, scale, rgb565);
}

static void draw_tile(int tx, int ty, uint32_t fill, const char *label, int scale, uint16_t txt)
{
    display_fill(tx, ty, ST_TILE_W, ST_TILE_H, fill);
    if (label && *label)
        draw_tile_text(tx, ty, label, scale, txt);
}

/* Status line one row below the bottom tile row (scale 1, grey). */
static void draw_footer(const char *s)
{
    display_text(4, ST_BOT_Y + ST_TILE_H + 6, s, 1, RGB_GREY);
}

/* EXIT tile at key 10 physical position (middle row rightmost). Used in all subtests
 * except BUTTONS (where key 10 occupies that slot as a testable key). */
static void draw_exit_tile(void)
{
    draw_tile(ST_EXIT_X, ST_EXIT_Y, COL_EXIT, "EXIT", 2, RGB_WHITE);
}

/* ---- screen renderers -------------------------------------------------------------------- */
static void render_menu(void)
{
    /* Row 1: BUTTONS / DISPLAY / TOUCH / ENCODERS  (keys 1–4)
     * Row 2: only EXIT at rightmost slot             (key 10) */
    static const char *labels[4] = { "BUTTONS", "DISPLAY", "TOUCH", "ENCODERS" };
    int i;

    g_clear_toggle ^= 1;
    display_fill(0, 0, LCD_W, LCD_H, COL_BG);
    for (i = 0; i < 4; i++)
        draw_tile(ROW_X[i], ST_TOP_Y, COL_MENU, labels[i], 2, RGB_WHITE);
    draw_tile(ST_EXIT_X, ST_EXIT_Y, COL_EXIT, "EXIT", 2, RGB_WHITE);
    display_text(ST_TITLE_X(13), ST_TITLE_Y, "SELFTEST MENU", 2, RGB_YELLOW);
    display_flush(g_clear_toggle);
}

static void render_buttons(void)
{
    char buf[8];
    int i;

    g_clear_toggle ^= 1;
    display_fill(0, 0, LCD_W, LCD_H, COL_BG);

    /* Top row: keys 1–5 */
    for (i = 0; i < 5; i++) {
        uint8_t c = i + 1;
        uint32_t fill;
        const char *label;

        if (btn_held[c])        { fill = COL_PRESSED; label = "PRESS"; }
        else if (btn_tested[c]) { fill = COL_DONE;    label = "OK";    }
        else                    { fill = COL_IDLE;     label = NULL;    }
        rt_snprintf(buf, sizeof(buf), "%d", (int)c);
        draw_tile(ROW_X[i], ST_TOP_Y, fill, label ? label : buf, 2, RGB_WHITE);
    }

    /* Middle row: keys 6–10 */
    for (i = 0; i < 5; i++) {
        uint8_t c = 6 + (uint8_t)i;
        uint32_t fill;
        const char *label;

        if (btn_held[c])        { fill = COL_PRESSED; label = "PRESS"; }
        else if (btn_tested[c]) { fill = COL_DONE;    label = "OK";    }
        else                    { fill = COL_IDLE;     label = NULL;    }
        rt_snprintf(buf, sizeof(buf), "%d", (int)c);
        draw_tile(ROW_X[i], ST_MID_Y, fill, label ? label : buf, 2, RGB_WHITE);
    }

    /* Bottom row: encoder buttons */
    for (i = 0; i < 4; i++) {
        uint8_t c = enc_info[i].btn;
        uint32_t fill = btn_held[c] ? COL_PRESSED : (btn_tested[c] ? COL_DONE : COL_ENC_BG);
        const char *lbl;

        rt_snprintf(buf, sizeof(buf), "E%d", i + 1);
        lbl = btn_held[c] ? "PRESS" : (btn_tested[c] ? "OK" : buf);
        draw_tile(ROW_X[i], ST_BOT_Y, fill, lbl, 2, RGB_WHITE);
    }

    /* Touch strip tile 4 (0x43): EXIT — same size as encoder tiles */
    draw_tile(ROW_X[4], ST_BOT_Y, COL_EXIT, "EXIT", 2, RGB_WHITE);

    {
        char status[48];
        rt_snprintf(status, sizeof(status), "TESTED: %d / %d",
                    btn_done_count, BTN_CODE_COUNT);
        draw_footer(status);
    }
    display_text(ST_TITLE_X(12), ST_TITLE_Y, "BUTTONS TEST", 2, RGB_YELLOW);
    display_flush(g_clear_toggle);
}

static const uint32_t disp_colors[] = {
    0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
};
#define DISP_COLOR_PHASES  ((int)(sizeof(disp_colors) / sizeof(disp_colors[0])))

static void render_display_color(int phase)
{
    g_clear_toggle ^= 1;
    display_fill(0, 0, LCD_W, LCD_H, disp_colors[phase]);
    display_flush(g_clear_toggle);
}

static void render_display_bars(void)
{
    static const uint32_t bars[] = {
        0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
        0xffffff00u, 0xff00ffffu, 0xffff00ffu, 0xff808080u,
    };
    int i;

    g_clear_toggle ^= 1;
    for (i = 0; i < 8; i++)
        display_fill(i * (LCD_W / 8), 0, LCD_W / 8, LCD_H, bars[i]);
    display_flush(g_clear_toggle);
}

static void render_touch(void)
{
    static const uint8_t tcodes[]  = { 0x40, 0x41, 0x42, 0x43 };
    static const char   *tlabels[] = { "T1",  "T2",  "T3",  "T4"  };
    char buf[48];
    int i;

    g_clear_toggle ^= 1;
    display_fill(0, 0, LCD_W, LCD_H, COL_BG);
    draw_exit_tile();

    for (i = 0; i < 4; i++) {
        uint32_t fill = (touch_live_code == tcodes[i] && touch_live_state) ?
                        COL_PRESSED : COL_IDLE;
        int tx = 4 + i * 200;

        display_fill(tx, ST_BOT_Y, 176, ST_TILE_H, fill);
        draw_tile_text_w(tx, ST_BOT_Y, 176, tlabels[i], 3, RGB_WHITE);
    }

    if (touch_live_code == 0x39) {
        if (touch_live_dir == 1)
            rt_snprintf(buf, sizeof(buf), "SWIPE LEFT");
        else if (touch_live_dir == 2)
            rt_snprintf(buf, sizeof(buf), "SWIPE RIGHT");
        else
            rt_snprintf(buf, sizeof(buf), "SWIPE H");
    } else if (touch_live_code == 0xb1) {
        rt_snprintf(buf, sizeof(buf), "SWIPE UP");
    } else if (touch_live_code == 0xb2) {
        rt_snprintf(buf, sizeof(buf), "SWIPE DOWN");
    } else if (touch_live_code) {
        rt_snprintf(buf, sizeof(buf), "CODE 0x%02X  STATE %d",
                    (unsigned)touch_live_code, (int)touch_live_state);
    } else {
        rt_snprintf(buf, sizeof(buf), "WAITING FOR TOUCH...");
    }
    draw_footer(buf);

    display_text(ST_TITLE_X(10), ST_TITLE_Y, "TOUCH TEST", 2, RGB_YELLOW);
    display_flush(g_clear_toggle);
}

static void render_encoders(void)
{
    int i;

    g_clear_toggle ^= 1;
    display_fill(0, 0, LCD_W, LCD_H, COL_BG);
    draw_exit_tile();

    for (i = 0; i < 4; i++) {
        uint32_t fill = enc_btn[i] ? COL_ENC_ACTIVE : COL_ENC_BG;
        char buf[16];

        display_fill(ROW_X[i], ST_BOT_Y, ST_TILE_W, ST_TILE_H, fill);
        rt_snprintf(buf, sizeof(buf), "ENC %d", i + 1);
        display_text(ROW_X[i] + 4, ST_BOT_Y + 4,  buf, 2, RGB_YELLOW);
        rt_snprintf(buf, sizeof(buf), "%+d", enc_rot[i]);
        display_text(ROW_X[i] + 4, ST_BOT_Y + 30, buf, 2, RGB_WHITE);
        display_text(ROW_X[i] + 4, ST_BOT_Y + 56,
                     enc_btn_seen[i] ? (enc_btn[i] ? "PRESS" : "OK") : "BTN?",
                     2, enc_btn_seen[i] ? RGB_GREEN : RGB_GREY);
    }
    draw_footer("TURN AND PRESS EACH ENCODER    KEY10: EXIT");
    display_text(ST_TITLE_X(13), ST_TITLE_Y, "ENCODERS TEST", 2, RGB_YELLOW);
    display_flush(g_clear_toggle);
}

/* ---- public interface -------------------------------------------------------------------- */
static void reset_state(void)
{
    btn_done_count = 0;
    memset(btn_tested,   0, sizeof(btn_tested));
    memset(btn_held,     0, sizeof(btn_held));
    memset(enc_rot,      0, sizeof(enc_rot));
    memset(enc_btn,      0, sizeof(enc_btn));
    memset(enc_btn_seen, 0, sizeof(enc_btn_seen));
    disp_phase       = 0;
    disp_ticks       = 0;
    touch_live_code  = 0;
    touch_live_state = 0;
    touch_live_dir   = 0;
}

static void enter_mode(enum st_mode mode)
{
    st_mode = mode;
    reset_state();
    switch (mode) {
    case ST_MENU:     render_menu();            break;
    case ST_BUTTONS:  render_buttons();         break;
    case ST_DISPLAY:  render_display_color(0);  break;
    case ST_TOUCH:    render_touch();           break;
    case ST_ENCODERS: render_encoders();        break;
    }
}

void selftest_start(void)
{
    g_selftest_mode = 1;
    enter_mode(ST_MENU);
    app_log("selftest started\n");
}

void selftest_stop(void)
{
    g_selftest_mode = 0;
    app_log("selftest stopped\n");
    display_logo();
}

void selftest_key_event(uint8_t code, uint8_t state)
{
    int enc;

    if (!g_selftest_mode)
        return;

    /* menu navigation: keys 1–5 select tests, key 10 exits selftest */
    if (st_mode == ST_MENU && state == 1) {
        switch (code) {
        case 1:  enter_mode(ST_BUTTONS);  return;
        case 2:  enter_mode(ST_DISPLAY);  return;
        case 3:  enter_mode(ST_TOUCH);    return;
        case 4:  enter_mode(ST_ENCODERS); return;
        case 10: selftest_stop();          return;
        default: break;
        }
    }

    /* key 10 = EXIT to menu from all subtests except BUTTONS (where it is tested) */
    if (st_mode != ST_BUTTONS && code == 10 && state == 1) {
        enter_mode(ST_MENU);
        return;
    }

    /* encoder rotation */
    enc = enc_idx_from_rot(code);
    if (enc >= 0 && state == 0) {
        if (enc_info[enc].cw == code)
            enc_rot[enc]++;
        else
            enc_rot[enc]--;
        if (st_mode == ST_ENCODERS)
            render_encoders();
        return;
    }

    /* button press / release */
    enc = enc_idx_from_btn(code);
    if (enc >= 0) {
        /* encoder button */
        enc_btn[enc] = state;
        if (state) {
            enc_btn_seen[enc] = 1;
            if (!btn_tested[code]) {
                btn_tested[code] = 1;
                btn_done_count++;
            }
        }
    } else if (code >= 1 && code <= 10) {
        /* matrix key */
        btn_held[code] = state;
        if (state && !btn_tested[code]) {
            btn_tested[code] = 1;
            btn_done_count++;
        }
    }

    switch (st_mode) {
    case ST_BUTTONS:
        render_buttons();
        break;

    case ST_ENCODERS:
        render_encoders();
        break;

    default:
        break;
    }
}

void selftest_touch_event(uint8_t code, uint8_t state, uint16_t x, uint16_t y)
{
    (void)y;
    if (!g_selftest_mode)
        return;
    /* touch tile 4 (0x43) exits the buttons test */
    if (st_mode == ST_BUTTONS && code == 0x43 && state == 1) {
        enter_mode(ST_MENU);
        return;
    }
    touch_live_code  = code;
    touch_live_state = state;
    touch_live_dir   = (uint8_t)x;  /* for swipes: dir byte; for tiles: unused */
    if (st_mode == ST_TOUCH)
        render_touch();
}

/* called every 30 ms from the keyboard thread */
void selftest_tick(void)
{
    if (!g_selftest_mode)
        return;

    if (st_mode != ST_DISPLAY)
        return;

    disp_ticks++;
    if (disp_ticks < DISP_PHASE_TICKS)
        return;
    disp_ticks = 0;

    disp_phase++;
    if (disp_phase < DISP_COLOR_PHASES) {
        render_display_color(disp_phase);
    } else if (disp_phase == DISP_COLOR_PHASES) {
        render_display_bars();
    } else {
        enter_mode(ST_MENU);
    }
}

/* L+R encoder 3 s combo: called by the EC11 thread at ~1 ms intervals */
void selftest_combo_poll(uint32_t ms_now, uint8_t left_down, uint8_t right_down)
{
    static uint32_t since;
    static uint8_t  armed;

    if (left_down && right_down) {
        if (!armed) {
            armed = 1;
            since = ms_now;
        } else if (ms_now - since >= 3000) {
            armed = 0;
            if (!g_selftest_mode && g_host_state <= 0)
                selftest_start();
            else if (g_selftest_mode)
                selftest_stop();
        }
    } else {
        armed = 0;
    }
}

/* ---- UART MSH command -------------------------------------------------------------------- */
static void cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_selftest_mode)
        selftest_stop();
    else
        selftest_start();
}
MSH_CMD_EXPORT_ALIAS(cmd_selftest, selftest, toggle the hardware selftest mode);
