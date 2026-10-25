/*
 * LCD backlight (PWM) and watchdog helpers.
 *   backlight_set  stock 0x40243e00      backlight_off  stock 0x40243e6a
 *   wdt_start      stock 0x40243b7c      wdt_stop       stock 0x40243bb4
 *   wdt_init       stock 0x4024396e      wdt_feed_hook  stock 0x40243938
 */
#include "akp05_app.h"
#include <drivers/watchdog.h>

#define PWM_DEV_NAME        "pwm"
#define PWM_CHANNEL         2
#define PWM_PERIOD_NS       1000000
#define PWM_NS_PER_LEVEL    10000        /* level 15..80 -> 1.5 % .. 8 % ... duty = level * 10 us of 1 ms */
#define BACKLIGHT_MIN_LEVEL 15

#define WDT_DEV_NAME        "wdt"
#define WDT_TIMEOUT_S       20
#define WDT_HOOK_PERIOD_MS  15000

int backlight_set(uint32_t level)
{
    struct rt_device_pwm *pwm = (struct rt_device_pwm *)rt_device_find(PWM_DEV_NAME);
    uint32_t lv = (level > BACKLIGHT_MIN_LEVEL - 1) ? (level & 0xff) : BACKLIGHT_MIN_LEVEL;

    if (pwm == RT_NULL) {
        rt_kprintf("pwm sample run failed! can't find device!\n");
        return 1;
    }
    rt_pwm_set(pwm, PWM_CHANNEL, PWM_PERIOD_NS, lv * PWM_NS_PER_LEVEL);
    rt_pwm_enable(pwm, PWM_CHANNEL);
    return 0;
}

int backlight_off(void)
{
    struct rt_device_pwm *pwm = (struct rt_device_pwm *)rt_device_find(PWM_DEV_NAME);

    if (pwm == RT_NULL) {
        rt_kprintf("pwm sample run failed! can't find device!\n");
        return 1;
    }
    rt_pwm_disable(pwm, PWM_CHANNEL);
    return 0;
}

int wdt_start(void)
{
    if (rt_device_control(g_ctx.wdt, RT_DEVICE_CTRL_WDT_START, RT_NULL) != 0) {
        rt_kprintf("start %s failed!\n", WDT_DEV_NAME);
        return -1;
    }
    return 0;
}

int wdt_stop(void)
{
    if (rt_device_control(g_ctx.wdt, RT_DEVICE_CTRL_WDT_STOP, RT_NULL) != 0) {
        rt_kprintf("start %s failed!\n", WDT_DEV_NAME);       /* sic: the stock message says "start" here too */
        return -1;
    }
    return 0;
}

/* Fed from a periodic timer, but only when the host talked to us since the previous tick. */
void wdt_feed_hook(void *arg)
{
    (void)arg;
    if (!g_ctx.usb_active) {
        g_stats.wdt_nofeed++;
        app_log("wdt: no host traffic since the last tick, not feeding (reset in <= %d s)\n",
                   WDT_TIMEOUT_S);
        return;
    }
    rt_device_control(rt_device_find(WDT_DEV_NAME), RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    g_ctx.usb_active = 0;
}

/* INIT_APP_EXPORT: stock 0x4024396e */
int wdt_init(void)
{
    g_ctx.first_boot = 1;
    usb_serial_from_efuse();
    g_ctx.wdt_timeout = WDT_TIMEOUT_S;
    g_ctx.wdt = rt_device_find(WDT_DEV_NAME);
    if (g_ctx.wdt == RT_NULL) {
        rt_kprintf("find %s failed!\n", WDT_DEV_NAME);
    } else {
        rt_device_init(g_ctx.wdt);
        if (rt_device_control(g_ctx.wdt, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &g_ctx.wdt_timeout) != 0)
            rt_kprintf("set %s timeout failed!\n", WDT_DEV_NAME);
    }
    g_ctx.wdt_timer = rt_timer_create("wdt_timer_hook", wdt_feed_hook, RT_NULL,
                                      rt_tick_from_millisecond(WDT_HOOK_PERIOD_MS), RT_TIMER_FLAG_PERIODIC);
    if (g_ctx.wdt_timer != RT_NULL)
        rt_kputs("wdt_time_init success");
    return 0;
}
INIT_APP_EXPORT(wdt_init);
