/*
 * Command parser and ACK frames. Rebuilt from stock 0x40241ce0 (parser) and the "ACK..OK" frames that are
 * built at the start of 0x40242b98 / 0x40242cd8 / 0x40242ea2.
 *
 * Packet layout (host -> device, 1024 bytes):  'C' 'R' 'T' 00 00 <3..6 ASCII command letters> ...
 *   LOG    : length = big-endian u32 at bytes 8..11
 *   BGPIC  : length = big-endian u32 at bytes 10..13
 *   BAT    : length = big-endian u16 at bytes 10..11, key number in byte 12 of the header packet
 */
#include "akp05_app.h"
#include <string.h>

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void set_cmd(struct cmd_state *st, uint32_t id)
{
    st->data_cmd = 0;
    st->id = id;
}

static void set_data_cmd(struct cmd_state *st, uint32_t id, uint8_t kind, uint64_t len)
{
    st->id = id;
    st->data_cmd = 1;
    st->kind = kind;
    st->received = 0;
    st->length = len;
}

#define EQ(i, c) (p[i] == (uint8_t)(c))

void protocol_parse(const uint8_t *p, struct cmd_state *st)
{
    if (!(EQ(0, 'C') && EQ(1, 'R') && EQ(2, 'T')))
        return;

    switch (p[5]) {
    case 'C':
        if (EQ(6, 'L') && EQ(7, 'E'))
            set_cmd(st, CMD_CLE);
        else if (EQ(6, 'O') && EQ(7, 'N'))
            set_cmd(st, CMD_CON);
        break;
    case 'S':
        if (EQ(6, 'T') && EQ(7, 'P'))
            set_cmd(st, CMD_STP);
        else if (EQ(6, 'E') && EQ(7, 'T') && EQ(8, 'L') && EQ(9, 'B'))
            set_cmd(st, CMD_SETLB);
        break;
    case 'L':
        if (EQ(6, 'O') && EQ(7, 'G'))
            set_data_cmd(st, CMD_LOG, 4, be32(&p[8]));
        else if (EQ(6, 'B') && EQ(7, 'L') && EQ(8, 'I') && EQ(9, 'G'))
            set_cmd(st, CMD_LBLIG);
        else if (EQ(6, 'I') && EQ(7, 'G'))
            set_cmd(st, CMD_LIG);
        break;
    case 'B':
        if (EQ(6, 'G')) {
            if (EQ(7, 'P') && EQ(8, 'I') && EQ(9, 'C'))
                set_data_cmd(st, CMD_BGPIC, 10, be32(&p[10]));
            else if (EQ(7, 'C') && EQ(8, 'L') && EQ(9, 'E'))
                set_cmd(st, CMD_BGCLE);
        } else if (EQ(6, 'A') && EQ(7, 'T')) {
            set_data_cmd(st, CMD_BAT, 5, ((uint32_t)p[10] << 8) | p[11]);
        }
        break;
    case 'Q':
        if (EQ(6, 'U') && EQ(7, 'C') && EQ(8, 'M') && EQ(9, 'D'))
            set_cmd(st, CMD_QUCMD);
        break;
    case 'K':
        if (EQ(6, 'E') && EQ(7, 'Y'))
            set_cmd(st, CMD_KEY);
        break;
    case 'H':
        if (EQ(6, 'A') && EQ(7, 'N'))
            set_cmd(st, CMD_HAN);
        break;
    case 'D':
        if (EQ(6, 'I') && EQ(7, 'S'))
            set_cmd(st, CMD_DIS);
        else if (EQ(6, 'E') && EQ(7, 'L') && EQ(8, 'E') && EQ(9, 'D'))
            set_cmd(st, CMD_DELED);
        break;
    case 'A':
        if (EQ(6, 'P') && EQ(7, 'P') && EQ(8, 'N') && EQ(9, 'E') && EQ(10, 'W'))
            set_cmd(st, CMD_APPNEW);
        break;
    default:
        break;
    }
}

/* "ACK" 00 00 "OK" 00 ... : 0x4b4341 at +0, 0x4b4f00 at +4, rest zero (stock 0x40242ea2 prologue) */
void ack_init(uint8_t *frame)
{
    memset(frame, 0, PKT_IN_SIZE);
    frame[0] = 'A';
    frame[1] = 'C';
    frame[2] = 'K';
    frame[4] = 0;
    frame[5] = 'O';
    frame[6] = 'K';
}

void ack_key(uint8_t code, uint8_t state)
{
    uint8_t frame[PKT_IN_SIZE];

    ack_init(frame);
    frame[9] = code;
    frame[10] = state;
    hid_report_send(frame, PKT_IN_SIZE);
}
