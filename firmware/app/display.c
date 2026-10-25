/*
 * LCD output (800 x 480): 15 key tiles of 112 x 112 pixels, boot logo, double buffered frame buffer.
 * Rebuilt from stock 0x40241b04 (tile layout), 0x40241b86 (draw key image), 0x40241bf4 (clear),
 * 0x40241cb8 (flush), 0x40243ca6 (logo) and the decode/fill helpers they call (the SDK's decode_pic()
 * and a GE fill rectangle).
 *
 * Double buffering: g_clear_toggle selects the frame buffer page that is drawn to; CLE 0xff flips it and
 * clears the new page, STP pans the display to it.
 */
#include "akp05_app.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <aic_core.h>
#include <aic_osal.h>
#include "mpp_fb.h"
#include "mpp_ge.h"
#include "mpp_decoder.h"
#include "mpp_mem.h"
#include "artinchip_fb.h"

#include "font5x7.h"

/* Boot screen banner: firmware version and build time, drawn over the logo. CMake passes the three strings
 * (CMakeLists.txt: AKP05_FW_VERSION, git hash, time stamp of the configure step). */
#ifndef AKP05_FW_VERSION
#define AKP05_FW_VERSION "dev"
#endif
#ifndef AKP05_BUILD_DATE
#define AKP05_BUILD_DATE __DATE__
#endif
#ifndef AKP05_BUILD_TIME
#define AKP05_BUILD_TIME __TIME__
#endif
#define BANNER_SCALE    3                           /* pixels per font pixel */
#define BANNER_X        16
#define BANNER_LINE_H   (7 * BANNER_SCALE + 6)
#define BANNER_H        (2 * BANNER_LINE_H + 12)    /* black strip at the bottom of the screen */

#define TILE_W      112
#define TILE_H      112
#define TILE_DX     169
#define TILE_DY     174
#define TILE_X0     5
#define TILE_Y0     15

struct tile { uint16_t x1, y1, x2, y2; };
static struct tile tiles[NUM_KEYS];                 /* stock 0x402b8eb8 */

/* stock 0x402931d0: key number (1..15) -> tile number (see note in display_key_image) */
static const uint8_t key_to_tile[16] = { 4, 3, 2, 1, 0, 10, 9, 8, 7, 6, 15, 14, 13, 12, 11, 0 };

/* INIT_APP_EXPORT: stock 0x40241b04 */
int display_layout_init(void)
{
    int i;

    for (i = 0; i < NUM_KEYS; i++) {
        int col = i % 5, row = i / 5;

        tiles[i].x1 = (uint16_t)(TILE_X0 + col * TILE_DX);
        tiles[i].y1 = (uint16_t)(TILE_Y0 + row * TILE_DY);
        tiles[i].x2 = (uint16_t)(tiles[i].x1 + TILE_W);
        tiles[i].y2 = (uint16_t)(tiles[i].y1 + TILE_H);
    }
    return 0;
}
INIT_APP_EXPORT(display_layout_init);

/* ---- frame buffer helpers ------------------------------------------------------------------------ */
static int fb_info_get(struct mpp_fb *fb, struct aicfb_screeninfo *info)
{
    memset(info, 0, sizeof(*info));
    if (mpp_fb_ioctl(fb, AICFB_GET_SCREENINFO, info)) {
        printf("[E/aic_mpp] get screen info failed\n");
        return -1;
    }
    return 0;
}

static int ge_run(struct mpp_ge *ge)
{
    if (mpp_ge_emit(ge) < 0)
        return -1;
    return mpp_ge_sync(ge) < 0 ? -1 : 0;
}

/* Power the panel on once: black frame buffers first (the memory is not cleared by the allocator, the panel would
 * show whatever was there: grey lines and garbage), then AICFB_POWERON. Stock does this lazily in its fill helper
 * 0x402679d6; the logo drawn at boot (SDK decode_pic, which also pans) needs it too, so it lives here. */
static void fb_power_on_once(struct mpp_fb *fb, struct aicfb_screeninfo *info)
{
    static int powered;

    if (powered)
        return;
    memset((void *)info->framebuffer, 0, info->smem_len * 2);           /* both pages */
    aicos_dcache_clean_invalid_range((unsigned long *)info->framebuffer, info->smem_len * 2);
    mpp_fb_ioctl(fb, AICFB_POWERON, 0);
    powered = 1;
    {   /* The UI layer has four windows and there is a video layer; only UI window 0 is ours. The driver's copy of
         * their configuration is never initialised (it reads as disabled), but their hardware enable bits are
         * not necessarily clear after reset, so clear them for real: an update with enable = 0 writes the bit. */
        struct aicfb_layer_data layer;
        int rect;

        for (rect = 1; rect < 4; rect++) {
            memset(&layer, 0, sizeof(layer));
            layer.layer_id = AICFB_LAYER_TYPE_UI;
            layer.rect_id = rect;
            layer.enable = 0;
            mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer);
        }
        memset(&layer, 0, sizeof(layer));
        layer.layer_id = AICFB_LAYER_TYPE_VIDEO;
        layer.enable = 0;
        mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer);
        app_log("display build %s %s, firmware %s\n", __DATE__, __TIME__, AKP05_FW_VERSION);
    }
}

/* power the display on before anything is drawn */
static void fb_prepare(void)
{
    struct mpp_fb *fb = mpp_fb_open();
    struct aicfb_screeninfo info;

    if (fb == NULL)
        return;
    if (fb_info_get(fb, &info) == 0)
        fb_power_on_once(fb, &info);
    mpp_fb_close(fb);
}

/* fill a rectangle of page `page` with black: stock 0x402679d6. The display is powered on lazily here. */
static void fb_fill_rect(int x, int y, int w, int h, int page)
{
    struct mpp_fb *fb = mpp_fb_open();
    struct aicfb_screeninfo info;

    if (fb == NULL)
        return;
    if (fb_info_get(fb, &info) >= 0) {
        struct mpp_ge *ge;

        fb_power_on_once(fb, &info);
        ge = mpp_ge_open();
        if (ge != NULL) {
            struct ge_fillrect fill;

            memset(&fill, 0, sizeof(fill));
            fill.type = GE_NO_GRADIENT;
            fill.start_color = 0xff000000;
            fill.dst_buf.buf_type = MPP_PHY_ADDR;
            fill.dst_buf.phy_addr[0] = (unsigned long)info.framebuffer + (page ? info.smem_len : 0);
            fill.dst_buf.stride[0] = info.stride;
            fill.dst_buf.size.width = info.width;
            fill.dst_buf.size.height = info.height;
            fill.dst_buf.format = info.format;
            fill.dst_buf.crop_en = 1;
            fill.dst_buf.crop.x = x;
            fill.dst_buf.crop.y = y;
            fill.dst_buf.crop.width = w;
            fill.dst_buf.crop.height = h;
            if (mpp_ge_fillrect(ge, &fill) >= 0)
                ge_run(ge);
            mpp_ge_close(ge);
        }
    }
    mpp_fb_close(fb);
}

/* the JPEG / PNG decoder for `pic`, initialised for `len` bytes; NULL for an unsupported picture */
static struct mpp_decoder *decoder_create(const uint8_t *pic, int len, const struct aicfb_screeninfo *info)
{
    struct decode_config config;
    struct mpp_decoder *dec;

    memset(&config, 0, sizeof(config));
    config.bitstream_buffer_size = (len + 1023) & (~1023);
    config.extra_frame_num = 0;
    config.packet_count = 1;
    config.pix_fmt = info->format;             /* RGB straight from the decoder: the GE v11 cannot blit YUV */
    if (pic[0] == 0xff && pic[1] == 0xd8) {
        dec = mpp_decoder_create(MPP_CODEC_VIDEO_DECODER_MJPEG);
    } else if (pic[1] == 'P' && pic[2] == 'N' && pic[3] == 'G') {
        dec = mpp_decoder_create(MPP_CODEC_VIDEO_DECODER_PNG);
    } else {
        printf("[E/aic_mpp] decode_pic: unsupported picture\n");
        return NULL;
    }
    if (dec != NULL)
        mpp_decoder_init(dec, &config);
    return dec;
}

/* feed the picture to the decoder and blit the decoded frame to (x, y) of page `page`; 0 on success */
static int decode_and_blit(struct mpp_decoder *dec, const uint8_t *pic, int len,
                           const struct aicfb_screeninfo *info, int x, int y, int page)
{
    struct mpp_packet packet;
    struct mpp_frame frame;
    struct mpp_ge *ge;
    int ret = -1;

    memset(&packet, 0, sizeof(packet));
    mpp_decoder_get_packet(dec, &packet, len);
    memcpy(packet.data, pic, len);
    packet.size = len;
    packet.flag = PACKET_FLAG_EOS;
    mpp_decoder_put_packet(dec, &packet);
    if (mpp_decoder_decode(dec) < 0) {
        printf("[E/aic_mpp] decode error\n");
        return -1;
    }
    memset(&frame, 0, sizeof(frame));
    mpp_decoder_get_frame(dec, &frame);

    ge = mpp_ge_open();
    if (ge) {
        struct ge_bitblt blt;
        int r1, r2;

        memset(&blt, 0, sizeof(blt));
        memcpy(&blt.src_buf, &frame.buf, sizeof(struct mpp_buf));
        blt.dst_buf.buf_type = MPP_PHY_ADDR;
        blt.dst_buf.phy_addr[0] = (unsigned long)info->framebuffer + (page ? info->smem_len : 0);
        blt.dst_buf.format = info->format;
        blt.dst_buf.stride[0] = info->stride;
        blt.dst_buf.size.width = info->width;
        blt.dst_buf.size.height = info->height;
        blt.dst_buf.crop_en = 1;
        blt.dst_buf.crop.x = x;
        blt.dst_buf.crop.y = y;
        blt.dst_buf.crop.width = blt.src_buf.crop.width;
        blt.dst_buf.crop.height = blt.src_buf.crop.height;
        r1 = mpp_ge_bitblt(ge, &blt);
        r2 = (r1 >= 0) ? ge_run(ge) : -99;
        ret = r2;
        if (r2 < 0)
            app_err("draw failed: bitblt %d, run %d\n", r1, r2);
        mpp_ge_close(ge);
    } else {
        app_err("mpp_ge_open failed\n");
    }
    mpp_decoder_put_frame(dec, &frame);
    return ret;
}

/* decode a JPEG / PNG and blit it to the current page: the SDK's decode_pic() (mpp_test/pic_dec_test.c)
 * with the destination chosen by g_clear_toggle instead of the automatic pan. */
static int fb_draw_image(const uint8_t *pic, int len, int x, int y, int w, int h, int page)
{
    struct mpp_fb *fb;
    struct aicfb_screeninfo info;
    int ret = -1;

    (void)w; (void)h;
    if (pic == NULL || len <= 0) {
        printf("[E/aic_mpp] decode_pic: invalid parameter\n");
        return -1;
    }
    fb = mpp_fb_open();
    if (fb == NULL)
        return -1;
    if (fb_info_get(fb, &info) >= 0) {
        struct mpp_decoder *dec = decoder_create(pic, len, &info);

        if (dec != NULL) {
            ret = decode_and_blit(dec, pic, len, &info, x, y, page);
            mpp_decoder_destory(dec);
        }
    }
    mpp_fb_close(fb);
    return ret;
}

/* ---- public API ---------------------------------------------------------------------------------- */
/*
 * stock 0x40241b86. Keys 1..3 get a 182 pixel wide image shifted left by 35 px per step, the others a 112 x 112 one.
 * NOTE: the stock table maps key 5 to index -1 (a latent bug in the original, it reads a stale word before the
 * layout table); such keys are skipped here.
 */
void display_key_image(const uint8_t *buf, int size, int key, int layer)
{
    int tile;

    (void)layer;
    if (key < 1 || key > NUM_KEYS)
        return;
    tile = (int)key_to_tile[key - 1] - 1;
    if (tile < 0)
        return;
    if (key < 4) {
        int step = 4 - key;

        fb_draw_image(buf, size, tiles[tile].x1 + step * 35, tiles[tile].y1, 182, TILE_H, g_clear_toggle);
    } else {
        fb_draw_image(buf, size, tiles[tile].x1, tiles[tile].y1, TILE_W, TILE_H, g_clear_toggle);
    }
}

/* stock 0x40241bf4: 'C' clears the whole page, 0xff flips the page and clears it, 1..15 clears one tile */
void display_clear(int what)
{
    int tile;

    if (what == 'C') {
        fb_fill_rect(0, 0, LCD_W, LCD_H, g_clear_toggle);
        return;
    }
    if (what == 0xff) {
        g_clear_toggle = (g_clear_toggle == 0);
        fb_fill_rect(0, 0, LCD_W, LCD_H, g_clear_toggle);
        return;
    }
    if (what < 1 || what > NUM_KEYS)
        return;
    tile = (int)key_to_tile[what - 1] - 1;
    if (tile < 0)
        return;
    if (what < 5)
        fb_fill_rect(tiles[tile].x1 + (4 - what) * 36 - (4 - what), tiles[tile].y1, 184, 114, g_clear_toggle);
    else
        fb_fill_rect(tiles[tile].x1, tiles[tile].y1, 114, 114, g_clear_toggle);
}

/* stock 0x40241cb8: show the page that was drawn */
void display_flush(int arg)
{
    struct mpp_fb *fb = mpp_fb_open();
    unsigned int page = (unsigned int)arg;

    if (fb == NULL)
        return;
    mpp_fb_ioctl(fb, AICFB_PAN_DISPLAY, &page);
    mpp_fb_close(fb);
}

/* The panel is mounted upside down relative to the frame buffer (the key tiles and the logo are stored rotated by 180
 * degrees), so the banner is composed in screen coordinates (origin at the visible top left) and every pixel is
 * written at the rotated position of the buffer. Set to 0 for a panel that is not rotated. */
#define BANNER_ROTATE_180   1

struct banner_canvas {
    uint8_t  *mem;
    uint32_t  stride;
    int       w, h;
};

static void banner_pixel(const struct banner_canvas *cv, int x, int y)
{
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
        return;
#if BANNER_ROTATE_180
    x = cv->w - 1 - x;
    y = cv->h - 1 - y;
#endif
    ((uint16_t *)(cv->mem + (uint32_t)y * cv->stride))[x] = 0xffff;
}

/* draw one line of the banner (white pixels) at x, y of the visible screen */
static void banner_line(const struct banner_canvas *cv, int x, int y, const char *s)
{
    for (; *s; s++, x += 6 * BANNER_SCALE) {
        int c = *s;
        int row, col, dy, dx;

        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (c < FONT_FIRST || c > FONT_LAST)
            c = ' ';
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++) {
                if (!(font5x7[c - FONT_FIRST][row] & (1 << (4 - col))))
                    continue;
                for (dy = 0; dy < BANNER_SCALE; dy++)
                    for (dx = 0; dx < BANNER_SCALE; dx++)
                        banner_pixel(cv, x + col * BANNER_SCALE + dx, y + row * BANNER_SCALE + dy);
            }
    }
}

/* black strip with the firmware version and the build time at the bottom of the visible screen; the frame buffer of
 * this panel has 16 bits per pixel (stride 2 * width), anything else is left alone */
static void draw_banner(const struct aicfb_screeninfo *info, uint8_t *page_mem)
{
    struct banner_canvas cv = { page_mem, info->stride, (int)info->width, (int)info->height };
    char line[48];
    int y0 = cv.h - BANNER_H;
    int y;

    if (info->stride != info->width * 2 || y0 < 0)
        return;
    for (y = y0; y < cv.h; y++) {                   /* screen row y is buffer row h - 1 - y when rotated */
        int fy = BANNER_ROTATE_180 ? cv.h - 1 - y : y;

        memset(page_mem + (uint32_t)fy * info->stride, 0, info->stride);
    }
    rt_snprintf(line, sizeof(line), "AKP05E OPEN FIRMWARE %s", AKP05_FW_VERSION);
    banner_line(&cv, BANNER_X, y0 + 6, line);
    rt_snprintf(line, sizeof(line), "BUILT %s %s", AKP05_BUILD_DATE, AKP05_BUILD_TIME);
    banner_line(&cv, BANNER_X, y0 + 6 + BANNER_LINE_H, line);
}

/* stock 0x40243ca6: draw /data/logo_jpg_dir/test_logo.jpg full screen. Not in stock: the banner with the firmware
 * version, and a black screen with the banner when there is no logo file (stock leaves the screen dark). */
void display_logo(void)
{
    struct stat st;
    int have_logo = 0;
    int fd = open("/data/logo_jpg_dir/test_logo.jpg", O_RDWR);

    if (g_ctx.img_buf == NULL) {
        if (fd >= 0)
            close(fd);
        return;
    }
    if (fd >= 0) {
        if (fstat(fd, &st) != -1) {
            memset(g_ctx.img_buf, 0, IMG_BUF_SIZE);
            read(fd, g_ctx.img_buf, st.st_size);
            printf("file size is %ld\n", (long)st.st_size);
            have_logo = 1;
        }
        close(fd);
    }
    fb_prepare();
    /* Like the SDK's decode_pic() (the stock logo path): draw to the page that is NOT on screen, pan to it, wait for
     * the vertical blank, then copy it over the other page so both pages hold the same picture. */
    {
        struct mpp_fb *fb = mpp_fb_open();
        struct aicfb_screeninfo info;
        struct aicfb_layer_data layer;
        int page = 1;

        if (fb != NULL && fb_info_get(fb, &info) == 0) {
            memset(&layer, 0, sizeof(layer));
            layer.layer_id = AICFB_LAYER_TYPE_UI;
            layer.rect_id = 0;
            if (mpp_fb_ioctl(fb, AICFB_GET_LAYER_CONFIG, &layer) == 0)
                page = ((unsigned long)layer.buf.phy_addr[0] == (unsigned long)info.framebuffer) ? 1 : 0;
            if (!have_logo)
                fb_fill_rect(0, 0, LCD_W, LCD_H, page);
            if (!have_logo || fb_draw_image(g_ctx.img_buf, (int)st.st_size, 0, 0, LCD_W, LCD_H, page) == 0) {
                unsigned int pg = (unsigned int)page;
                uint8_t *shown = (uint8_t *)(unsigned long)(info.framebuffer + (page ? info.smem_len : 0));
                uint8_t *other = (uint8_t *)(unsigned long)(info.framebuffer + (page ? 0 : info.smem_len));

                aicos_dcache_clean_invalid_range((unsigned long *)shown, info.smem_len);   /* GE wrote it */
                draw_banner(&info, shown);
                aicos_dcache_clean_invalid_range((unsigned long *)shown, info.smem_len);
                mpp_fb_ioctl(fb, AICFB_PAN_DISPLAY, &pg);
                mpp_fb_ioctl(fb, AICFB_WAIT_FOR_VSYNC, 0);
                g_clear_toggle = (uint8_t)page;
                aicos_dcache_clean_invalid_range((unsigned long *)shown, info.smem_len);
                memcpy(other, shown, info.smem_len);
                aicos_dcache_clean_invalid_range((unsigned long *)other, info.smem_len);
                app_log("logo shown, page %d\n", page);
            } else {
                app_log("logo draw FAILED\n");
            }
        }
        if (fb != NULL)
            mpp_fb_close(fb);
    }
}

/* ---- UART test commands (msh), for bringing the display up without flashing again ------------------ */
#include <finsh.h>
#include <stdlib.h>

/* fbinfo: frame buffer geometry and what the two display layers currently point at */
static void cmd_fbinfo(int argc, char **argv)
{
    struct mpp_fb *fb = mpp_fb_open();
    struct aicfb_screeninfo info;
    struct aicfb_layer_data layer;
    int id;

    (void)argc;
    (void)argv;
    if (fb == NULL) {
        printf("fb open failed\n");
        return;
    }
    if (fb_info_get(fb, &info) == 0)
        printf("screen %dx%d fmt %d stride %d fb 0x%lx smem_len %d\n", (int)info.width, (int)info.height,
               (int)info.format, (int)info.stride, (unsigned long)info.framebuffer, (int)info.smem_len);
    for (id = 0; id < 2; id++) {
        int rect;

        for (rect = 0; rect < (id ? 4 : 1); rect++) {       /* the UI layer (1) has four rectangular windows */
            memset(&layer, 0, sizeof(layer));
            layer.layer_id = id;
            layer.rect_id = rect;
            if (mpp_fb_ioctl(fb, AICFB_GET_LAYER_CONFIG, &layer) < 0) {
                printf("layer %d rect %d: get config failed\n", id, rect);
                continue;
            }
            printf("layer %d rect %d: enable %d addr 0x%x stride %d size %dx%d fmt %d pos %d,%d scale %dx%d\n",
                   id, rect, (int)layer.enable, (unsigned)layer.buf.phy_addr[0], (int)layer.buf.stride[0],
                   (int)layer.buf.size.width, (int)layer.buf.size.height, (int)layer.buf.format,
                   (int)layer.pos.x, (int)layer.pos.y, (int)layer.scale_size.width,
                   (int)layer.scale_size.height);
        }
    }
    mpp_fb_close(fb);
}
MSH_CMD_EXPORT_ALIAS(cmd_fbinfo, fbinfo, show the frame buffer and layer configuration);

/* fbfill [RRGGBB|bars] [page]: CPU-fill a page of the frame buffer and pan to it (no decoder, no GE) */
static void cmd_fbfill(int argc, char **argv)
{
    static const unsigned bars[8] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0xffff00, 0x00ffff, 0xff00ff, 0x808080 };
    struct mpp_fb *fb;
    struct aicfb_screeninfo info;
    unsigned long rgb = 0xff0000;
    int bar_mode = 0, page = 0, x, y;
    unsigned int pg;
    uint16_t *p;

    if (argc > 1) {
        if (strcmp(argv[1], "bars") == 0)
            bar_mode = 1;
        else
            rgb = strtoul(argv[1], NULL, 16);
    }
    if (argc > 2)
        page = atoi(argv[2]) ? 1 : 0;
    fb_prepare();
    fb = mpp_fb_open();
    if (fb == NULL || fb_info_get(fb, &info) < 0) {
        printf("fb open failed\n");
        return;
    }
    p = (uint16_t *)(unsigned long)(info.framebuffer + (page ? info.smem_len : 0));
    for (y = 0; y < (int)info.height; y++) {
        for (x = 0; x < (int)info.width; x++) {
            unsigned long c = bar_mode ? bars[x * 8 / info.width] : rgb;
            uint16_t px = (uint16_t)((((c >> 16) & 0xff) >> 3) << 11 | (((c >> 8) & 0xff) >> 2) << 5 | ((c & 0xff) >> 3));

            if (x == 0 || y == 0 || x == (int)info.width - 1 || y == (int)info.height - 1)
                px = 0xffff;                    /* white 1 px frame: shows whether all four edges are visible */
            p[y * (info.stride / 2) + x] = px;
        }
    }
    aicos_dcache_clean_invalid_range((unsigned long *)p, info.smem_len);
    pg = (unsigned int)page;
    mpp_fb_ioctl(fb, AICFB_PAN_DISPLAY, &pg);
    mpp_fb_close(fb);
    printf("filled page %d at 0x%lx and panned\n", page, (unsigned long)p);
}
MSH_CMD_EXPORT_ALIAS(cmd_fbfill, fbfill, fbfill [RRGGBB|bars] [page]: fill the screen with a colour or colour bars);

/* showlogo: draw and show the boot logo again */
static void cmd_showlogo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    display_logo();
}
MSH_CMD_EXPORT_ALIAS(cmd_showlogo, showlogo, redraw the boot logo);
