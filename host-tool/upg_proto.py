"""ArtInChip USB upgrade protocol (host side): partition dump and partition flash.

Written from the Luban-Lite SDK sources (application/baremetal/bootloader/lib/aicupg, include/aicupg.h,
include/trans_rw_data.h) and verified against a real AKP05E, through the HID bootloader (33C3:8899) and through the
BootROM updater (33C3:6677). See docs/bootloader-protocol.md for the full description.

Layers
  1. transport: bulk OUT/IN pipe of the upgrade device (VID 0x33C3, PID 0x6677)
  2. CBW/CSW:   mass-storage-like framing, one CBW per WRITE or READ transfer
  3. commands:  16-byte "UPGC" header, optional argument, "UPGR" response

Flash writes go through UpgWriter (partition upgrade mode, one firmware component per partition); the
BootROM-stage WRITE / EXEC commands only touch RAM (they load the vendor updater).
"""
import struct
import zlib

VID, PID = 0x33C3, 0x6677

CMD_GET_HWINFO = 0x00
CMD_WRITE = 0x02          # write to RAM address (BROM stage)
CMD_EXEC = 0x04           # jump to RAM address (BROM stage)
CMD_SET_UPG_CFG = 0x0A
CMD_GET_PARTITION_TABLE = 0x17
CMD_READ_FWC_DATA = 0x18

CMD_SET_UPG_END = 0x0B
CMD_SET_FWC_META = 0x10
CMD_GET_BLOCK_SIZE = 0x11
CMD_SEND_FWC_DATA = 0x12
CMD_GET_FWC_CRC = 0x13
CMD_GET_FWC_BURN_RESULT = 0x14

UPG_MODE_FULL_DISK_UPGRADE = 0
UPG_MODE_PARTITION_UPGRADE = 1
UPG_MODE_DUMP_PARTITION = 3
FWC_META_LEN = 512
CBW_SIG, CSW_SIG = 0x43425355, 0x53425355
UPGC_MAGIC, UPGR_MAGIC = 0x43475055, 0x52475055
TRANS_WRITE, TRANS_READ = 0x01, 0x02
CHUNK = 64 * 1024         # device slice size (TRANS_DATA_BUFF_SIZE); WRITE CBW carries only one slice

# Struct layouts measured with riscv64-unknown-elf-gcc -march=rv32imafdc -mabi=ilp32d
SZ_STORAGE_MEDIA = 68
SZ_MEDIA_PARTITION = 304
OFF_PART, OFF_START, OFF_SIZE, OFF_ATTR = 72, 216, 224, 240
PART_NAME_LEN = 144


class ProtoError(Exception):
    pass


def cmd_header(cmd, length):
    s = (UPGC_MAGIC + ((0 << 24) | (cmd << 16) | (1 << 8) | 1) + length) & 0xFFFFFFFF
    return struct.pack("<IBBBBII", UPGC_MAGIC, 1, 1, cmd, 0, length, s)


def parse_resp(b):
    if len(b) != 16:
        raise ProtoError("response header must be 16 bytes, got %d" % len(b))
    magic, proto, ver, cmd, status, length, csum = struct.unpack("<IBBBBII", b)
    if magic != UPGR_MAGIC:
        raise ProtoError("bad response magic 0x%08x" % magic)
    want = (magic + ((status << 24) | (cmd << 16) | (ver << 8) | proto) + length) & 0xFFFFFFFF
    if want != csum:
        raise ProtoError("response checksum mismatch")
    return cmd, status, length


def storage_media(media_type=b"spi-nor", dev_id=0):
    return struct.pack("<64sI", media_type, dev_id)


def _fwc_attr(name):
    return b"required" if name.startswith("spl") else b"mtd:required"


def media_partition(name, start, size, media_type=b"spi-nor", attr=None, name_len=PART_NAME_LEN):
    """struct media_partition as the RV32 bootloader declares it (fwc_cmd.c / upg_internal.h):
    storage_media(68) | pad | aic_partition{name[name_len], u64 start, u64 size, next*} | attr[64].
    name_len is MAX_PARTITION_NAME (144 in the akp05 SDK -> 304 bytes, 32 -> 192 bytes)."""
    start_off = OFF_PART + name_len
    attr_off = OFF_PART + ((name_len + 8 + 8 + 4 + 7) & ~7)
    b = bytearray(attr_off + 64)
    b[0:SZ_STORAGE_MEDIA] = storage_media(media_type)
    nb = name.encode()
    if len(nb) >= name_len:
        raise ValueError("partition name too long")
    b[OFF_PART:OFF_PART + len(nb)] = nb
    struct.pack_into("<QQ", b, start_off, start, size)
    attr = _fwc_attr(name) if attr is None else attr
    b[attr_off:attr_off + len(attr)] = attr
    return bytes(b)


class ReportTransport:
    """HID framing: every OUT transfer is padded with zeros to whole reports, IN data is read as whole
    reports and cut to the wanted length (the rest of a report is discarded).

    ep_out/ep_in need write(data, timeout) and read(size, timeout). A HID device on Windows only accepts
    reports of the full size (here 1024 payload bytes + report id 0, added by libusb), so a bare 31-byte
    CBW makes WriteFile fail with an I/O error.
    """
    def __init__(self, ep_out, ep_in, report=1024, timeout=10000):
        self.ep_out, self.ep_in, self.report, self.timeout = ep_out, ep_in, report, timeout

    def out(self, data):
        data = bytes(data)
        pad = bytes(1)
        for i in range(0, len(data), self.report):
            self.ep_out.write(data[i:i + self.report].ljust(self.report, pad), self.timeout)

    def inn(self, n):
        buf = bytearray()
        while len(buf) < n:
            rep = bytes(self.ep_in.read(self.report, self.timeout))
            if not rep:
                raise IOError("empty HID report")
            buf += rep[:n - len(buf)]
        return bytes(buf)


class Upg:
    def __init__(self, transport, log=None):
        self.t = transport
        self.tag = 0x1000
        self.log = log or (lambda *a: None)

    def _cbw(self, flags, length):
        self.tag += 1
        # bytes 12..15 as built by upgcmdHid.exe: flags, lun=0, cb_length=1, cb[0]=TRANS_WRITE/TRANS_READ
        return struct.pack("<IIIBBBB15s", CBW_SIG, self.tag, length, flags, 0, 1,
                           TRANS_WRITE if flags == 0 else TRANS_READ, b"\0" * 15)

    def _csw(self):
        b = self.t.inn(13)
        sig, tag, residue, status = struct.unpack("<IIIB", b)
        if sig != CSW_SIG or tag != self.tag:
            raise ProtoError("bad CSW (sig 0x%08x tag 0x%x)" % (sig, tag))
        if status != 0:
            raise ProtoError("CSW status %d, residue %d" % (status, residue))

    def write(self, data):
        if len(data) > CHUNK:
            raise ValueError("one WRITE transfer is limited to %d bytes" % CHUNK)
        self.t.out(self._cbw(0x00, len(data)))
        self.t.out(data)
        self._csw()

    def read(self, n):
        self.t.out(self._cbw(0x80, n))
        data = self.t.inn(n)
        self._csw()
        return data

    # --- command helpers ---------------------------------------------------
    def _resp(self):
        cmd, status, length = parse_resp(self.read(16))
        return status, length

    def hwinfo(self):
        self.write(cmd_header(CMD_GET_HWINFO, 0))
        status, length = self._resp()
        return status, self.read(length) if length else b""

    def ram_write(self, addr, data):
        """BROM-stage WRITE to RAM. RAM only."""
        self.write(cmd_header(CMD_WRITE, 8 + len(data)))
        self.write(struct.pack("<II", addr, len(data)))
        for i in range(0, len(data), CHUNK):
            self.write(data[i:i + CHUNK])
        status, _ = self._resp()
        if status != 0:
            raise ProtoError("WRITE to 0x%x failed, status %d" % (addr, status))

    def ram_exec(self, addr):
        """BROM-stage EXEC. The device may re-enumerate, so a USB error afterwards is expected."""
        self.write(cmd_header(CMD_EXEC, 4))
        self.write(struct.pack("<I", addr))
        status, _ = self._resp()
        return status

    def set_upg_cfg(self, mode=UPG_MODE_DUMP_PARTITION):
        """SET_UPG_CFG as sent by the vendor tool before a dump: header, u32 length (32), 32-byte cfg."""
        self.write(cmd_header(CMD_SET_UPG_CFG, 4 + 32))
        self.write(struct.pack("<I", 32))
        self.write(bytes([mode]) + bytes(31))
        status, _ = self._resp()
        if status != 0:
            raise ProtoError("SET_UPG_CFG status %d" % status)

    def partition_table(self, media_type=b"spi-nor"):
        self.write(cmd_header(CMD_GET_PARTITION_TABLE, SZ_STORAGE_MEDIA))
        self.write(storage_media(media_type))
        status, length = self._resp()
        if status != 0:
            raise ProtoError("GET_PARTITION_TABLE status %d" % status)
        return self.read(length).split(b"\0")[0].decode("ascii", "replace")

    def read_partition(self, name, start, size, progress=None, media_type=b"spi-nor", layout="rv144"):
        """Read one MTD partition (read-only).
        layout: rv144 (304 byte argument, name[144]: what the AKP05E bootloader expects) or rv32 (192 bytes, name[32]).
        The device waits for exactly sizeof(struct media_partition) bytes; any other length fails with CSW status 1."""
        build = {"rv144": media_partition,
                 "rv32": lambda *a: media_partition(*a, name_len=32)}[layout]
        arg = build(name, start, size, media_type)
        self.write(cmd_header(CMD_READ_FWC_DATA, len(arg)))
        self.write(arg)
        status, length = self._resp()
        if status != 0 or length != size:
            raise ProtoError("READ_FWC_DATA %s: status %d, length %d (wanted %d)"
                             % (name, status, length, size))
        out = bytearray()
        while len(out) < size:
            n = min(CHUNK, size - len(out))
            out += self.read(n)
            if progress:
                progress(len(out), size)
        return bytes(out)


def fwc_meta(name, partition, size, crc, attr=b"mtd;required", offset=0, ram=0):
    """512 byte "firmware component" record (struct fwc_meta of the bootloader, upg_internal.h):
    magic[8] "META", name[64], partition[64], offset, size, crc, ram (u32 each), attr[64], padding."""
    b = bytearray(FWC_META_LEN)
    b[0:4] = b"META"
    for off, text, limit in ((8, name, 64), (0x48, partition, 64)):
        raw = text.encode()
        if len(raw) >= limit:
            raise ValueError("field too long: " + text)
        b[off:off + len(raw)] = raw
    struct.pack_into("<IIII", b, 0x88, offset, size, crc & 0xFFFFFFFF, ram)
    b[0x98:0x98 + len(attr)] = attr
    return bytes(b)


class UpgWriter(Upg):
    """Flash write commands (device side: application/baremetal/bootloader/lib/aicupg/fwc_cmd.c).

    Sequence per component: SET_FWC_META, GET_BLOCK_SIZE, SEND_FWC_DATA, GET_FWC_CRC, GET_FWC_BURN_RESULT;
    one SET_UPG_END after the last component. The medium (spi-nor) must be selected first with send_image_info(),
    otherwise a fresh updater writes the component into its RAM medium.
    """

    def set_fwc_meta(self, meta):
        if len(meta) != FWC_META_LEN:
            raise ValueError("meta must be %d bytes" % FWC_META_LEN)
        self.write(cmd_header(CMD_SET_FWC_META, FWC_META_LEN))
        self.write(meta)
        status, _ = self._resp()
        if status != 0:
            raise ProtoError("SET_FWC_META status %d" % status)

    def _u32_reply(self, cmd):
        self.write(cmd_header(cmd, 0))
        status, length = self._resp()
        if status != 0 or length != 4:
            raise ProtoError("command 0x%02x: status %d, length %d" % (cmd, status, length))
        return struct.unpack("<I", self.read(4))[0]

    def get_block_size(self):
        return self._u32_reply(CMD_GET_BLOCK_SIZE)

    def get_fwc_crc(self):
        return self._u32_reply(CMD_GET_FWC_CRC)

    def get_burn_result(self):
        """0 = the component was written successfully"""
        return self._u32_reply(CMD_GET_FWC_BURN_RESULT)

    def send_fwc_data(self, data, progress=None):
        self.write(cmd_header(CMD_SEND_FWC_DATA, len(data)))
        sent = 0
        while sent < len(data):
            chunk = data[sent:sent + CHUNK]
            self.write(chunk)
            sent += len(chunk)
            if progress:
                progress(sent, len(data))
        status, _ = self._resp()
        if status != 0:
            raise ProtoError("SEND_FWC_DATA status %d" % status)

    def send_image_info(self, media_type="spi-nor", platform="d13x", product="AKP05", version="1.0.0"):
        """The "image.info" component (the firmware image header, 2048 bytes). The bootloader reads the media
        type from it and switches the target of the FOLLOWING components to it (ram_fwc.c). Without it a fresh
        updater writes to its RAM media: block size 1, the data never reaches the flash (the "flash twice" effect)."""
        hdr = bytearray(2048)
        hdr[0:6] = b"AIC.FW"
        for off, text in ((8, platform), (72, product), (136, version), (200, media_type)):
            hdr[off:off + len(text)] = text.encode()
        data = bytes(hdr)
        self.set_fwc_meta(fwc_meta("image.info", "", len(data), zlib.crc32(data), b"required"))
        self.get_block_size()
        self.send_fwc_data(data)
        self.get_fwc_crc()
        result = self.get_burn_result()
        if result != 0:
            raise ProtoError("image.info was refused, result %d" % result)

    def set_upg_end(self):
        self.write(cmd_header(CMD_SET_UPG_END, 4 + 32))
        self.write(struct.pack("<I", 32))
        self.write(bytes(32))
        status, _ = self._resp()
        if status != 0:
            raise ProtoError("SET_UPG_END status %d" % status)

def parse_size(s):
    s = s.strip().lower()
    mult = {"k": 1024, "m": 1024 * 1024}.get(s[-1], 1)
    return int(s[:-1] if s[-1] in "km" else s) * mult


def parse_mtd(table):
    """'spi0.0:256k(spl),128k(env),...' (optionally prefixed with 'mtd=') -> [(name, start, size)]."""
    import re
    parts, off = [], 0
    for size, name in re.findall(r"(\d+[kKmM]?)\(([^)]+)\)", table):
        sz = parse_size(size)
        parts.append((name, off, sz))
        off += sz
    return parts
