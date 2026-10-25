/*
 * Application entry (stock 0x40241a2a, jumped to from RT-Thread's main thread) and resource setup
 * (stock 0x40243a18).
 */
#include "akp05_app.h"
#include <string.h>
#include <stdio.h>
#include <finsh.h>
#include <msh.h>
#include "aic_reboot_reason.h"
#include <aic_core.h>

void cmd_reboot(int argc, char **argv);        /* SDK: kernel/common/cmd/aic_reboot_cmd.c */

/* Stock "aicupg" (0x402489ee): argument "hid" -> reboot reason 6, "gotobl" -> 5, anything else -> 4, then reboot.
 * Reason 6 makes the SPL run the HID upgrade at once (33C3:8899). The SDK's own aicupg command ignores the
 * argument and always sets 4, which sends the SPL into the BootROM upgrade mode (33C3:6677, slow, no HID). */
static void reboot_to_upgrade(int reason)
{
    aic_set_reboot_reason((enum aic_reboot_reason)reason);
    cmd_reboot(0, NULL);
}

/* UART shortcut with the stock behaviour of "aicupg hid" (the SDK command of that name cannot be replaced) */
static void upghid(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    reboot_to_upgrade(6);
}
MSH_CMD_EXPORT(upghid, Reboot into the HID upgrade bootloader);

static void app_resources_init(void)
{
    struct app_ctx *c = &g_ctx;

    if (!kfifo_init(&c->fifo, IMG_BUF_SIZE))
        fputs("ringbuff init fail\n", stderr);
    rx_ring_init(RX_RING_SIZE);
    /* Exactly like stock (0x40243a18): img_buf from the RT-Thread heap, the staging buffer from the PSRAM CMA
     * region (aicos_malloc type 2). Do not "simplify" this: both blocks from rt_malloc() fails (the heap lives in
     * the small SRAM, the second 512 KiB block is NULL and the first image packet then writes to address 0), and
     * both blocks from the 5.5 MB CMA region used more of it than stock and made the USB enumeration unreliable. */
    c->img_buf = rt_malloc(IMG_BUF_SIZE);
    c->stage_buf = aicos_malloc(MEM_CMA, IMG_BUF_SIZE);
    app_log("buffers: img %p stage %p fifo %p\n", c->img_buf, c->stage_buf, c->fifo.data);
    if (c->img_buf == NULL || c->stage_buf == NULL)
        app_err("image buffers: img %p stage %p, out of memory\n", c->img_buf, c->stage_buf);

    c->hid_sem = rt_sem_create("hid recv sem", 0, RT_IPC_FLAG_FIFO);
    if (c->hid_sem == RT_NULL)
        rt_kputs("creat hid recv sem fail");
    c->logo_sem = rt_sem_create("logo read sem", 0, RT_IPC_FLAG_FIFO);
    if (c->logo_sem == RT_NULL)
        rt_kputs("creat logo read sem fail");
    c->logo_wr_sem = rt_sem_create("logo write sem", 0, RT_IPC_FLAG_FIFO);
    if (c->logo_wr_sem == RT_NULL)
        rt_kputs("creat logo write sem fail");
    c->vib_sem = rt_sem_create("vibration motor sem", 0, RT_IPC_FLAG_FIFO);
    if (c->vib_sem == RT_NULL)
        rt_kputs("creat Vibration motor sem fail");
    c->cfg_sem = rt_sem_create("write config sem", 0, RT_IPC_FLAG_FIFO);
    if (c->cfg_sem == RT_NULL)
        rt_kputs("creat write config sem fail");
    c->mq = rt_mq_create("Qpic", 0x28, 4000, RT_IPC_FLAG_FIFO);
}

int main(void)
{
    app_resources_init();
    threads_start();
    rt_sem_release(g_ctx.logo_sem);                 /* show the logo */
    rt_kputs("/*===========================================================*/");
    rt_kprintf("/* Firmware compile time:%s         %s*\n", __DATE__, __TIME__);
    rt_kputs("/*===========================================================*/");

    for (;;) {
        /* APPNEW: leave the application and reboot into the HID bootloader (needed to flash again) */
        if (g_boot_request == 1) {
            g_boot_request = 0;
            backlight_off();
            reboot_to_upgrade(6);
        } else if (g_boot_request == 2) {
            g_boot_request = 0;
            backlight_off();
            reboot_to_upgrade(4);
        }
        usb_watchdog_tick();
        diag_status_tick();
        rt_thread_mdelay(100);
    }
    return 0;
}
