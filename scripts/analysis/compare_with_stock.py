#!/usr/bin/env python3
"""Compare the rebuilt application functions with the stock ones at instruction level.

For every (our symbol, stock address, stock size) pair below the mnemonic sequences of both implementations are
compared (compressed "c." prefixes are ignored), which shows how close the reconstruction is to the original code.

  python scripts/analysis/compare_with_stock.py [--elf build/images/d13x.elf] [--stock-os private/stock-dump/os.bin]

The stock "os" partition is your own dump of the device (see docs/stock-firmware.md); it is not part of the repository.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fit_dump                                                    # noqa: E402  (FIT parser next to this script)

TC = os.path.join(ROOT, "sdk", "toolchain", "bin")
OBJDUMP = os.path.join(TC, "riscv64-unknown-elf-objdump.exe")
NM = os.path.join(TC, "riscv64-unknown-elf-nm.exe")
STOCK_LOAD_ADDRESS = 0x40210000                                    # load address of stock segment 0

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--elf", default=os.path.join(ROOT, "build", "images", "d13x.elf"))
ap.add_argument("--stock-os", default=os.path.join(ROOT, "private", "stock-dump", "os.bin"))
ARGS = ap.parse_args()
ELF = ARGS.elf
SEG0 = os.path.join(tempfile.gettempdir(), "akp05_stock_seg0.bin")
fit_dump.extract(ARGS.stock_os, "seg0", SEG0)

# (our symbol, stock address, stock size in bytes)
# Left out on purpose: functions that deliberately differ from stock (knob_buttons_scan, touch_poll, wdt_feed_hook with its
# log line) and static functions the compiler inlined.
PAIRS = [
    ("protocol_parse",            0x40241ce0, 870),
    ("dispatch_command",          0x40242494, 884),
    ("usb_recv_thread_entry",     0x40242808, 0x402429aa - 0x40242808),
    ("usb_img_thread_entry",      0x40242398, 252),
    ("leds_apply",                0x40241740, 420),
    ("backlight_set",             0x40243e00, 106),
    ("backlight_off",             0x40243e6a, 52),
    ("rx_ring_write",             0x40242220, 192),
    ("rx_ring_read",              0x402422e0, 182),
    ("rx_ring_init",              0x402421cc, 84),
    ("kfifo_init",                0x40242052, 78),
    ("kfifo_in",                  0x402420a0, 162),
    ("kfifo_out",                 0x40242142, 138),
    ("logo_write_file",           0x40243bf6, 176),
    ("display_layout_init",       0x40241b04, 128),
    ("display_flush",             0x40241cb8, 40),
    ("scan_columns",              0x40242af8, 160),
    ("knob_process",              0x40242b98, 188),
    ("wdt_start",                 0x40243b7c, 56),
    ("wdt_stop",                  0x40243bb4, 56),
    ("usb_serial_from_efuse",     0x402399d4, 222),
    ("custom_out_cb",             0x4023997e, 0x402399ae - 0x4023997e),
]


def mnemonics(lines):
    out = []
    for l in lines:
        m = re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4,8}\s+)+([a-z][a-z0-9._]*)", l)
        if m:
            mn = m.group(1)
            out.append(mn[2:] if mn.startswith("c.") else mn)
    return out


def our_function(name):
    nm = subprocess.run([NM, "-n", "-S", ELF], capture_output=True, text=True).stdout
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 4 and p[3] == name and p[2].lower() == "t":
            addr, size = int(p[0], 16), int(p[1], 16)
            d = subprocess.run([OBJDUMP, "-d", "-M", "no-aliases", "--start-address=%#x" % addr,
                                "--stop-address=%#x" % (addr + size), ELF], capture_output=True, text=True).stdout
            return size, mnemonics(d.splitlines())
    return None, None


def stock_function(addr, size):
    d = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "riscv:rv32", "-M", "no-aliases",
                        "--adjust-vma=%#x" % STOCK_LOAD_ADDRESS, "--start-address=%#x" % addr, "--stop-address=%#x" % (addr + size),
                        SEG0], capture_output=True, text=True).stdout
    return mnemonics(d.splitlines())


print("%-26s %6s %6s %5s %8s %8s" % ("function", "stock", "ours", "size", "mnem sim", "multiset"))
tot_s = tot_o = 0
sims = []
for name, addr, ssz in PAIRS:
    osz, om = our_function(name)
    sm = stock_function(addr, ssz)
    if om is None:
        print("%-26s %6d %6s  (not found)" % (name, ssz, "-"))
        continue
    seq = difflib.SequenceMatcher(None, sm, om, autojunk=False).ratio()
    cs, co = Counter(sm), Counter(om)
    inter = sum((cs & co).values())
    multi = 2.0 * inter / (len(sm) + len(om))
    sims.append(seq)
    print("%-26s %6d %6d %4.0f%% %7.0f%% %7.0f%%" % (name, ssz, osz, 100.0 * osz / ssz, 100 * seq, 100 * multi))
    tot_s += ssz; tot_o += osz
print("total bytes stock %d ours %d (%.0f%%), mean sequence similarity %.0f%%" % (tot_s, tot_o, 100.0 * tot_o / tot_s, 100 * sum(sims) / len(sims)))
