"""Flash command against the software model of the bootloader (the model of test_sim.py plus the write commands).

Like test_sim.py this only shows that the host code is consistent with the device side SDK source; it does not
prove anything about real hardware.
"""
import struct
import unittest
import zlib

import upg_proto as P
from test_sim import FakeDevice


class FlashFakeDevice(FakeDevice):
    BLOCK = 0x10000

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.flash = bytearray(self.flash)
        self.meta = None
        self.last_crc = 0
        self.upg_end_seen = False
        self.media = "ram"              # a fresh updater writes to RAM until image.info selects the medium
        self.ram_writes = 0

    # -- host -> device
    def _device_write(self, data):
        if len(data) == 16 and struct.unpack_from("<I", data)[0] == P.UPGC_MAGIC:
            cmd = data[6]
            if cmd in (P.CMD_SET_FWC_META, P.CMD_GET_BLOCK_SIZE, P.CMD_SEND_FWC_DATA, P.CMD_GET_FWC_CRC,
                       P.CMD_GET_FWC_BURN_RESULT, P.CMD_SET_UPG_END):
                self.cmd = {"cmd": cmd, "state": "arg", "got": 0, "addr": 0, "len": 0, "idx": 0}
                if cmd == P.CMD_SEND_FWC_DATA:
                    self.meta["buf"] = bytearray()
                return
            super()._device_write(data)             # header of one of the read-only commands
            return
        c = self.cmd
        if c and c["cmd"] == P.CMD_SET_FWC_META:
            assert len(data) == P.FWC_META_LEN and data[:4] == b"META"
            parts = data[0x48:0x88].split(b"\0")[0].decode().split(";")
            size, crc = struct.unpack_from("<II", data, 0x8c)
            self.meta = dict(name=data[8:0x48].split(b"\0")[0].decode(), parts=parts, size=size, crc=crc)
            c["state"] = "resp"
            return
        if c and c["cmd"] == P.CMD_SEND_FWC_DATA:
            self.meta["buf"] += data
            if len(self.meta["buf"]) >= self.meta["size"]:
                buf = bytes(self.meta["buf"])
                if self.meta["name"] == "image.info":
                    self.media = buf[200:264].split(bytes(1))[0].decode()
                    self.last_crc = zlib.crc32(buf) & 0xFFFFFFFF
                    c["state"] = "resp"
                    return
                if self.media != "spi-nor":
                    self.ram_writes += 1
                    self.last_crc = zlib.crc32(buf) & 0xFFFFFFFF
                    c["state"] = "resp"
                    return
                for part in self.meta["parts"]:
                    start = next(s for n, s, _ in self.parts if n == part)
                    self.flash[start:start + len(buf)] = buf
                self.writes_to_flash += 1
                self.last_crc = zlib.crc32(buf) & 0xFFFFFFFF
                c["state"] = "resp"
            return
        if c and c["cmd"] == P.CMD_SET_UPG_END:
            if c["state"] == "arg":
                assert struct.unpack("<I", data)[0] == 32
                c["state"] = "cfg"
            else:
                assert len(data) == 32
                self.upg_end_seen = True
                c["state"] = "resp"
            return
        super()._device_write(data)

    # -- device -> host
    def _device_read(self, n):
        c = self.cmd
        if c and c["cmd"] in (P.CMD_SET_FWC_META, P.CMD_SEND_FWC_DATA, P.CMD_SET_UPG_END):
            assert c["state"] == "resp" and n == 16
            c["state"] = "end"
            return self._resp(c["cmd"], 0, 0)
        if c and c["cmd"] in (P.CMD_GET_BLOCK_SIZE, P.CMD_GET_FWC_CRC, P.CMD_GET_FWC_BURN_RESULT):
            if c["state"] == "arg":
                assert n == 16
                c["state"] = "data"
                return self._resp(c["cmd"], 0, 4)
            assert n == 4
            value = {P.CMD_GET_BLOCK_SIZE: self.BLOCK, P.CMD_GET_FWC_CRC: self.last_crc,
                     P.CMD_GET_FWC_BURN_RESULT: 0}[c["cmd"]]
            return struct.pack("<I", value)
        return super()._device_read(n)


class FlashTests(unittest.TestCase):
    def test_fwc_meta_layout(self):
        m = P.fwc_meta("image.target.os", "os", 1234, 0xdeadbeef, b"mtd;required")
        self.assertEqual(len(m), 512)
        self.assertEqual(m[:4], b"META")
        self.assertTrue(m[8:].startswith(b"image.target.os\0"))
        self.assertTrue(m[0x48:].startswith(b"os\0"))
        self.assertEqual(struct.unpack_from("<III", m, 0x88), (0, 1234, 0xdeadbeef))
        self.assertTrue(m[0x98:].startswith(b"mtd;required\0"))

    def test_without_image_info_the_write_goes_to_ram(self):
        dev = FlashFakeDevice()
        up = P.UpgWriter(dev)
        old = bytes(dev.flash)
        up.set_upg_cfg(P.UPG_MODE_PARTITION_UPGRADE)
        up.set_fwc_meta(P.fwc_meta("image.target.os", "os", 4096, zlib.crc32(bytes(4096))))
        up.send_fwc_data(bytes(4096))
        self.assertEqual((dev.ram_writes, dev.writes_to_flash), (1, 0))
        self.assertEqual(bytes(dev.flash), old)

    def test_image_info_selects_the_flash(self):
        dev = FlashFakeDevice()
        up = P.UpgWriter(dev)
        up.set_upg_cfg(P.UPG_MODE_PARTITION_UPGRADE)
        up.send_image_info()
        self.assertEqual(dev.media, "spi-nor")
        up.set_fwc_meta(P.fwc_meta("image.target.os", "os", 4096, zlib.crc32(bytes(4096))))
        up.send_fwc_data(bytes(4096))
        self.assertEqual((dev.ram_writes, dev.writes_to_flash), (0, 1))

    def test_write_then_read_back(self):
        dev = FlashFakeDevice()
        dev.media = "spi-nor"
        up = P.UpgWriter(dev)
        old = bytes(dev.flash)
        os_start = next(s for n, s, _ in dev.parts if n == "os")
        image = bytes(range(256)) * 700                      # 179200 bytes: three 64K slices
        crc = zlib.crc32(image) & 0xFFFFFFFF

        up.set_upg_cfg(P.UPG_MODE_PARTITION_UPGRADE)
        up.set_fwc_meta(P.fwc_meta("image.target.os", "os", len(image), crc))
        self.assertEqual(up.get_block_size(), FlashFakeDevice.BLOCK)
        up.send_fwc_data(image)
        self.assertEqual(up.get_fwc_crc(), crc)
        self.assertEqual(up.get_burn_result(), 0)
        up.set_upg_end()
        self.assertTrue(dev.upg_end_seen)

        self.assertEqual(bytes(dev.flash[os_start:os_start + len(image)]), image)
        # nothing outside the os partition was touched (the first partition, spl, in particular)
        self.assertEqual(bytes(dev.flash[:os_start]), old[:os_start])
        up.set_upg_cfg()
        self.assertEqual(up.read_partition("os", os_start, len(image)), image)

    def test_env_goes_to_both_copies(self):
        dev = FlashFakeDevice()
        up = P.UpgWriter(dev)
        env = b"E" * 4096
        up.send_image_info()
        up.set_fwc_meta(P.fwc_meta("image.target.env", "env;env_r", len(env), zlib.crc32(env), b"mtd;optional"))
        up.send_fwc_data(env)
        for name in ("env", "env_r"):
            start = next(s for n, s, _ in dev.parts if n == name)
            self.assertEqual(bytes(dev.flash[start:start + len(env)]), env)


if __name__ == "__main__":
    unittest.main()
