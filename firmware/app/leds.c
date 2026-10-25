/*
 * RGB LED strip: 24 LEDs, WS2812 style, driven through QSPI as a bit stream (one byte per LED bit).
 * Rebuilt from stock 0x40241740 (toolchain disassembly).
 *
 *   - the 72 colour bytes are scaled with the brightness (0..100) as float and truncated to 8 bits,
 *   - every LED becomes 24 bytes: colour word G<<16 | R<<8 | B, MSB first, bit 1 -> 0x78, bit 0 -> 0x60,
 *   - the 576 byte buffer is sent as one single-line QSPI message.
 *
 * NOTE: in the stock image the QSPI device pointer (0x402b8eac) is never assigned, so the strip is never
 * driven ("QSPI device is not init yet."). The routine is kept as it is.
 */
#include "akp05_app.h"
#include <string.h>
#include <aic_osal.h>

#define LED_COUNT   24
#define LED_BYTES   (LED_COUNT * 24)         /* 576 */
#define LED_BIT0    0x60
#define LED_BIT1    0x78

uint8_t     g_led_rgb[72];
uint8_t     g_leds_on;
float       g_led_brightness;
float       g_led_saved;
int         g_led_mode;
rt_device_t g_led_qspi;

static uint8_t led_spi_buf[LED_BYTES];       /* stock 0x402b8c20 */

void leds_apply(const uint8_t *rgb72)
{
    uint8_t tmp[72];
    int i, bit;

    memcpy(tmp, rgb72, sizeof(tmp));
    for (i = 0; i < LED_COUNT; i++) {
        uint8_t r = (uint8_t)(uint32_t)(((float)tmp[i * 3 + 0] * g_led_brightness) / 100.0f);
        uint8_t g = (uint8_t)(uint32_t)(((float)tmp[i * 3 + 1] * g_led_brightness) / 100.0f);
        uint8_t b = (uint8_t)(uint32_t)(((float)tmp[i * 3 + 2] * g_led_brightness) / 100.0f);
        uint32_t word = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;

        tmp[i * 3 + 0] = r;
        tmp[i * 3 + 1] = g;
        tmp[i * 3 + 2] = b;
        for (bit = 0; bit < 24; bit++)
            led_spi_buf[i * 24 + bit] = ((word << bit) & 0x800000) ? LED_BIT1 : LED_BIT0;
    }

    if (g_led_qspi == RT_NULL) {
        rt_kputs("QSPI device is not init yet.");
        return;
    }
    {
        struct rt_qspi_message msg;
        uint8_t *dma = rt_malloc_align(LED_BYTES, 32);
        int sent;

        if (dma == RT_NULL) {
            rt_kputs("Low memory!");
            return;
        }
        memcpy(dma, led_spi_buf, LED_BYTES);
        memset(&msg, 0, sizeof(msg));
        msg.parent.send_buf = dma;
        msg.parent.length = LED_BYTES;
        msg.parent.cs_take = 1;
        msg.parent.cs_release = 1;
        msg.qspi_data_lines = 1;
        rt_spi_take_bus((struct rt_spi_device *)g_led_qspi);
        sent = rt_qspi_transfer_message((struct rt_qspi_device *)g_led_qspi, &msg);
        if (sent == LED_BYTES)
            rt_spi_release_bus((struct rt_spi_device *)g_led_qspi);
        else
            rt_kprintf("Send data failed. ret 0x%x\n", (int)sent);
        rt_free_align(dma);
    }
}
