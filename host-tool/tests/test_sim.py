"""Protocol self-test against a software model of the SDK's device side.

This only proves the host code is self-consistent with my reading of the SDK
(framing, struct offsets, chunking). It says nothing about real hardware.
Run (from the repository root):  PYTHONPATH=src python -m unittest discover -s src/tests
"""
import os
import struct
import unittest

import upg_proto as P


class FakeDevice:
    """Mirrors trans_layer_rw_proc() + aicupg_data_packet_write/read() + the used commands."""

    def __init__(self, mtd="spi0.0:512k(spl),128k(env),128k(env_r),2m(os),1m(rodata),10m(data)", name_len=144):
        self.mtd = mtd
        self.name_len = name_len        # MAX_PARTITION_NAME of the device-side struct (RV32, 8-byte aligned u64)
        self.cfg = None
        self.parts = P.parse_mtd(mtd)
        total = sum(s for _, _, s in self.parts)
        self.flash = os.urandom(total)
        self.ram = {}
        self.exec_calls = []
        self.inq = bytearray()
        self.pending = None      # (kind, tag, length)
        self.buf = bytearray()
        self.cmd = None          # current command state
        self.writes_to_flash = 0

    # ---- host-facing pipe
    def out(self, data):
        data = bytes(data)
        if self.pending is None:
            assert len(data) == 31, "CBW must be 31 bytes"
            sig, tag, length, flags, _, cblen, bcmd, _ = struct.unpack("<IIIBBBB15s", data)
            assert cblen == 1, "real device ignores a CBW whose cb_length is not 1"
            assert sig == P.CBW_SIG
            if bcmd == P.TRANS_WRITE:
                assert flags == 0
                assert length <= P.CHUNK, "device handles only one slice per WRITE CBW"
                self.pending = ("w", tag, length)
                self.buf = bytearray()
            else:
                assert bcmd == P.TRANS_READ and flags == 0x80
                chunk = self._device_read(length)
                assert len(chunk) == length
                self.inq += chunk
                self.inq += struct.pack("<IIIB", P.CSW_SIG, tag, 0, 0)
        else:
            _, tag, length = self.pending
            self.buf += data
            if len(self.buf) == length:
                self.reject = False
                self._device_write(bytes(self.buf))
                if self.reject:                          # observed: CSW status 1, residue = whole data stage
                    self.inq += struct.pack("<IIIB", P.CSW_SIG, tag, length, 1)
                else:
                    self.inq += struct.pack("<IIIB", P.CSW_SIG, tag, 0, 0)
                self.pending = None

    def inn(self, n):
        assert len(self.inq) >= n, "host read more than the device produced"
        b = bytes(self.inq[:n])
        del self.inq[:n]
        return b

    # ---- device logic
    @staticmethod
    def _resp(cmd, status, length):
        s = (P.UPGR_MAGIC + ((status << 24) | (cmd << 16) | (1 << 8) | 1) + length) & 0xFFFFFFFF
        return struct.pack("<IBBBBII", P.UPGR_MAGIC, 1, 1, cmd, status, length, s)

    def _device_write(self, data):
        if len(data) == 16 and struct.unpack_from("<I", data)[0] == P.UPGC_MAGIC:
            magic, proto, ver, cmd, _, length, csum = struct.unpack("<IBBBBII", data)
            assert csum == (magic + ((cmd << 16) | (ver << 8) | proto) + length) & 0xFFFFFFFF
            self.cmd = {"cmd": cmd, "state": "arg", "got": 0, "addr": 0, "len": 0, "idx": 0}
            return
        c = self.cmd
        assert c is not None
        if c["cmd"] == P.CMD_WRITE:
            if c["state"] == "arg":
                c["addr"], c["len"] = struct.unpack("<II", data)
                c["state"] = "data"
            else:
                a = c["addr"] + c["idx"]
                for i, byte in enumerate(data):
                    self.ram[a + i] = byte
                c["idx"] += len(data)
                if c["idx"] >= c["len"]:
                    c["state"] = "resp"
        elif c["cmd"] == P.CMD_EXEC:
            (addr,) = struct.unpack("<I", data)
            self.exec_calls.append(addr)
            c["state"] = "resp"
        elif c["cmd"] == P.CMD_GET_PARTITION_TABLE:
            assert len(data) == P.SZ_STORAGE_MEDIA
            assert data[:7] == b"spi-nor"
            c["state"] = "resp"
        elif c["cmd"] == P.CMD_SET_UPG_CFG:
            if c["state"] == "arg":
                assert struct.unpack("<I", data)[0] == 32
                c["state"] = "cfg"
            else:
                assert len(data) == 32
                self.cfg = data[0]
                c["state"] = "resp"
        elif c["cmd"] == P.CMD_READ_FWC_DATA:
            # fwc_cmd.c: the handler needs sizeof(struct media_partition) of the RV32 build (MAX_PARTITION_NAME = L);
            # observed on the real device: 184 B (vendor tool) and 512 B are both answered with CSW status 1
            L = self.name_len
            sizeof_mp = 72 + ((L + 20 + 7) & ~7) + 64
            if len(data) != sizeof_mp:
                self.reject = True
                return
            name = data[72:72 + L].split(b"\0")[0].decode()
            start, size = struct.unpack_from("<QQ", data, 72 + L)
            attr = data[sizeof_mp - 64:].split(b"\0")[0]
            assert attr == (b"required" if name.startswith("spl") else b"mtd:required"), attr
            c.update(name=name, start=start, size=size, idx=0, state="resp")
        else:
            raise AssertionError("unexpected write for command %d" % c["cmd"])

    def _device_read(self, n):
        c = self.cmd
        if c["cmd"] == P.CMD_GET_HWINFO:
            if c["state"] == "arg":
                c["state"] = "data"
                assert n == 16
                return self._resp(0, 0, 108)
            return b"AICHW\0\0\0" + bytes(30) + bytes([1, 1, 0]) + bytes(67)
        if c["cmd"] == P.CMD_SET_UPG_CFG:
            assert c["state"] == "resp" and n == 16
            c["state"] = "end"
            return self._resp(c["cmd"], 0, 0)
        if c["cmd"] in (P.CMD_WRITE, P.CMD_EXEC):
            assert c["state"] == "resp" and n == 16
            c["state"] = "end"
            return self._resp(c["cmd"], 0, 0)
        if c["cmd"] == P.CMD_GET_PARTITION_TABLE:
            if c["state"] == "resp":
                assert n == 16
                c["state"] = "data"
                return self._resp(c["cmd"], 0, 128)
            assert n == 128
            return self.mtd.encode().ljust(128, b"\0")
        if c["cmd"] == P.CMD_READ_FWC_DATA:
            if c["state"] == "arg":
                raise TimeoutError("device did not answer READ_FWC_DATA (argument too short)")
            if c["state"] == "resp":
                assert n == 16
                c["state"] = "data"
                return self._resp(c["cmd"], 0, c["size"])
            base = next(s for nm, s, _ in self.parts if nm == c["name"])
            out = self.flash[base + c["idx"]: base + c["idx"] + n]
            c["idx"] += n
            return out
        raise AssertionError


class Tests(unittest.TestCase):
    def test_header_checksum(self):
        h = P.cmd_header(P.CMD_GET_PARTITION_TABLE, 68)
        self.assertEqual(len(h), 16)
        magic, proto, ver, cmd, res, length, csum = struct.unpack("<IBBBBII", h)
        self.assertEqual(csum, (magic + ((cmd << 16) | (ver << 8) | proto) + length) & 0xFFFFFFFF)

    def test_struct_offsets(self):
        mp = P.media_partition("os", 0x1000, 0x200000)
        self.assertEqual(len(mp), 304)
        self.assertEqual(mp[72:74], b"os")
        self.assertEqual(struct.unpack_from("<QQ", mp, 216), (0x1000, 0x200000))
        self.assertTrue(mp[240:].startswith(b"mtd:required\0"))
        self.assertEqual(len(P.media_partition("os", 0, 1, name_len=32)), 192)

    def test_full_dump_matches_flash(self):
        for name_len, layout in ((32, "rv32"), (144, "rv144")):   # argument size must match the device build
            dev = FakeDevice(name_len=name_len)
            up = P.Upg(dev)
            status, hw = up.hwinfo()
            self.assertEqual(status, 0)
            self.assertEqual(hw[:5], b"AICHW")
            table = up.partition_table()
            parts = P.parse_mtd(table)
            self.assertEqual([p[0] for p in parts], ["spl", "env", "env_r", "os", "rodata", "data"])
            self.assertEqual(parts[1], ("env", 0x80000, 0x20000))
            up.set_upg_cfg()
            self.assertEqual(dev.cfg, P.UPG_MODE_DUMP_PARTITION)
            image = b"".join(up.read_partition(n, s, z, layout=layout) for n, s, z in parts)
            self.assertEqual(image, dev.flash, "name_len=%d" % name_len)

    def test_wrong_sized_argument_is_rejected_like_the_real_device(self):
        """The device waits for exactly sizeof(struct media_partition); 184 bytes (a 32-bit x86 build, the argument of
        the vendor's Windows tool) or 512 bytes make the data stage fail with CSW status 1."""
        for size in (184, 512):
            dev = FakeDevice()
            up = P.Upg(dev)
            up.set_upg_cfg()
            with self.assertRaises(P.ProtoError):
                up.write(P.cmd_header(P.CMD_READ_FWC_DATA, size))
                up.write(bytes(size))

    def test_ram_load_and_exec(self):
        dev = FakeDevice()
        up = P.Upg(dev)
        data = os.urandom(200 * 1024)          # > one 64K slice -> multiple WRITE CBWs
        up.ram_write(0x40100000, data)
        self.assertEqual(bytes(dev.ram[0x40100000 + i] for i in range(len(data))), data)
        self.assertEqual(up.ram_exec(0x40100000), 0)
        self.assertEqual(dev.exec_calls, [0x40100000])

    def test_parse_mtd(self):
        self.assertEqual(P.parse_mtd("mtd=spi0.0:256k(spl),2m(os)"), [("spl", 0, 262144), ("os", 262144, 2097152)])


class HidSim:
    """HID view of FakeDevice: like Windows WriteFile, only whole 1024-byte reports are accepted;
    the device answers with whole (zero padded) reports. One logical transfer = one or more reports."""
    REPORT = 1024

    def __init__(self, dev):
        self.dev, self.q = dev, []

    def _push(self, blob):
        pad = bytes(1)
        for i in range(0, len(blob), self.REPORT):
            self.q.append(blob[i:i + self.REPORT].ljust(self.REPORT, pad))

    def write(self, data, timeout=None):
        data = bytes(data)
        if len(data) != self.REPORT:
            raise OSError("Input/Output Error (HID write must be a full %d-byte report)" % self.REPORT)
        d = self.dev
        if d.pending is None:                         # first report of a transfer = CBW
            sig, tag, length, flags, _, cblen, bcmd, _ = struct.unpack("<IIIBBBB15s", data[:31])
            assert cblen == 1, "real device ignores a CBW whose cb_length is not 1"
            d.out(data[:31])
            if flags == 0x80:                         # READ: <length> data bytes, then the CSW as its own report
                inq = bytes(d.inq); d.inq.clear()
                self._push(inq[:length]); self._push(inq[length:length + 13])
        else:
            _, tag, length = d.pending
            d.out(data[:min(length - len(d.buf), self.REPORT)])
            if d.pending is None:                     # WRITE finished -> CSW report
                inq = bytes(d.inq); d.inq.clear()
                self._push(inq)

    def read(self, size, timeout=None):
        if size != self.REPORT:
            raise OSError("HID read must request a full report")
        if not self.q:
            raise TimeoutError("no report from device")
        return self.q.pop(0)


class HidTests(unittest.TestCase):
    def test_unpadded_cbw_fails_like_the_real_device(self):
        sim = HidSim(FakeDevice())
        with self.assertRaises(OSError):
            P.Upg(_Raw(sim)).hwinfo()

    def test_full_dump_over_hid_reports(self):
        for name_len, layout in ((32, "rv32"), (144, "rv144")):
            dev = FakeDevice(name_len=name_len)
            sim = HidSim(dev)
            up = P.Upg(P.ReportTransport(sim, sim, 1024))
            status, hw = up.hwinfo()
            self.assertEqual(status, 0)
            parts = P.parse_mtd(up.partition_table())
            up.set_upg_cfg()
            image = b"".join(up.read_partition(n, s, z, layout=layout) for n, s, z in parts)
            self.assertEqual(image, dev.flash)


class _Raw:
    """Transport without padding (the first attempt that failed on the real device)."""
    def __init__(self, sim):
        self.sim = sim

    def out(self, data):
        self.sim.write(bytes(data))

    def inn(self, n):
        return self.sim.read(n)


class PipeSim:
    """HidPipe look-alike on top of HidSim: write(report incl. id) / read(size incl. id)."""
    def __init__(self, sim):
        self.sim = sim

    def write(self, report):
        assert report[0] == 0 and len(report) == 1025
        self.sim.write(report[1:])
        return len(report)

    def read(self, size):
        assert size == 1025
        return bytes(1) + self.sim.read(1024)


class WinHidAdapterTests(unittest.TestCase):
    def test_pipe_endpoint_report_id_handling(self):
        import akp05_tool as A
        dev = FakeDevice(name_len=144)
        pipe = PipeSim(HidSim(dev))
        up = P.Upg(P.ReportTransport(A.PipeEndpoint(pipe), A.PipeEndpoint(pipe), 1024))
        status, hw = up.hwinfo()
        self.assertEqual(status, 0)
        parts = P.parse_mtd(up.partition_table())
        up.set_upg_cfg()
        env = next(p for p in parts if p[0] == "env")
        data = up.read_partition(*env)
        self.assertEqual(data, dev.flash[env[1]:env[1] + env[2]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
