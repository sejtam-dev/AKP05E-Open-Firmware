#!/usr/bin/env python3
"""Host tool for ArtInChip based Stream Dock devices (Ajazz AKP05E): dump, flash, recover and talk to the device.

Every step is a separate command, nothing happens implicitly. Commands are grouped by the state the device is in.

Running firmware (USB 0300:3004, HID):
  hid-list          list the HID interfaces of the Stream Dock (read only)
  hid-talk          send host commands (DIS, LIG:30, ...) and print the key / knob / touch events it reports
  logo              upload a new boot logo (LOG command) without flashing
  enter-bootloader  send the HID packet that reboots the firmware into the HID bootloader (CRT APPNEW)

HID bootloader (USB 33C3:8899, reached with enter-bootloader or "upghid" on the UART):
  hid-hwinfo        HWINFO incl. boot_stage (must be 1)  (read only)
  hid-dump          read all flash partitions into files  (read only)
  flash             write os / rodata / data / env, dry run unless --write; never writes spl; verifies by reading back

BootROM recovery (USB 33C3:6677 after shorting PA.0 to GND, see docs/boot-and-recovery.md):
  usb-info          HWINFO of the BootROM / updater (read only)
  load-updater      load the vendor updater into RAM (nothing is written to flash); afterwards use
                    "flash --transport bulk" and "hid-dump --transport bulk" against the updater

Verified on a real AKP05E: hid-list, enter-bootloader, hid-hwinfo, hid-dump, flash (through the HID bootloader and
through the BootROM updater with --transport bulk), hid-dump through the updater (--transport bulk), usb-info,
load-updater, logo and hid-talk. See docs/host-tool.md.
"""
import argparse
import os
import struct
import sys
import time
import zlib

from upg_proto import (PID, VID, Upg, UpgWriter, ProtoError, ReportTransport, parse_mtd, fwc_meta,
                       UPG_MODE_PARTITION_UPGRADE)

STREAMDOCK_VID = 0x0300
APPNEW_PACKET  = b"CRT" + bytes(2) + b"APPNEW"     # host command: reboot into the HID bootloader
SELFTEST_PACKET = b"CRT" + bytes(2) + b"SELFT"    # host command: toggle selftest mode


# ---------------------------------------------------------------- transports
class _Raw:
    """Endpoint stand-in for devices whose descriptors libusb cannot walk."""
    def __init__(self, dev, addr, timeout):
        self.dev, self.addr, self.timeout = dev, addr, timeout

    def write(self, data, timeout=None):
        return self.dev.write(self.addr, data, timeout or self.timeout)

    def read(self, n, timeout=None):
        return self.dev.read(self.addr, n, timeout or self.timeout)


class UsbTransport:
    def __init__(self, vid=VID, pid=PID, timeout_ms=10000, report=0, verbose=False, libusb_path=None):
        self.verbose = verbose
        import usb.core
        import usb.util
        backend = None
        try:  # Windows: bundled libusb DLL
            import libusb_package
            import usb.backend.libusb1
            finder = (lambda name: libusb_path) if libusb_path else libusb_package.find_library
            backend = usb.backend.libusb1.get_backend(find_library=finder)
            if libusb_path:
                print('using libusb:', libusb_path)
        except ImportError:
            pass
        self.dev = usb.core.find(idVendor=vid, idProduct=pid, backend=backend)
        if self.dev is None:
            raise SystemExit("USB device %04x:%04x not found (is it in bootloader mode, "
                             "and is the libusbK/WinUSB driver bound to it?)" % (vid, pid))
        self.timeout = timeout_ms
        # libusb exposes HID devices through a synthetic configuration; be tolerant about it
        try:
            self.dev.set_configuration()
        except Exception as exc:
            print("note: set_configuration skipped (%s)" % exc)
        self.ep_out = self.ep_in = None
        try:
            for cfg in self.dev:
                for intf in cfg:
                    for ep in intf:
                        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT and self.ep_out is None:
                            self.ep_out = ep
                        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN and self.ep_in is None:
                            self.ep_in = ep
        except Exception as exc:
            print("note: descriptor walk failed (%s)" % exc)
        if self.ep_out is None or self.ep_in is None:
            print("note: endpoints not found in descriptors, assuming 0x01 / 0x81")
            self.ep_out = _Raw(self.dev, 0x01, self.timeout)
            self.ep_in = _Raw(self.dev, 0x81, self.timeout)
        self.rt = ReportTransport(self.ep_out, self.ep_in, report, self.timeout) if report else None

    def out(self, data):
        if self.verbose:
            print("  OUT %4d B  %s" % (len(data), bytes(data[:16]).hex(" ")))
        if self.rt:
            return self.rt.out(data)
        self.ep_out.write(data, self.timeout)

    def inn(self, n):
        r = self._inn(n)
        if self.verbose:
            print("  IN  %4d B  %s" % (len(r), r[:16].hex(" ")))
        return r

    def _inn(self, n):
        if self.rt:
            return self.rt.inn(n)
        buf = bytearray()
        while len(buf) < n:
            chunk = self.ep_in.read(min(n - len(buf), 65536), self.timeout)
            if not len(chunk):
                raise IOError("short read")
            buf += bytes(chunk)
        return bytes(buf)


class PipeEndpoint:
    """Adapts HidPipe (reports include the report-id byte) to the write(payload)/read(size) endpoint API."""

    def __init__(self, pipe):
        self.pipe = pipe

    def write(self, data, timeout=None):
        return self.pipe.write(bytes(1) + bytes(data))     # report id 0 + payload

    def read(self, size, timeout=None):
        return self.pipe.read(size + 1)[1:]                 # drop the report id


class WinHidTransport:
    """HID bootloader through the Windows HID API (no libusb): 1 report = report-id 0 + 1024 payload bytes."""

    def __init__(self, vid, pid, timeout_ms=15000, verbose=False):
        import winhid
        from upg_proto import ReportTransport
        self.verbose = verbose
        devs = [d for d in winhid.list_devices(vid, pid) if d["output_len"] > 1 and d["input_len"] > 1]
        if not devs:
            raise SystemExit("HID device %04x:%04x not found (is it in bootloader mode?)" % (vid, pid))
        d = devs[0]
        self.payload = d["output_len"] - 1
        print("HID path:", d["path"], " report: in=%d out=%d" % (d["input_len"], d["output_len"]))
        self.pipe = winhid.HidPipe(d["path"], timeout_ms)
        pipe = self.pipe
        self.rt = ReportTransport(PipeEndpoint(pipe), PipeEndpoint(pipe), self.payload, timeout_ms)

    def out(self, data):
        if self.verbose:
            print("  OUT %4d B  %s" % (len(data), bytes(data[:16]).hex(" ")))
        self.rt.out(data)

    def inn(self, n):
        r = self.rt.inn(n)
        if self.verbose:
            print("  IN  %4d B  %s" % (len(r), r[:16].hex(" ")))
        return r


DISCLAIMER = ("USE AT YOUR OWN RISK: no warranty. The authors accept no responsibility or liability for bricked or damaged "
              "devices.")


def confirm(prompt, word):
    print(DISCLAIMER)
    print(prompt)
    ans = input("Type %s to continue: " % word).strip()
    if ans != word:
        raise SystemExit("aborted")


def hexdump(b, width=16):
    for i in range(0, len(b), width):
        c = b[i:i + width]
        print("  %04x  %-*s  %s" % (i, width * 3, " ".join("%02x" % x for x in c),
                                    "".join(chr(x) if 32 <= x < 127 else "." for x in c)))


# ------------------------------------------------------------------ commands
def cmd_hid_list(args):
    import winhid
    timing = [] if args.timing else None
    t0 = time.time()
    devs = winhid.list_devices(None if args.all else args.vid, None, timing)
    total = time.time() - t0
    if not devs:
        print("No HID device found (VID filter %s)." % ("none" if args.all else "%04x" % args.vid))
    for d in devs:
        print("VID %04x PID %04x iface %s usage_page %04x usage %04x in=%d out=%d  %s / %s" % (
            d["vendor_id"], d["product_id"], d["interface_number"], d["usage_page"], d["usage"],
            d["input_len"], d["output_len"], d["manufacturer"], d["product"]))
        print("    path:", d["path"])
    if timing is not None:
        print("listing took %.2f s" % total)
        for path, sec, what in sorted(timing, key=lambda t: -t[1])[:8]:
            print("  %6.2f s  %-20s %s" % (sec, what, path[:70]))


def cmd_enter_bootloader(args):
    import winhid
    payload = bytes.fromhex(args.packet_hex.replace(" ", "")) if args.packet_hex else APPNEW_PACKET
    devs = [d for d in winhid.list_devices(args.vid, args.pid) if d["output_len"] > 1]
    if not devs:
        raise SystemExit("no HID interface with an output report found for %04x:%04x" % (args.vid, args.pid))
    # prefer the vendor-defined interface (usage page 0xFFA0 on the OEM firmware), else interface 0
    devs.sort(key=lambda d: (d["usage_page"] != 0xFFA0, d["interface_number"] or 0))
    dev = devs[0]
    size = dev["output_len"] - 1 if not args.report_size else args.report_size
    if len(payload) > size:
        raise SystemExit("packet longer than the output report (%d bytes)" % size)
    report = b"\x00" + payload.ljust(size, b"\x00")      # leading 0 = report id (none used)
    print("Interface %s usage_page %04x, output report %d bytes" % (
        dev["interface_number"], dev["usage_page"], dev["output_len"]))
    hexdump(report[:32])
    print("This makes the device reboot into the USB bootloader. The vendor updater does the same.")
    confirm("The screen/keys will stop working until you power-cycle or flash.", "ENTER")
    print("written bytes:", winhid.write(dev["path"], report))


def cmd_usb_info(args):
    up = Upg(UsbTransport(), print)
    status, data = up.hwinfo()
    print("HWINFO status", status)
    hexdump(data)
    if len(data) >= 41:
        print("magic=%r init_mode=%d curr_mode=%d boot_stage=%d" % (data[:8], data[38], data[39], data[40]))


def parse_updater_items(path):
    d = open(path, "rb").read()
    items = []
    for off in range(0x800, 0x2000, 0x200):
        e = d[off:off + 0x200]
        if e[:4] != b"META":
            break
        name = e[8:0x48].split(b"\0")[0].decode()
        o, s, crc, ram = struct.unpack("<IIII", e[0x88:0x98])
        attr = e[0x98:0xD8].split(b"\0")[0].decode()
        if name.startswith("image.updater"):
            items.append((name, ram, attr, d[o:o + s]))
    return items


def cmd_load_updater(args):
    items = parse_updater_items(args.img)
    if not items:
        raise SystemExit("no image.updater.* items in %s" % args.img)
    print("Items to load into RAM (nothing is written to flash):")
    for name, ram, attr, data in items:
        print("  %-24s ram=0x%08x size=0x%x attr=%s" % (name, ram, len(data), attr))
    for name, ram, attr, data in items:
        i = data.find(struct.pack("<I", 0x41490005), 0, 0x7100)      # resource section 5 = partition table
        if i >= 0:
            ln = struct.unpack("<I", data[i + 4:i + 8])[0]
            print("  %s partition table: %s" % (name, data[i + 8:i + 8 + ln].split(b"\0")[0].decode(errors="replace")))
    print("The stock layout is spl 512k, env 128k, env_r 128k, os 2m, rodata 1m, data 10m. If the updater lists another "
          "one, flash only the partitions that sit at the same offsets (the flash command checks that).")
    confirm("The updater must come from an image for the SAME SoC/PSRAM as your device.", "LOAD")
    if args.only_psram:
        items = items[:1]
    mk = lambda t=None: Upg(UsbTransport(timeout_ms=(t or args.timeout) * 1000, verbose=args.verbose), print)
    up = mk()
    for name, ram, attr, data in items:
        last = name == items[-1][0]
        print("write", name)
        up.ram_write(ram, data)
        if "run" in attr:
            addr = ram + args.exec_offset
            print("exec 0x%08x" % addr)
            try:
                print("exec status", up.ram_exec(addr))
            except Exception as exc:  # the device usually re-enumerates here
                print("exec: %s" % exc)
            if last and not args.only_psram:
                time.sleep(args.settle)
                continue
            # the BROM must answer again before the next item can be written: poll HWINFO for a while
            answer = None
            for attempt in range(args.wait):
                time.sleep(1)
                try:
                    answer = mk(2).hwinfo()
                    break
                except (Exception, SystemExit) as exc:
                    if attempt % 3 == 0:
                        print("  %2d s after exec: no answer (%s)" % (attempt + 1, str(exc).splitlines()[0]))
            if answer is None:
                raise SystemExit("the device does not answer %d s after exec of %s at 0x%08x. Unplug USB, repeat the "
                                 "PA.0 short and try another --exec-offset (0x108, 0x100)." % (args.wait, name, addr))
            status, info = answer
            print("  device answers again: HWINFO status %d, boot_stage=%s" % (status, info[40] if len(info) > 40 else "?"))
            up = mk()
    print("Done. Run 'usb-info' to see whether the updater is running (boot_stage changes).")


HID_VID, HID_PID = 0x33C3, 0x8899


def _hid_upg(args, cls=Upg):
    if args.transport == "winhid":
        return cls(WinHidTransport(args.vid, args.pid, args.timeout * 1000, args.verbose), print)
    if args.transport == "bulk":
        # the updater loaded through the BROM (load-updater) speaks the same protocol over the bulk pipes of 33C3:6677
        pid = 0x6677 if args.pid == HID_PID else args.pid
        return cls(UsbTransport(args.vid, pid, args.timeout * 1000, 0, args.verbose, args.libusb), print)
    return cls(UsbTransport(args.vid, args.pid, args.timeout * 1000, args.report_size, args.verbose,
                            args.libusb), print)


def cmd_hid_hwinfo(args):
    up = _hid_upg(args)
    status, data = up.hwinfo()
    print("HWINFO status", status, "len", len(data))
    hexdump(data)
    if len(data) >= 41:
        stage = data[40]
        print("magic=%r init_mode=%d curr_mode=%d boot_stage=%d  (1 = bootloader/updater)" % (data[:8], data[38], data[39], stage))


def cmd_hid_dump(args):
    up = _hid_upg(args)
    status, hw = up.hwinfo()
    stage = hw[40] if len(hw) > 40 else -1
    print("boot_stage =", stage)
    if stage != 1 and not args.force:
        raise SystemExit("device is not in the bootloader/updater stage (boot_stage != 1). "
                         "Use --force only if you know what you are doing.")
    table = up.partition_table()
    print("partition table:", table)
    parts = parse_mtd(table)
    if not parts:
        raise SystemExit("could not parse the partition table")
    os.makedirs(args.out, exist_ok=True)
    open(os.path.join(args.out, "partition_table.txt"), "w").write(table + chr(10))
    want = set(args.partitions.split(",")) if args.partitions else None
    todo = [p for p in parts if not want or p[0] in want]
    if args.list_only:
        return
    up.set_upg_cfg()
    for name, start, size in todo:
        print("reading %-8s start=0x%x size=0x%x" % (name, start, size))
        data = up.read_partition(name, start, size,
                                 lambda a, b: print(chr(13) + "  %d/%d KiB" % (a // 1024, b // 1024), end=""),
                                 layout=args.arg_layout)
        print()
        open(os.path.join(args.out, name + ".bin"), "wb").write(data)
        print("  crc32 %08x  size %d" % (zlib.crc32(data) & 0xFFFFFFFF, len(data)))
    print("Done. Files are in", args.out)


# --------------------------------------------------------------------------------------------- flash
# Only these partitions can be written. "spl" (the bootloader) is deliberately not on the list: without it the
# device cannot be recovered by this tool.
FLASH_COMPONENTS = (
    # option name,  image.target.<name>, partitions it goes to, attribute string
    ("os",     "os",     ("os",),            b"mtd;required"),
    ("rodata", "rodata", ("rodata",),        b"mtd;optional"),
    ("data",   "data",   ("data",),          b"mtd;optional"),
    ("env",    "env",    ("env", "env_r"),   b"mtd;optional"),
)


def flash_plan(args, table_parts):
    """Validate the requested files against the device's partition table; returns a list of components."""
    sizes = {name: size for name, _, size in table_parts}
    plan = []
    for opt, target, parts, attr in FLASH_COMPONENTS:
        path = getattr(args, opt)
        if not path:
            continue
        with open(path, "rb") as f:
            data = f.read()
        for part in parts:
            if part not in sizes:
                raise SystemExit("partition '%s' is not in the device's partition table" % part)
            if len(data) > sizes[part]:
                raise SystemExit("%s is %d bytes, partition '%s' only has %d" % (path, len(data), part, sizes[part]))
        if opt == "os":
            if data[:4] != b"\xd0\x0d\xfe\xed":
                raise SystemExit("%s is not a FIT image (d00dfeed magic missing)" % path)
        plan.append(dict(opt=opt, name="image.target." + target, parts=parts, attr=attr, data=data, path=path,
                         crc=zlib.crc32(data) & 0xFFFFFFFF))
    if not plan:
        raise SystemExit("nothing to flash: give at least --os")
    return plan


# Layout of the stock device. The partition table the bootloader reports comes from the updater that is running
# (stock SPL or the vendor updater loaded by load-updater); an updater with another table would make a write by
# partition name land on the wrong flash offsets, so every partition that is written is checked against this.
EXPECTED_LAYOUT = {"spl": (0, 0x80000), "env": (0x80000, 0x20000), "env_r": (0xA0000, 0x20000),
                   "os": (0xC0000, 0x200000), "rodata": (0x2C0000, 0x100000), "data": (0x3C0000, 0xA00000)}


def check_layout(parts, plan):
    have = {name: (st, sz) for name, st, sz in parts}
    for c in plan:
        for name in c["parts"]:
            want = EXPECTED_LAYOUT.get(name)
            if want is None or have.get(name) != want:
                got = have.get(name)
                raise SystemExit(
                    "refusing to write %s: the device's partition table has it at %s (start, size) but the stock layout "
                    "is %s. The running updater uses another layout, so the write would land on the wrong flash "
                    "offsets. Only partitions that match are safe to write." % (
                        name, tuple(hex(v) for v in got) if got else "nothing",
                        tuple(hex(v) for v in want) if want else "unknown"))


def cmd_flash(args):
    up = _hid_upg(args, UpgWriter)
    status, hw = up.hwinfo()
    stage = hw[40] if len(hw) > 40 else -1
    if stage != 1:
        raise SystemExit("device is not in the bootloader (boot_stage %d). Run enter-bootloader first." % stage)
    table = up.partition_table()
    parts = parse_mtd(table)
    plan = flash_plan(args, parts)
    check_layout(parts, plan)

    print("partition table:", table)
    print("flash plan (spl is never touched):")
    for c in plan:
        print("  %-8s %-14s %9d bytes  crc32 %08x  -> %s" % (c["opt"], c["name"], len(c["data"]), c["crc"],
                                                          ",".join(c["parts"])))
    if not args.write:
        print("\nDry run only. Nothing was written. Add --write to flash.")
        return
    confirm("This ERASES and rewrites the partitions above on the connected device.\n"
            "Keep a dump of the original firmware (hid-dump) before you continue.", "FLASH")

    up.set_upg_cfg(UPG_MODE_PARTITION_UPGRADE)
    up.send_image_info()                                # selects the SPI NOR media for the components that follow
    for c in plan:
        print("writing %s ..." % c["name"])
        up.set_fwc_meta(fwc_meta(c["name"], ";".join(c["parts"]), len(c["data"]), c["crc"], c["attr"]))
        print("  block size", up.get_block_size())
        up.send_fwc_data(c["data"], lambda a, b: print(chr(13) + "  %d/%d KiB" % (a // 1024, b // 1024), end=""))
        print()
        dev_crc = up.get_fwc_crc()
        result = up.get_burn_result()
        print("  device crc %08x (image %08x), burn result %d" % (dev_crc, c["crc"], result))
        if result != 0:
            raise SystemExit("the device reported a write error for %s - do NOT reboot, re-run the flash" % c["name"])
    up.set_upg_end()

    if args.no_verify:
        print("Written. Verification skipped.")
        return
    print("verifying by reading the partitions back ...")
    up.set_upg_cfg()                                    # dump mode
    start = {name: st for name, st, _ in parts}
    for c in plan:
        part = c["parts"][0]
        back = up.read_partition(part, start[part], len(c["data"]), layout=args.arg_layout)
        ok = back == c["data"]
        print("  %-8s %s" % (c["opt"], "OK" if ok else "MISMATCH"))
        if not ok:
            raise SystemExit("verification failed for %s - re-run the flash before rebooting" % c["opt"])
    print("Flash complete and verified. Power-cycle the device.")


# ---------------------------------------------------------------------------------------------- logo
LOGO_MAX_PAYLOAD = 0x80000 - 1024       # the firmware stages an upload in 512 KiB, the first 1024 bytes are the header


def jpeg_info(data):
    """Returns (width, height, progressive) of a JPEG, raises ValueError if it is not a usable one."""
    if data[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG (no FFD8 at the start)")
    if data[-2:] != b"\xff\xd9":
        raise ValueError("the JPEG does not end with FFD9 (the firmware detects the end of an upload by it)")
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xff:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xd8, 0x01) or 0xd0 <= marker <= 0xd7 or marker == 0xff:
            i += 1 if marker == 0xff else 2
            continue
        seglen = struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xc0, 0xc1, 0xc2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h, marker == 0xc2
        i += 2 + seglen
    raise ValueError("no frame header found in the JPEG")


def logo_packets(data):
    """Host -> device reports (1024 byte payloads) of the LOG command: a header packet
    'CRT' 00 00 'LOG' + big-endian length at bytes 8..11, then the file in 1024 byte chunks."""
    head = bytearray(1024)
    head[0:5] = b"CRT\x00\x00"
    head[5:8] = b"LOG"
    head[8:12] = struct.pack(">I", len(data))
    packets = [bytes(head)]
    for off in range(0, len(data), 1024):
        packets.append(data[off:off + 1024].ljust(1024, b"\x00"))
    return packets


def cmd_logo(args):
    import winhid
    with open(args.file, "rb") as f:
        data = f.read()
    try:
        w, h, progressive = jpeg_info(data)
    except ValueError as exc:
        raise SystemExit("%s: %s" % (args.file, exc))
    if progressive:
        raise SystemExit("progressive JPEG: save it as baseline (the hardware decoder needs a baseline file)")
    if len(data) > LOGO_MAX_PAYLOAD:
        raise SystemExit("the file is %d bytes, the maximum is %d" % (len(data), LOGO_MAX_PAYLOAD))
    if (w, h) != (800, 480):
        print("warning: the screen is 800x480, this image is %dx%d" % (w, h))
    devs = [d for d in winhid.list_devices(args.vid, args.pid) if d["output_len"] > 1]
    if not devs:
        raise SystemExit("no HID interface found for %04x:%04x - is the firmware running (not the bootloader)?"
                         % (args.vid, args.pid))
    devs.sort(key=lambda d: (d["usage_page"] != 0xFFA0, d["interface_number"] or 0))
    path = devs[0]["path"]
    packets = logo_packets(data)
    print("sending %d bytes in %d reports ..." % (len(data), len(packets)))
    for i, pkt in enumerate(packets):
        winhid.write(path, b"\x00" + pkt)               # leading 0 = report id
        time.sleep(0.002)
    print("Done. The device stores the file and redraws the logo; it answers with an ACK frame.")


# ---------------------------------------------------------------------------------------------- hid-talk
def talk_packet(spec):
    """'DIS', 'HAN', 'STP', 'CON', 'LIG:30', 'CLE:C' or raw hex (prefix 'hex:') -> 1024 byte host packet."""
    if spec.startswith("hex:"):
        return bytes.fromhex(spec[4:].replace(" ", ""))
    name, _, arg = spec.partition(":")
    pkt = bytearray(b"CRT" + bytes(2) + name.encode())
    pkt.extend(bytes(16 - len(pkt)))
    if name == "LIG":
        pkt[10] = int(arg or "30", 0)
    elif name == "CLE":
        pkt[11] = ord(arg or "C")
    return bytes(pkt)


# touch swipe frames (firmware/app/touch.c): code, and the direction byte 11 that this firmware adds
SWIPE_CODES = (0x39, 0xb1, 0xb2)
SWIPE_DIRECTIONS = {1: "left", 2: "right", 3: "up", 4: "down"}


def describe_input(data):
    """data = one input report including the report id byte. ACK frames: byte 9 = code, byte 10 = state."""
    body = data[1:]
    if body[:3] == b"ACK":
        extra = ""
        if body[9] in SWIPE_CODES and body[10] == 1 and body[11] in SWIPE_DIRECTIONS:
            dx = int.from_bytes(body[12:14], "big", signed=True)
            dy = int.from_bytes(body[14:16], "big", signed=True)
            extra = "  swipe %s dx=%d dy=%d" % (SWIPE_DIRECTIONS[body[11]], dx, dy)
        elif any(body[11:15]):
            extra = "  x=%d y=%d" % ((body[11] << 8) | body[12], (body[13] << 8) | body[14])
        return "ACK code 0x%02x state %d%s" % (body[9], body[10], extra)
    return "report " + body[:16].hex(" ")


def cmd_hid_talk(args):
    import winhid
    devs = [d for d in winhid.list_devices(args.vid, args.pid) if d["output_len"] > 1]
    if not devs:
        raise SystemExit("no HID interface with an output report found for %04x:%04x" % (args.vid, args.pid))
    devs.sort(key=lambda d: (d["usage_page"] != 0xFFA0, d["interface_number"] or 0))
    dev = devs[0]
    size = dev["output_len"] - 1
    print("interface %s usage_page %04x, output %d / input %d bytes" % (dev["interface_number"], dev["usage_page"],
                                                                       dev["output_len"], dev["input_len"]))
    pipe = winhid.HidPipe(dev["path"], timeout_ms=200)
    try:
        for spec in args.cmd or []:
            pkt = talk_packet(spec)
            pipe.write(bytes([0]) + pkt.ljust(size, bytes(1)))
            print("sent", spec)
            deadline = time.time() + args.wait
            while time.time() < deadline:
                try:
                    print("  <-", describe_input(pipe.read(dev["input_len"])))
                except TimeoutError:
                    pass
        print("listening for %.0f s (press keys, turn / press the knobs, touch the display) ..." % args.listen)
        deadline = time.time() + args.listen
        while time.time() < deadline:
            try:
                print(time.strftime("%H:%M:%S"), describe_input(pipe.read(dev["input_len"])))
            except TimeoutError:
                pass
    finally:
        pipe.close()



def cmd_selftest(args):
    """Send CRT SELFT to toggle selftest mode on the running firmware."""
    import winhid
    devs = [d for d in winhid.list_devices(args.vid, args.pid) if d["output_len"] > 1]
    if not devs:
        raise SystemExit("no HID interface found for %04x:%04x - is the firmware running?" % (args.vid, args.pid))
    devs.sort(key=lambda d: (d["usage_page"] != 0xFFA0, d["interface_number"] or 0))
    dev = devs[0]
    size = dev["output_len"] - 1
    report = b"\x00" + SELFTEST_PACKET.ljust(size, b"\x00")
    print("sending SELFT to interface %s (usage_page %04x) ..." % (dev["interface_number"], dev["usage_page"]))
    winhid.write(dev["path"], report)
    print("done - selftest mode toggled on the device")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("hid-list"); a.add_argument("--vid", type=lambda x: int(x, 0), default=STREAMDOCK_VID)
    a.add_argument("--all", action="store_true", help="list HID devices of every vendor")
    a.add_argument("--timing", action="store_true", help="print how long the listing took and the slowest steps")
    a.set_defaults(fn=cmd_hid_list)
    a = sub.add_parser("enter-bootloader")
    a.add_argument("--vid", type=lambda x: int(x, 0), default=STREAMDOCK_VID)
    a.add_argument("--pid", type=lambda x: int(x, 0), default=0x3004)
    a.add_argument("--packet-hex", help="hex bytes of the host packet (default: CRT APPNEW)"); a.add_argument("--report-size", type=int, default=0,
                   help="payload size; default = what the device reports")
    a.set_defaults(fn=cmd_enter_bootloader)
    a = sub.add_parser("usb-info"); a.set_defaults(fn=cmd_usb_info)
    a = sub.add_parser("load-updater")
    a.add_argument("--img", required=True)
    a.add_argument("--exec-offset", type=lambda x: int(x, 0), default=0,
                   help="exec offset inside the loaded .aic; 0 worked on the AKP05E (0x108 and 0x100 are the code entry, they hung the BROM)")
    a.add_argument("--settle", type=float, default=3.0)
    a.add_argument("--timeout", type=int, default=10, help="USB timeout in seconds")
    a.add_argument("--wait", type=int, default=20, help="seconds to wait for the BROM after the psram stage")
    a.add_argument("--only-psram", action="store_true", help="run only the psram init stage and check the device (diagnostics)")
    a.add_argument("-v", "--verbose", action="store_true", help="print every USB transfer")
    a.set_defaults(fn=cmd_load_updater)
    for nm, fn in (("hid-hwinfo", cmd_hid_hwinfo), ("hid-dump", cmd_hid_dump)):
        a = sub.add_parser(nm)
        a.add_argument("--vid", type=lambda x: int(x, 0), default=HID_VID)
        a.add_argument("--pid", type=lambda x: int(x, 0), default=HID_PID)
        a.add_argument("--timeout", type=int, default=15, help="USB timeout in seconds")
        a.add_argument("-v", "--verbose", action="store_true", help="print every USB transfer")
        a.add_argument("--transport", choices=("winhid", "libusb", "bulk"), default="winhid",
                       help="winhid = Windows HID API (default), libusb = HID reports through libusb, "
                            "bulk = bulk pipes of the BROM-loaded updater (33C3:6677)")
        a.add_argument("--libusb", help="path to libusb-1.0.dll (e.g. the vendor's 1.0.27 x64) for --transport libusb")
        a.add_argument("--report-size", type=int, default=1024,
                       help="HID report payload size (0 = send/read exact lengths, no padding)")
        if nm == "hid-dump":
            a.add_argument("--out", required=True)
            a.add_argument("--partitions", help="comma separated subset, e.g. env,os")
            a.add_argument("--list-only", action="store_true", help="only read the partition table")
            a.add_argument("--force", action="store_true", help="continue even if boot_stage != 1")
            a.add_argument("--arg-layout", choices=("rv144", "rv32"), default="rv144",
                           help="READ_FWC_DATA argument: rv144 = 304 bytes, what the AKP05E expects (default), rv32 = 192 bytes")
        a.set_defaults(fn=fn)
    a = sub.add_parser("flash", help="write os/rodata/data/env to the device (dry run by default, never writes spl)")
    a.add_argument("--vid", type=lambda x: int(x, 0), default=HID_VID)
    a.add_argument("--pid", type=lambda x: int(x, 0), default=HID_PID)
    a.add_argument("--timeout", type=int, default=15, help="USB timeout in seconds")
    a.add_argument("-v", "--verbose", action="store_true", help="print every USB transfer")
    a.add_argument("--transport", choices=("winhid", "libusb", "bulk"), default="winhid",
                   help="bulk = the updater loaded through the BROM (33C3:6677), winhid = HID bootloader (33C3:8899)")
    a.add_argument("--libusb", help="path to libusb-1.0.dll for --transport libusb")
    a.add_argument("--report-size", type=int, default=1024)
    a.add_argument("--arg-layout", choices=("rv144", "rv32"), default="rv144")
    a.add_argument("--os", help="d13x_os.itb from the build")
    a.add_argument("--rodata", help="rodata.fatfs")
    a.add_argument("--data", help="data.lfs")
    a.add_argument("--env", help="env.bin (written to env and env_r)")
    a.add_argument("--write", action="store_true", help="really write (without it only a dry run is done)")
    a.add_argument("--no-verify", action="store_true", help="skip the read-back verification")
    a.set_defaults(fn=cmd_flash)
    a = sub.add_parser("logo", help="upload a new boot logo (baseline JPEG, 800x480) to the RUNNING firmware")
    a.add_argument("file", help="JPEG file")
    a.add_argument("--vid", type=lambda x: int(x, 0), default=0x0300)
    a.add_argument("--pid", type=lambda x: int(x, 0), default=0x3004)
    a.set_defaults(fn=cmd_logo)
    a = sub.add_parser("hid-talk", help="talk to the RUNNING firmware over HID: send commands, print what comes back")
    a.add_argument("--vid", type=lambda x: int(x, 0), default=0x0300)
    a.add_argument("--pid", type=lambda x: int(x, 0), default=0x3004)
    a.add_argument("--cmd", action="append", help="DIS, HAN, STP, CON, LIG:30, CLE:C, SELFT or hex:<bytes> (repeatable)")
    a.add_argument("--wait", type=float, default=1.0, help="seconds to listen after each command")
    a.add_argument("--listen", type=float, default=10.0, help="seconds to listen at the end for key/knob/touch events")
    a.set_defaults(fn=cmd_hid_talk)
    a = sub.add_parser("selftest", help="toggle the hardware selftest mode on the RUNNING firmware")
    a.add_argument("--vid", type=lambda x: int(x, 0), default=0x0300)
    a.add_argument("--pid", type=lambda x: int(x, 0), default=0x3004)
    a.set_defaults(fn=cmd_selftest)
    args = p.parse_args()
    try:
        args.fn(args)
    except ProtoError as e:
        sys.exit("protocol error: %s" % e)
    except Exception as e:
        if type(e).__name__ == "USBError":
            msg = str(e)
            if "10060" in msg or "timed out" in msg.lower():
                sys.exit("USB timeout: %s" % msg + chr(10) + "The device did not accept or answer a transfer. Unplug USB for 10 s, "
                         "enter the bootloader again and retry; close every other program that uses the device." + chr(10) +
                         "Add -v to see every transfer.")
            sys.exit("USB error: %s  (try --report-size 0; add -v to see every transfer)" % msg)
        raise


if __name__ == "__main__":
    main()
