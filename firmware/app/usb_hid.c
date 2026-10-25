/*
 * USB device (CherryUSB, ArtInChip port): two HID interfaces.
 *   interface 0: vendor HID, IN 0x82 (512 B, "ACK" frames), OUT 0x03 (1024 B, commands)
 *   interface 1: boot keyboard, IN 0x81 (8 B), used by the KEY command
 * The descriptor blob (usb_desc_blob.inc) is copied byte for byte from the stock image (seg1 @ 0x402b38e8),
 * the registration order from stock 0x40239ab2. The serial number is patched from the eFuse chip id
 * (stock 0x402399d4).
 */
#include "akp05_app.h"
#include "usbd_core.h"
#include "usbd_hid.h"
#include <aic_core.h>
#include <string.h>

#define EP_CUSTOM_IN   0x82
#define EP_KBD_IN      0x81
#define EP_CUSTOM_OUT  0x03
#define SERIAL_OFFSET  160          /* first UTF-16 char of the serial number string in the blob */

extern int drv_efuse_init(void);
extern int drv_efuse_read(uint32_t addr, void *data, uint32_t size);

static uint8_t hid_descriptor[] = {
#include "usb_desc_blob.inc"
};

static const uint8_t hid_custom_report_desc[54] = {
    0x06, 0xa0, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x09, 0x02, 0xa1, 0x00, 0x06, 0xa1, 0xff, 0x09, 0x03,
    0x09, 0x04, 0x15, 0x80, 0x25, 0x7f, 0x35, 0x00, 0x45, 0xff, 0x75, 0x08, 0x96, 0x00, 0x02, 0x81,
    0x02, 0x09, 0x05, 0x09, 0x06, 0x15, 0x80, 0x25, 0x7f, 0x35, 0x00, 0x45, 0xff, 0x75, 0x08, 0x96,
    0x00, 0x04, 0x91, 0x02, 0xc0, 0xc0,
};

static const uint8_t hid_kbd_report_desc[63] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t read_buffer[PKT_OUT_SIZE];

/* Enumeration watchdog state (not in stock): the device controller occasionally stops answering the host in the
 * middle of an enumeration (Windows: "USB device not recognized", the OUT endpoint close times out). */
static volatile uint8_t usb_started;        /* usb_hid_init() has run */
static volatile uint8_t usb_configured;     /* the host selected the configuration */
static volatile uint8_t out_rearm_pending;  /* the OUT endpoint was not re-armed, usb_watchdog_tick() retries */
static volatile uint8_t usb_reset_seen;     /* the host reset the bus since the last re-attach */

/* Trace of the last control requests and USB events (not in stock). Filled from the interrupt without any printing,
 * printed by diag.c from the main thread when a bus reset or an EP0 error was counted. */
#define TRACE_LEN 48
struct usb_trace_entry {
    uint32_t tick_ms;
    char     kind;          /* S setup, R bus reset, C configured, F EP0 IN refused, B bogus EP0 OUT dropped,
                             * D stale SETUP dropped */
    uint8_t  b[8];
};
static struct usb_trace_entry trace[TRACE_LEN];
static volatile uint32_t trace_n;

static void trace_add(char kind, const uint8_t *b8)
{
    struct usb_trace_entry *e = &trace[trace_n % TRACE_LEN];

    e->tick_ms = (uint32_t)((uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
    e->kind = kind;
    if (b8)
        memcpy(e->b, b8, 8);
    else
        memset(e->b, 0, 8);
    trace_n++;
}

void usb_trace_dump(int count)
{
    uint32_t n = trace_n;
    uint32_t first = (n > (uint32_t)count) ? n - (uint32_t)count : 0;
    uint32_t i;

    if (n > TRACE_LEN && first < n - TRACE_LEN)
        first = n - TRACE_LEN;
    app_log("usb trace, oldest first (ms, S=setup type req value index len, R=reset, C=configured):\n");
    for (i = first; i < n; i++) {
        const struct usb_trace_entry *e = &trace[i % TRACE_LEN];

        if (e->kind == 'S')
            app_log(" %u S %02x %02x %02x%02x %02x%02x %02x%02x\n", (unsigned)e->tick_ms, e->b[0], e->b[1], e->b[3],
                    e->b[2], e->b[5], e->b[4], e->b[7], e->b[6]);
        else if (e->kind == 'F')
            app_log(" %u F EP0 IN refused: error %d, length %u, retries done %u\n", (unsigned)e->tick_ms,
                    (int)(int8_t)e->b[0], (unsigned)(e->b[1] | (e->b[2] << 8)), (unsigned)e->b[3]);
        else
            app_log(" %u %c\n", (unsigned)e->tick_ms, e->kind);
    }
}

static void in_queue_reset(void);
static void in_pump_locked(void);

void usbd_event_handler(uint8_t event)             /* stock 0x402399ae */
{
    if (event == USBD_EVENT_CONFIGURED) {
        g_stats.usb_configured++;
        trace_add('C', NULL);
        usb_configured = 1;
        usbd_ep_start_read(EP_CUSTOM_OUT, read_buffer, PKT_OUT_SIZE);
    } else if (event == USBD_EVENT_RESET) {
        trace_add('R', NULL);
        g_stats.usb_resets++;
        in_queue_reset();
        usb_configured = 0;
        usb_reset_seen = 1;
    } else if (event == USBD_EVENT_DISCONNECTED) {
        g_stats.usb_disconnects++;
        in_queue_reset();
        usb_configured = 0;
        usb_reset_seen = 0;
    }
}

/* The ArtInChip CherryUSB port can report one SETUP packet twice: first through the "setup phase done" interrupt
 * (the request is then handled), later through "transfer done" of the same EP0 OUT transfer. The second report
 * is converted to nbytes = xfer_len - remaining = 8 - 16 = -8, which CherryUSB reads as 4 GB of data: it moves the
 * EP0 data pointer, re-arms EP0 OUT for a data stage that does not exist, and the control transfer that is in
 * flight (typically the first GET_DESCRIPTOR after SET_ADDRESS) is lost. The host waits 5 s, resets the bus and
 * the enumeration fails over and over ("EP0 recv -8 bytes, 8 remained" on the UART). The linker option
 * --wrap=usbd_event_ep_out_complete_handler (CMakeLists.txt) routes the port's call through this filter; a real
 * EP0 OUT completion never has a negative length. The SDK itself stays unmodified. */
extern void __real_usbd_event_ep_out_complete_handler(uint8_t ep, uint32_t nbytes);

void __wrap_usbd_event_ep_out_complete_handler(uint8_t ep, uint32_t nbytes)
{
    if (ep == 0x00 && (int32_t)nbytes < 0) {
        g_stats.ep0_bogus++;
        trace_add('B', NULL);
        return;
    }
    __real_usbd_event_ep_out_complete_handler(ep, nbytes);
}

/* EP0 IN: CherryUSB ignores the return value of usbd_ep_start_write() for the next chunk of a data stage (the
 * 64 + 2 byte configuration descriptor). If the controller still reports the endpoint as enabled (-2) the chunk
 * is never sent, the host times out after 5 s and resets the bus. Retry for a short moment; the counters in the
 * status line show whether this ever happens. Other endpoints pass through unchanged. */
extern int __real_usbd_ep_start_write(const uint8_t ep, const uint8_t *data, uint32_t data_len);

int __wrap_usbd_ep_start_write(const uint8_t ep, const uint8_t *data, uint32_t data_len)
{
    int ret = __real_usbd_ep_start_write(ep, data, data_len);
    int i;

    if (ep != 0x80 || ret == 0)
        return ret;
    g_stats.ep0_in_fail++;
    g_stats.ep0_in_last_err = ret;
    for (i = 0; i < 100 && ret == -2; i++) {
        aic_udelay(1);
        ret = __real_usbd_ep_start_write(ep, data, data_len);
    }
    {
        uint8_t b[8] = { (uint8_t)ret, (uint8_t)data_len, (uint8_t)(data_len >> 8), (uint8_t)i, 0, 0, 0, 0 };

        trace_add('F', b);
    }
    if (ret != 0)
        g_stats.ep0_in_lost++;
    return ret;
}

/* Stale SETUP completions (found from the control request trace of this device, 2026-10-25).
 *
 * The controller reports one SETUP packet through two interrupts, "setup phase done" and "transfer done" of EP0 OUT.
 * The port treats whichever comes first as the request (ep0_ctrl_stage 1 -> 2) and the other one as stale, which is
 * only right while the stage is still 2. For a request without a data stage the status stage finishes, and the port
 * re-arms the SETUP read (stage 1), before the second interrupt is served. It is then taken for a new SETUP packet and
 * the old request runs a second time (a second status ZLP that the host never fetches, and stage 2 again). The next
 * real SETUP packet - on this device the report descriptor request after SET_IDLE - arrives with stage 2, is ignored
 * (the "-8 bytes" report filtered above), and the host gets no answer: after 5 s it resets the bus.
 *
 * A stale report does not come with a new SETUP packet, so the setup buffer still holds the old request. After every
 * request bRequest, wValue and wIndex are overwritten with a marker (bmRequestType and wLength stay: the port's IN
 * completion looks at them); a new packet overwrites the marker, a stale report finds it intact. A stale report is
 * dropped and the stage is put back to 1, which is where the port had left it. ep0_ctrl_stage is a global of the port. */
extern uint8_t ep0_ctrl_stage;
extern void __real_usbd_event_ep0_setup_complete_handler(uint8_t *psetup);

#define SETUP_MARK 0xa5

void __wrap_usbd_event_ep0_setup_complete_handler(uint8_t *psetup)
{
    int i, stale = 1;

    for (i = 1; i <= 5; i++)
        if (psetup[i] != SETUP_MARK)
            stale = 0;
    if (stale) {
        g_stats.ep0_stale++;
        trace_add('D', NULL);
        ep0_ctrl_stage = 1;
        return;
    }
    trace_add('S', psetup);
    __real_usbd_event_ep0_setup_complete_handler(psetup);
    for (i = 1; i <= 5; i++)
        psetup[i] = SETUP_MARK;
}

/* ---- IN endpoint 0x82: one serialised sender for all events (keys, knobs, touch, logo ack) ----
 * Not in stock. The stock firmware (and this one until now) built a 512 byte frame on the stack of whichever thread
 * had an event and called usbd_ep_start_write() directly, dropping the frame when the endpoint was still busy. Three
 * threads can do that at the same moment (keyboard / touch, encoders, image thread), the call is not atomic (EPENA
 * test, then register writes) and the controller reads the frame later, from the TX FIFO interrupt: fast knob
 * turning produced bursts that raced each other and were followed by a bus reset of the host.
 * Now every sender only appends the 16 byte header of its frame (all frames are "ACK..OK" frames, zero after byte 15)
 * to a queue; the frame is expanded into one static buffer and handed to the controller from here and from the IN
 * completion callback, never two at once. A full queue drops the new event (counted). */
#define EVT_QUEUE_LEN   64
#define EVT_HDR_LEN     16

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t in_buf[PKT_IN_SIZE];
static uint8_t evt_q[EVT_QUEUE_LEN][EVT_HDR_LEN];
static volatile uint32_t evt_head, evt_tail;        /* free running indexes */
static volatile uint8_t in_busy;                    /* a frame is in the controller, in_cb() has not run yet */

/* caller holds the interrupt lock */
static void in_pump_locked(void)
{
    int ret;

    if (in_busy || evt_head == evt_tail || !usb_configured)
        return;
    memset(in_buf, 0, PKT_IN_SIZE);
    memcpy(in_buf, evt_q[evt_tail % EVT_QUEUE_LEN], EVT_HDR_LEN);
    ret = usbd_ep_start_write(EP_CUSTOM_IN, in_buf, PKT_IN_SIZE);
    if (ret == 0) {
        in_busy = 1;
        evt_tail++;
        g_stats.tx_ok++;
    } else {
        g_stats.tx_fail++;                          /* kept in the queue, retried by in_cb() / the main loop */
        g_stats.tx_last_err = ret;
    }
}

static void in_queue_reset(void)                    /* bus reset / disconnect: the controller forgot everything */
{
    in_busy = 0;
    evt_tail = evt_head;
}

static void in_cb(uint8_t ep, uint32_t nbytes)
{
    rt_base_t lvl;

    (void)nbytes;
    if (ep != EP_CUSTOM_IN)
        return;
    lvl = rt_hw_interrupt_disable();
    in_busy = 0;
    in_pump_locked();
    rt_hw_interrupt_enable(lvl);
}

/* stock 0x4023997e: queue the report, wake the receive thread, arm the next transfer */
static void custom_out_cb(uint8_t ep, uint32_t nbytes)
{
    (void)ep;
    if (rx_ring_write(read_buffer, (int)nbytes) != 0)
        g_stats.rx_bad++;
    g_stats.rx_pkts++;
    rt_sem_release(g_ctx.hid_sem);
    if (usbd_ep_start_read(EP_CUSTOM_OUT, read_buffer, PKT_OUT_SIZE) != 0) {
        g_stats.out_rearm_fail++;
        out_rearm_pending = 1;
    }
}

static struct usbd_endpoint custom_in_ep  = { .ep_cb = in_cb,         .ep_addr = EP_CUSTOM_IN };
static struct usbd_endpoint kbd_in_ep     = { .ep_cb = in_cb,         .ep_addr = EP_KBD_IN };
static struct usbd_endpoint custom_out_ep = { .ep_cb = custom_out_cb, .ep_addr = EP_CUSTOM_OUT };
static struct usbd_interface intf0, intf1;

/* stock 0x402399d4: serial = first 12 hex digits of the 16 byte chip id (efuse offset 16) */
void usb_serial_from_efuse(void)
{
    uint32_t id[4] = { 0 };
    char hex[17];
    uint32_t w[2];
    int i, j;

    if (!drv_efuse_init()) {                 /* RT_TRUE (1) on success */
        rt_kputs("efuse init fail");
        return;
    }
    drv_efuse_read(16, id, 16);
    for (j = 0; j < 2; j++) {
        w[j] = __builtin_bswap32(id[j]);
        for (i = 0; i < 8; i++) {
            uint32_t n = (w[j] >> (28 - 4 * i)) & 0xf;

            hex[j * 8 + i] = (char)(n <= 9 ? '0' + n : 'A' + n - 10);
        }
    }
    for (i = 0; i < 12; i++)
        hid_descriptor[SERIAL_OFFSET + 2 * i] = (uint8_t)hex[i];
}

/* stock 0x40239ab2 (called by the logo thread 1.5 s after boot) */
int usb_hid_init(void)
{
    usbd_desc_register(hid_descriptor);
    usbd_add_interface(usbd_hid_init_intf(&intf0, hid_custom_report_desc, sizeof(hid_custom_report_desc)));
    usbd_add_interface(usbd_hid_init_intf(&intf1, hid_kbd_report_desc, sizeof(hid_kbd_report_desc)));
    usbd_add_endpoint(&custom_in_ep);
    usbd_add_endpoint(&custom_out_ep);
    usbd_add_endpoint(&kbd_in_ep);
    usbd_initialize();
    usb_started = 1;
    return 0;
}

/* Called every 100 ms from main(): if the host has been resetting the bus for 5 s without the device getting
 * configured, detach and attach again so that the host starts a fresh enumeration. */
void usb_watchdog_tick(void)
{
    static int stuck;

    {   /* a frame that the controller refused is retried here if no completion callback does it */
        rt_base_t lvl = rt_hw_interrupt_disable();

        in_pump_locked();
        rt_hw_interrupt_enable(lvl);
    }
    if (out_rearm_pending && usb_configured) {
        out_rearm_pending = 0;
        if (usbd_ep_start_read(EP_CUSTOM_OUT, read_buffer, PKT_OUT_SIZE) != 0 && usb_configured)
            out_rearm_pending = 1;      /* still enabled (already armed) or busy: harmless, try again later */
    }
    if (!usb_started || usb_configured || !usb_reset_seen) {
        stuck = 0;
        return;
    }
    if (++stuck < 50)
        return;
    stuck = 0;
    in_queue_reset();
    app_log("USB: not configured 5 s after a bus reset, re-attaching\n");
    usb_reset_seen = 0;
    usbd_deinitialize();
    rt_thread_mdelay(300);                  /* the host must see the disconnect */
    usb_hid_init();
}

/* stock 0x402399c8: queue one ACK frame (PKT_IN_SIZE bytes, only the first 16 are kept, see above).
 * 0 = queued, negative = queue full or not an ACK frame. */
int hid_report_send(const void *buf, int len)
{
    rt_base_t lvl;
    int ret = 0;

    if (len != PKT_IN_SIZE)
        return -1;
    lvl = rt_hw_interrupt_disable();
    if (evt_head - evt_tail >= EVT_QUEUE_LEN) {
        g_stats.tx_drop++;
        ret = -2;
    } else {
        memcpy(evt_q[evt_head % EVT_QUEUE_LEN], buf, EVT_HDR_LEN);
        evt_head++;
        in_pump_locked();
    }
    rt_hw_interrupt_enable(lvl);
    return ret;
}

USB_MEM_ALIGNX static uint8_t kbd_buf[8];           /* the controller reads the report after the call returns */

int hid_kbd_send(const void *buf, int len)
{
    if (len > (int)sizeof(kbd_buf))
        len = (int)sizeof(kbd_buf);
    memcpy(kbd_buf, buf, (size_t)len);
    return usbd_ep_start_write(EP_KBD_IN, kbd_buf, (uint32_t)len);
}
