/*
 * Writing the uploaded boot logo to the littlefs "data" partition: stock 0x40243bf6.
 *
 * The payload starts 0x400 bytes into the staging buffer (after the 1024 byte header packet); its length is the
 * big-endian u32 at bytes 8..11 of the header packet. The flags 0xa02 / 0x402 are RT-Thread DFS flags.
 */
#include "akp05_app.h"
#include <unistd.h>
#include <fcntl.h>

#define DFS_O_RDWR_CREAT_APPEND  0xa02
#define DFS_O_RDWR_APPEND        0x402

static int logo_fd;

int logo_write_file(const char *path, const uint8_t *img_buf, uint32_t len_unused)
{
    uint32_t len = ((uint32_t)img_buf[8] << 24) | ((uint32_t)img_buf[9] << 16) |
                   ((uint32_t)img_buf[10] << 8) | img_buf[11];
    int n;

    (void)len_unused;
    logo_fd = open(path, DFS_O_RDWR_CREAT_APPEND);
    if (logo_fd < 0) {
        logo_fd = open(path, DFS_O_RDWR_APPEND);
        if (logo_fd < 0) {
            rt_kprintf("[E/%s] %s()%d open file Error :%s", "usb", "write_image_to_flie", 35, path);
            return -1;
        }
    }
    n = write(logo_fd, img_buf + 0x400, len);
    close(logo_fd);
    return n;
}
