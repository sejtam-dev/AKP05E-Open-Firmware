/*
 * Threads and the command dispatcher.
 *   usb_recv_thread_entry  stock 0x40242808     dispatch_command  stock 0x40242494
 *   usb_img_thread_entry   stock 0x40242398     logo_thread_entry stock 0x40243d4c
 *   threads_start          stock 0x402418e6
 */
#include "akp05_app.h"
#include <string.h>

#define LOGO_PATH   "/data/logo_jpg_dir/test_logo.jpg"

/* ---------------------------------------------------------------------------------------------- state */
volatile uint8_t g_backlight_on = 1;        /* 0x402b3a69 */
volatile int     g_host_state   = -1;       /* 0x402b3a6c */
uint8_t          g_backlight_level = 80;    /* 0x402b3a61 */
uint8_t          g_stp_pending  = 1;        /* 0x402b3a60 */
volatile uint8_t g_boot_request;            /* 0x402b8f48 */
int              g_clear_toggle;            /* 0x402b8eb4 */
uint8_t          g_logo_mode    = 0x11;     /* 0x402b3a64 */
static uint8_t   g_splash_mode  = 0x11;     /* 0x402b3a68 */

struct app_ctx g_ctx;

/* ------------------------------------------------------------------------------------- dispatcher */
static void leds_after_wake(void)
{
    if (g_led_mode == 0) {
        if (g_led_saved != 0.0f)
            g_led_brightness = g_led_saved;
        if (g_leds_on)
            leds_apply(g_led_rgb);
    }
    g_backlight_on = 1;
}

void dispatch_command(struct cmd_state *st, const uint8_t *pkt, struct app_ctx *ctx)
{
    switch (st->id) {
    case CMD_HAN:                                   /* sleep */
        if (g_logo_mode == 31)
            g_led_brightness = 0;
        backlight_off();
        if (g_led_mode == 0) {
            g_led_saved = g_led_brightness;
            g_led_brightness = 0;
            if (g_leds_on)
                leds_apply(g_led_rgb);
        }
        g_backlight_on = 0;
        break;

    case CMD_DIS:                                   /* wake / connect */
        if (g_host_state != 1) {
            wdt_start();
            rt_device_control(rt_device_find("wdt"), RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
            if (ctx->wdt_timer != RT_NULL)
                rt_timer_start(ctx->wdt_timer);
            display_clear(0xff);
            g_host_state = 1;
        }
        backlight_set(g_backlight_level);
        leds_after_wake();
        break;

    case CMD_CLE:                                   /* clear a key or the whole screen ('C') */
        g_stp_pending = 1;
        display_clear(pkt[11]);
        if (pkt[11] == 'C') {
            wdt_stop();
            if (ctx->wdt_timer != RT_NULL)
                rt_timer_stop(ctx->wdt_timer);
            g_host_state = 0;
            memset(g_led_rgb, 0, 13);
            rt_sem_release(ctx->logo_sem);          /* show the logo again */
            if (g_logo_mode == 31)
                g_led_brightness = 0;
        }
        break;

    case CMD_STP:                                   /* make the drawn page visible */
        if (g_stp_pending) {
            display_flush(g_clear_toggle);
            g_stp_pending = 0;
        }
        break;

    case CMD_APPNEW:                                /* reboot into the bootloader, see main() */
        g_boot_request = (pkt[11] == 0xff) ? 2 : 1;
        break;

    case CMD_KEY: {                                 /* 8 byte keyboard report */
        uint8_t rep[8];

        memcpy(rep, &pkt[10], 8);
        hid_kbd_send(rep, 8);
        break;
    }

    case CMD_LIG: {                                 /* backlight level, 0..60 */
        uint8_t lv = pkt[10];

        if (lv > 60)
            lv = 60;
        g_backlight_level = lv;
        backlight_set(lv);
        break;
    }

    case CMD_SETLB:                                 /* 24 LED colours */
        memcpy(g_led_rgb, &pkt[10], sizeof(g_led_rgb));
        g_leds_on = 1;
        leds_apply(g_led_rgb);
        break;

    case CMD_LBLIG:                                 /* LED brightness */
        g_led_brightness = (float)pkt[10];
        if (g_leds_on)
            leds_apply(g_led_rgb);
        break;

    case CMD_QUCMD:                                 /* LED mode */
        if (pkt[10] == 0x11)
            g_led_mode = 2;
        else if (pkt[10] == 0x1f)
            g_led_mode = 0;
        else if (pkt[10] == 0xff)
            g_led_mode = 1;
        break;

    case CMD_DELED:
        g_leds_on = 0;
        break;

    default:                                        /* CON, BAT, LOG, BGPIC, BGCLE: handled by the data path */
        break;
    }
}

/* ------------------------------------------------------------------------------ receive thread */
static int has_end_marker(const uint8_t *stage, uint32_t length)
{
    const uint8_t *tail = stage + length;

    if (tail[1023] == 0xd9 && tail[1022] == 0xff)               /* JPEG EOI                    */
        return 1;
    if (tail[1019] == 'D' && tail[1018] == 'N' && tail[1017] == 'E' && tail[1016] == 'I')   /* PNG IEND */
        return 1;
    return 0;
}

void usb_recv_thread_entry(void *p)
{
    struct app_ctx *ctx = p;
    struct cmd_state st;
    uint8_t pkt[PKT_OUT_SIZE];
    uint32_t offset = 0;

    memset(&st, 0, sizeof(st));
    st.data_cmd = 1;                                /* initial value of the stock image */

    for (;;) {
        rt_sem_take(ctx->hid_sem, RT_WAITING_FOREVER);
        ctx->usb_active = 1;
        rx_ring_read(pkt, PKT_OUT_SIZE);
        if (!st.receiving && !(pkt[0] == 'C' && pkt[1] == 'R' && pkt[2] == 'T')) {
            if (g_stats.stray++ < 5)
                app_log("stray report outside a stream: %02x %02x %02x %02x %02x %02x %02x %02x\n", pkt[0], pkt[1],
                        pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7]);
        }
        protocol_parse(pkt, &st);

        if (st.data_cmd == 0) {                     /* plain command */
            g_stats.cmds++;
            app_dbg("cmd %d (%c%c%c%c)\n", (int)st.id, pkt[5], pkt[6], pkt[7], pkt[8]);
            dispatch_command(&st, pkt, ctx);
            st.data_cmd = 1;
            continue;
        }

        /* data stream (header packet of LOG / BAT / BGPIC, then the payload in 1024 byte chunks) */
        if (st.receiving == 0) {
            st.received = 0;
            st.receiving = 1;
            offset = 0;
            app_dbg("upload start: cmd %d kind %d length %d\n", (int)st.id, (int)st.kind, (int)st.length);
        } else {
            uint64_t next = st.received + PKT_OUT_SIZE;

            offset = (uint32_t)st.received;
            if ((next >> 32) != 0 || next > IMG_BUF_SIZE) {
                rt_kprintf("[E/%s] %s()%d The data length %d is too long\n", "usb", "usb_recv_buf_thread_entry",
                           54, (int)next);
                g_stats.uploads_bad++;
                st.receiving = 0;
                continue;
            }
        }
        if (st.data_cmd != 1)
            continue;

        memcpy(ctx->stage_buf + offset, pkt, PKT_OUT_SIZE);
        st.received += PKT_OUT_SIZE;
        if (st.received < st.length + PKT_OUT_SIZE)
            continue;                               /* more data to come */
        if (st.kind == 3)
            continue;

        if (has_end_marker(ctx->stage_buf, (uint32_t)st.length)) {
            uint32_t in = kfifo_in(&ctx->fifo, ctx->stage_buf, (uint32_t)st.received);

            if (in != (uint32_t)st.received) {
                g_stats.fifo_trunc++;
                app_err("image fifo full: %d of %d bytes stored, the next image will be corrupt\n", (int)in,
                        (int)st.received);
            }
            g_stats.uploads_ok++;

            app_dbg("upload complete: cmd %d, %d bytes queued (%d offered)\n", (int)st.id, (int)in,
                       (int)st.received);
            rt_mq_send(ctx->mq, &st, 32);
        } else {
            g_stats.uploads_bad++;
            app_log("upload rejected: cmd %d, no JPEG/PNG end marker after %d bytes\n", (int)st.id,
                       (int)st.length);
        }
        /* upload finished or rejected: reset the stream state */
        st.data_cmd = 0;
        st.received = 0;
        st.length = 0;
        st.id = 0;
    }
}

/* ---------------------------------------------------------------------------------- image thread */
void usb_img_thread_entry(void *p)
{
    struct app_ctx *ctx = p;
    struct cmd_state msg;
    uint8_t ok[PKT_IN_SIZE];

    ack_init(ok);
    for (;;) {
        if (rt_mq_recv(ctx->mq, &msg, 32, RT_WAITING_FOREVER) != RT_EOK) {
            rt_kputs("err");
            continue;
        }
        kfifo_out(&ctx->fifo, ctx->img_buf, (uint32_t)msg.received);

        if (msg.id == CMD_LOG) {
            int n = logo_write_file(LOGO_PATH, ctx->img_buf, 0);

            if (n < 0) {
                rt_kputs("write file error!");
            } else if ((uint64_t)(int64_t)n == msg.length) {
                hid_report_send(ok, PKT_IN_SIZE);
                rt_sem_release(ctx->logo_sem);
                rt_thread_mdelay(500);
                rt_kprintf("file readly  size is %d \n", (int)msg.length);
            }
        } else if (msg.id == CMD_BAT) {
            display_key_image(ctx->img_buf + 0x400, (int)msg.length, ctx->img_buf[0xc], 1);
        } else {
            app_log("image thread: cmd %d has no handler (stock ignores it too)\n", (int)msg.id);
        }
    }
}

/* ----------------------------------------------------------------------------------- logo thread */
void logo_thread_entry(void *p)
{
    struct app_ctx *ctx = p;

    for (;;) {
        if (rt_sem_take(ctx->logo_sem, RT_WAITING_FOREVER) != RT_EOK)
            return;
        display_logo();
        /* g_backlight_on starts as 1 in the stock image and the stock logo is visible at boot, but nothing in the
         * stock logo path switches the PWM on: it is already running there (the bootloader/board left it on).
         * Here the PWM driver starts idle, so do it explicitly. */
        if (g_backlight_on)
            backlight_set(g_backlight_level);
        if (g_host_state == 0 && g_splash_mode == 0xff) {
            g_backlight_on = 1;
            backlight_set(g_backlight_level);
            rt_thread_mdelay(500);
            backlight_off();
            g_backlight_on = 0;
        }
        if (ctx->first_boot == 1) {
            rt_thread_mdelay(1500);                 /* show the logo, then enumerate on USB */
            usb_hid_init();
            ctx->first_boot = 0;
        }
    }
}

/* ------------------------------------------------------------------------------------- creation */
static rt_thread_t start_thread(const char *name, void (*entry)(void *), struct app_ctx *ctx, uint8_t prio,
                                const char *err)
{
    rt_thread_t t = rt_thread_create(name, entry, ctx, 0x1000, prio, 10);

    if (t == RT_NULL) {
        rt_kputs(err);
        return RT_NULL;
    }
    rt_thread_startup(t);
    return t;
}

int threads_start(void)
{
    struct app_ctx *ctx = &g_ctx;

    if (!start_thread("logo read", logo_thread_entry, ctx, 15, "Failed to create logo read thread"))
        return -1;
    if (!start_thread("keyboard scan", input_threads_entry_keyboard, ctx, 12, "Failed to create keyboard scan thread"))
        return -1;
    if (!start_thread("ec11 scan", input_threads_entry_ec11, ctx, 11, "Failed to create ec11 scan thread"))
        return -1;
    if (!start_thread("usb recv buf", usb_recv_thread_entry, ctx, 0, "Failed to create usb recv buf thread"))
        return -1;
    if (!start_thread("usb img event", usb_img_thread_entry, ctx, 1, "Failed to create usb img event thread"))
        return -1;
    return 0;
}
