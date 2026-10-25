/*
 * Status lines for the UART log (not in the stock firmware). They only read counters and never change behaviour.
 * Every 5 s: at AKP05_APP_LOG 2 always, at level 1 only while something changed since the last print.
 * rt_kprintf() has a 128 byte buffer, so the status is split into short lines.
 */
#include "akp05_app.h"
#include <string.h>

struct app_stats g_stats;

void diag_status_tick(void)
{
#if AKP05_APP_LOG
    static int ticks;
    static struct app_stats last;
    static uint32_t last_resets, last_in_fail, last_bogus, last_stale;
    uint32_t fifo_used;

    if (++ticks < 50)
        return;
    ticks = 0;
#if AKP05_APP_LOG < 2
    if (memcmp((const void *)&g_stats, &last, sizeof(last)) == 0)
        return;
#endif
    memcpy(&last, (const void *)&g_stats, sizeof(last));
    fifo_used = g_ctx.fifo.in - g_ctx.fifo.out;
    app_log("status: host %d rx %u bad %u stray %u cmds %u up ok %u bad %u trunc %u\n",
            (int)g_host_state, (unsigned)g_stats.rx_pkts, (unsigned)g_stats.rx_bad, (unsigned)g_stats.stray,
            (unsigned)g_stats.cmds, (unsigned)g_stats.uploads_ok, (unsigned)g_stats.uploads_bad,
            (unsigned)g_stats.fifo_trunc);
    app_log("status: tx ok %u fail %u drop %u err %d, ring %u fifo %u mq %d, wdt-nofeed %u\n",
            (unsigned)g_stats.tx_ok, (unsigned)g_stats.tx_fail, (unsigned)g_stats.tx_drop, g_stats.tx_last_err,
            (unsigned)rx_ring_used(),
            (unsigned)fifo_used, g_ctx.mq ? (int)g_ctx.mq->entry : -1, (unsigned)g_stats.wdt_nofeed);
    app_log("status: usb reset %u cfg %u disc %u ep0-bogus %u stale %u\n",
            (unsigned)g_stats.usb_resets, (unsigned)g_stats.usb_configured, (unsigned)g_stats.usb_disconnects,
            (unsigned)g_stats.ep0_bogus, (unsigned)g_stats.ep0_stale);
    app_log("status: ep0-in fail %u lost %u err %d, out-rearm fail %u\n", (unsigned)g_stats.ep0_in_fail,
            (unsigned)g_stats.ep0_in_lost, g_stats.ep0_in_last_err, (unsigned)g_stats.out_rearm_fail);
    /* something went wrong on the bus since the last print: show the control requests that led to it */
    if (g_stats.usb_resets != last_resets || g_stats.ep0_in_fail != last_in_fail || g_stats.ep0_bogus != last_bogus ||
        g_stats.ep0_stale != last_stale) {
        last_stale = g_stats.ep0_stale;
        last_resets = g_stats.usb_resets;
        last_in_fail = g_stats.ep0_in_fail;
        last_bogus = g_stats.ep0_bogus;
        usb_trace_dump(24);
    }
#endif
}
