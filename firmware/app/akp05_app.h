/*
 * AKP05E application - reconstructed from the stock firmware (seg0 @ 0x40241740 - 0x40243ef2).
 *
 * Every function carries a "stock 0x........" note: the address of the routine in the dumped firmware it was
 * rebuilt from (verified against the toolchain disassembly, not only against the decompiler).
 */
#ifndef AKP05_APP_H
#define AKP05_APP_H

#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------------------------------ logging */
/*
 * UART diagnostics of the application (115200 8N1 on the PA.0 pad). CMake option AKP05_APP_LOG:
 *   0 = errors only, 1 = events (default: uploads, key / knob / touch edges, logo, USB re-attach),
 *   2 = verbose (also every received command and a status line every 5 s). At level 1 the status line appears only
 *       while the counters change.
 */
#ifndef AKP05_APP_LOG
#define AKP05_APP_LOG 1
#endif
#define app_err(...)  rt_kprintf("[E/app] " __VA_ARGS__)
#if AKP05_APP_LOG >= 2
#define app_dbg(...)  rt_kprintf("[app] " __VA_ARGS__)
#else
#define app_dbg(...)  ((void)0)
#endif
#if AKP05_APP_LOG
#define app_log(...)  rt_kprintf("[app] " __VA_ARGS__)
#else
#define app_log(...)  ((void)0)
#endif

/* ------------------------------------------------------------------------------------------ protocol */
#define PKT_OUT_SIZE      1024          /* HID OUT report (host -> device), one command or one data chunk */
#define PKT_IN_SIZE       512           /* HID IN report (device -> host), "ACK..OK" frames */
#define IMG_BUF_SIZE      0x80000       /* 512 KiB image staging / fifo / decode buffers */
#define RX_RING_SIZE      0x100000      /* 1 MiB ring between the USB callback and the receive thread */

#define LCD_W             800
#define LCD_H             480
#define NUM_KEYS          15

/* command ids returned by the parser (stock 0x40241ce0) */
enum akp05_cmd {
    CMD_NONE   = 0,
    CMD_HAN    = 1,      /* sleep: backlight off                           */
    CMD_DIS    = 2,      /* wake / connect: backlight on, watchdog start   */
    CMD_CLE    = 3,      /* clear key tile (byte 11) or 'C' = everything   */
    CMD_STP    = 4,      /* flush: pan display                             */
    CMD_CON    = 5,
    CMD_APPNEW = 6,      /* reboot into the bootloader                     */
    CMD_KEY    = 7,      /* send an 8 byte keyboard report                 */
    CMD_LIG    = 8,      /* backlight level 0..60                          */
    CMD_BAT    = 9,      /* upload JPEG/PNG for one key tile               */
    CMD_LOG    = 10,     /* upload boot logo                               */
    CMD_BGPIC  = 11,
    CMD_BGCLE  = 12,
    CMD_SETLB  = 13,     /* 24 RGB colours of the LED strip                */
    CMD_LBLIG  = 14,     /* LED brightness                                 */
    CMD_QUCMD  = 15,     /* LED mode                                       */
    CMD_DELED  = 16,     /* LEDs off                                       */
    CMD_SELFTEST = 17,   /* enter / toggle selftest mode                   */
};

/*
 * State of the receive thread (stock 0x402b3a70, 32 bytes; it is also the message sent to the image thread).
 * kind: u16 at offset 0 is 0x0401 (LOG), 0x0501 (BAT) or 0x0a01 (BGPIC) for commands that are followed by data.
 */
struct cmd_state {
    uint8_t  data_cmd;      /* 0x00  1 = a data stream follows this header                      */
    uint8_t  kind;          /* 0x01  4 = LOG, 5 = BAT, 10 = BGPIC                               */
    uint8_t  pad2[2];
    uint32_t id;            /* 0x04  enum akp05_cmd                                             */
    uint8_t  pad8;
    uint8_t  receiving;     /* 0x09  data stream in progress                                    */
    uint8_t  pad10[6];
    uint64_t length;        /* 0x10  payload length announced by the header                     */
    uint64_t received;      /* 0x18  bytes received so far (header packet included)             */
};

void protocol_parse(const uint8_t *pkt, struct cmd_state *st);
void ack_init(uint8_t *frame);                      /* "ACK\0\0OK\0\0..." 512 byte frame        */
void ack_key(uint8_t code, uint8_t state);          /* key / knob / touch event to the host     */

/* ------------------------------------------------------------------------------------------ buffers */
struct kfifo {                      /* stock 0x40242052 / 0x402420a0 / 0x40242142 (power of two) */
    uint8_t *data;
    uint32_t size;
    volatile uint32_t out;
    volatile uint32_t in;
};
int      kfifo_init(struct kfifo *f, uint32_t size);
uint32_t kfifo_in(struct kfifo *f, const void *src, uint32_t len);
uint32_t kfifo_out(struct kfifo *f, void *dst, uint32_t len);

void rx_ring_init(uint32_t size);                   /* stock 0x402421cc */
int  rx_ring_write(const uint8_t *src, int len);    /* stock 0x40242220 */
int  rx_ring_read(uint8_t *dst, int len);           /* stock 0x402422e0 */

/* ------------------------------------------------------------------------------------------ context */
/* stock 0x402b8f98 */
struct app_ctx {
    rt_mq_t          mq;            /* 0x00 "Qpic": completed uploads for the image thread           */
    struct kfifo     fifo;          /* 0x04 completed upload data                                    */
    uint8_t         *img_buf;       /* 0x14 decode / logo buffer                                     */
    uint8_t         *stage_buf;     /* 0x18 HID receive staging buffer                               */
    rt_sem_t         cfg_sem;       /* 0x1c "write config sem"                                       */
    rt_sem_t         logo_sem;      /* 0x20 "logo read sem"                                          */
    rt_sem_t         logo_wr_sem;   /* 0x24 "logo write sem"                                         */
    rt_sem_t         hid_sem;       /* 0x28 "hid recv sem"                                           */
    rt_sem_t         vib_sem;       /* 0x2c "vibration motor sem"                                    */
    uint8_t          first_boot;    /* 0x30                                                          */
    rt_timer_t       wdt_timer;     /* 0x34 "wdt_timer_hook"                                         */
    rt_device_t      wdt;           /* 0x38                                                          */
    volatile uint8_t usb_active;    /* 0x3c set by the receive thread, consumed by the watchdog hook */
    uint32_t         wdt_timeout;   /* 0x40 seconds                                                  */
};
extern struct app_ctx g_ctx;

/* ------------------------------------------------------------------------------------------ diagnostics */
/* Counters for the status line (diag.c). Not part of the stock firmware; they never influence the behaviour. */
struct app_stats {
    volatile uint32_t rx_pkts;          /* OUT reports queued by the endpoint callback                    */
    volatile uint32_t rx_bad;           /* OUT reports that could not be queued (ring error)              */
    volatile uint32_t cmds;             /* plain commands dispatched                                      */
    volatile uint32_t stray;            /* reports outside a stream that do not start with "CRT"          */
    volatile uint32_t uploads_ok;       /* uploads queued for the image thread                            */
    volatile uint32_t uploads_bad;      /* uploads rejected (no end marker, too long)                     */
    volatile uint32_t fifo_trunc;       /* uploads that did not fit into the image fifo                   */
    volatile uint32_t tx_ok;            /* IN reports accepted by the controller                          */
    volatile uint32_t tx_fail;          /* IN frames refused by the controller (retried later)            */
    volatile uint32_t tx_drop;          /* IN frames dropped because the event queue was full             */
    volatile int      tx_last_err;      /* last error code of usbd_ep_start_write                         */
    volatile uint32_t wdt_nofeed;       /* watchdog ticks without host traffic                            */
    volatile uint32_t usb_resets;       /* bus resets seen                                                */
    volatile uint32_t usb_configured;   /* SET_CONFIGURATION seen                                         */
    volatile uint32_t usb_disconnects;
    volatile uint32_t ep0_in_fail;      /* EP0 IN writes refused by the controller (usb_hid.c)             */
    volatile int      ep0_in_last_err;  /* error code of the last refused EP0 IN write                     */
    volatile uint32_t ep0_in_lost;      /* ... and still refused after the retry window                    */
    volatile uint32_t out_rearm_fail;   /* re-arming the OUT endpoint failed (retried from the main loop)  */
    volatile uint32_t ep0_stale;        /* stale SETUP completions dropped (usb_hid.c)                    */
    volatile uint32_t ep0_bogus;       /* bogus EP0 OUT completions dropped (usb_hid.c)                 */
};
extern struct app_stats g_stats;
void usb_trace_dump(int count);                     /* usb_hid.c: last control requests / USB events           */
void diag_status_tick(void);                       /* called every 100 ms from main(), prints every 5 s */
uint32_t rx_ring_used(void);                        /* bytes queued and not yet read                       */

/* ------------------------------------------------------------------------------------------ shared state */
extern volatile uint8_t  g_backlight_on;     /* stock 0x402b3a69 */
extern volatile int      g_host_state;       /* stock 0x402b3a6c: -1 never connected, 0 off, 1 connected */
extern uint8_t           g_backlight_level;  /* stock 0x402b3a61 (0..60, default 80)                     */
extern uint8_t           g_stp_pending;      /* stock 0x402b3a60                                         */
extern volatile uint8_t  g_boot_request;     /* stock 0x402b8f48: 1 = "aicupg hid", 2 = "aicupg"         */
extern int               g_clear_toggle;     /* stock 0x402b8eb4                                         */
extern uint8_t           g_logo_mode;        /* stock 0x402b3a64 (0x11 / 0x1f)                           */

/* ------------------------------------------------------------------------------------------ modules */
/* backlight.c */
int  backlight_set(uint32_t level);          /* stock 0x40243e00 */
int  backlight_off(void);                    /* stock 0x40243e6a */
int  wdt_start(void);                        /* stock 0x40243b7c */
int  wdt_stop(void);                         /* stock 0x40243bb4 */
int  wdt_init(void);                         /* stock 0x4024396e */
void wdt_feed_hook(void *arg);               /* stock 0x40243938 */

/* leds.c */
extern uint8_t  g_led_rgb[72];               /* stock 0x402b8e60 */
extern uint8_t  g_leds_on;                   /* stock 0x402b8c1c */
extern float    g_led_brightness;            /* stock 0x402b3a10 */
extern float    g_led_saved;                 /* stock 0x402b8f44 */
extern int      g_led_mode;                  /* stock 0x402b8f40 */
extern rt_device_t g_led_qspi;               /* stock 0x402b8eac, never assigned in the stock image */
void leds_apply(const uint8_t *rgb72);       /* stock 0x40241740 */

/* input.c */
int  pins_init(void);                        /* stock 0x402429aa */
int  knob_i2c_init(void);                    /* stock 0x4024324e */
void input_threads_entry_keyboard(void *p);  /* stock 0x4024321c */
void input_threads_entry_ec11(void *p);      /* stock 0x40242e24 */

/* touch.c */
void touch_poll(void);                       /* stock 0x402433d4 (called from the keyboard thread) */

/* display.c */
int  display_layout_init(void);              /* stock 0x40241b04 */
void display_key_image(const uint8_t *buf, int size, int key, int layer);   /* stock 0x40241b86 */
void display_clear(int what);                /* stock 0x40241bf4: 'C' / 0xff / key number */
void display_flush(int arg);                 /* stock 0x40241cb8 */
void display_logo(void);                     /* stock 0x40243ca6 */

/* threads.c */
void usb_recv_thread_entry(void *p);         /* stock 0x40242808 */
void usb_img_thread_entry(void *p);          /* stock 0x40242398 */
void logo_thread_entry(void *p);             /* stock 0x40243d4c */
int  threads_start(void);                    /* stock 0x402418e6 */
void dispatch_command(struct cmd_state *st, const uint8_t *pkt, struct app_ctx *ctx);  /* stock 0x40242494 */

/* usb_hid.c */
int  hid_report_send(const void *buf, int len);       /* stock 0x402399c8: IN endpoint 0x82 */
int  hid_kbd_send(const void *buf, int len);          /* stock 0x4023a5e4(0x81, ...)         */
void usb_watchdog_tick(void);                       /* enumeration watchdog, see usb_hid.c */
int  usb_hid_init(void);                              /* stock 0x40239ab2                    */

void usb_serial_from_efuse(void);                     /* stock 0x402399d4 */

/* storage.c */
int  logo_write_file(const char *path, const uint8_t *img_buf, uint32_t len);  /* stock 0x40243bf6 */

/* display.c: selftest drawing helpers (visual coordinates; call display_flush after drawing) */
void display_fill(int vx, int vy, int vw, int vh, uint32_t argb);
void display_text(int vx, int vy, const char *s, int scale, uint16_t rgb565);

/* selftest.c */
#include "selftest.h"

#endif /* AKP05_APP_H */
