/*
 * Buffers between the USB endpoint callback and the receive thread.
 *   - rx ring: stock 0x402421cc (init), 0x40242220 (write), 0x402422e0 (read), globals 0x402b8f30..0x402b8f3c
 *   - kfifo  : stock 0x40242052 (init, size must be a power of two), 0x402420a0 (in), 0x40242142 (out)
 */
#include "akp05_app.h"
#include <string.h>
#include <aic_osal.h>

/* ----- plain ring (head/tail pointers, no wrap bookkeeping other than the pointers themselves) */
static uint8_t *rb_start;       /* 0x402b8f30 */
static uint8_t *rb_end;         /* 0x402b8f34 */
static uint8_t *rb_rd;          /* 0x402b8f38 */
static uint8_t *rb_wr;          /* 0x402b8f3c */

void rx_ring_init(uint32_t size)
{
    if (rb_start == NULL)
        rb_start = aicos_malloc(MEM_CMA, size);
    if (rb_start == NULL) {
        app_err("rx ring: out of memory (%u bytes)\n", (unsigned)size);
        return;
    }
    memset(rb_start, 0, size);
    rb_rd = rb_start;
    rb_wr = rb_start;
    rb_end = rb_start + size;
}

int rx_ring_write(const uint8_t *src, int len)
{
    if (rb_start == NULL) {
        rt_kputs("WriteRingBuff:RingBuff is not Init!");
        return -1;
    }
    if ((int)(rb_end - rb_start) < len) {
        rt_kputs("WriteRingBuff:New add buff is too long");
        return -1;
    }
    if (rb_end < rb_wr + len) {
        int first = rb_end - rb_wr;

        memcpy(rb_wr, src, first);
        memcpy(rb_start, src + first, len - first);
        rb_wr = rb_start + (len - first);
        return 0;
    }
    memcpy(rb_wr, src, len);
    rb_wr += len;
    return 0;
}

uint32_t rx_ring_used(void)
{
    uint8_t *wr = rb_wr, *rd = rb_rd;

    if (rb_start == NULL)
        return 0;
    return (uint32_t)(wr >= rd ? wr - rd : (rb_end - rb_start) - (rd - wr));
}

int rx_ring_read(uint8_t *dst, int len)
{
    if (rb_start == NULL) {
        rt_kputs("ReadRingBuff:RingBuff is not Init!");
        return -1;
    }
    if ((int)(rb_end - rb_start) < len) {
        rt_kputs("ReadRingBuff:Read buff size is too long");
        return -1;
    }
    if (len == 0)
        return 0;
    if (rb_end < rb_rd + len) {
        int first = rb_end - rb_rd;

        memcpy(dst, rb_rd, first);
        memcpy(dst + first, rb_start, len - first);
        rb_rd = rb_start + (len - first);
    } else {
        memcpy(dst, rb_rd, len);
        rb_rd += len;
    }
    return len;
}

/* ----- kfifo (single producer / single consumer, free running indexes, size = 2^n) */
int kfifo_init(struct kfifo *f, uint32_t size)
{
    if (size == 0)
        return 0;
    if ((size - 1) & size)
        return 0;                           /* not a power of two */
    f->data = aicos_malloc(MEM_CMA, size);
    if (f->data == NULL)
        return 0;
    f->size = size;
    f->out = 0;
    f->in = 0;
    return 1;
}

uint32_t kfifo_in(struct kfifo *f, const void *src, uint32_t len)
{
    uint32_t in = f->in, out = f->out;
    uint32_t space = f->size - (in - out);
    uint32_t off, first;

    if (len > space)
        len = space;
    if (len == 0)
        return 0;
    off = in & (f->size - 1);
    first = f->size - off;
    if (first > len)
        first = len;
    memcpy(f->data + off, src, first);
    memcpy(f->data, (const uint8_t *)src + first, len - first);
    f->in = in + len;
    return len;
}

uint32_t kfifo_out(struct kfifo *f, void *dst, uint32_t len)
{
    uint32_t in = f->in, out = f->out;
    uint32_t avail = in - out;
    uint32_t off, first;

    if (len > avail)
        len = avail;
    if (len == 0)
        return 0;
    off = out & (f->size - 1);
    first = f->size - off;
    if (first > len)
        first = len;
    memcpy(dst, f->data + off, first);
    memcpy((uint8_t *)dst + first, f->data, len - first);
    f->out = out + len;
    return len;
}
