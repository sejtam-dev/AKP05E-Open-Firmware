#!/usr/bin/env python3
"""Dump or unpack an ArtInChip FIT image (the "os" partition / d13x_os.itb).

  python scripts/analysis/fit_dump.py <image>                       print the node tree with all properties
  python scripts/analysis/fit_dump.py <image> --extract seg0 out.bin   write the data of one image node

The FIT is a flattened device tree (big endian). The stock bootloader (tinySPL) reads, per image node, the
properties `data-offset`, `data-size`, `load`, `entry` (and verifies the `hash-*` subnodes) and, from the `images` node,
a `version` string; it does not check the presence of the latter (a missing `version` crashes it, see
docs/boot-and-recovery.md). `data-offset` is relative to the end of the FIT header (0x800 bytes of padding).
"""
import argparse
import struct
import sys

HEADER_PAD = 0x800          # mkimage -B 0x800 (see scripts/pack_image.py)
INT_PROPS = {"load", "entry", "data-size", "data-offset", "timestamp", "#address-cells", "value"}   # cells, not text


def parse(path):
    """Returns (raw bytes, [printable lines], {image node name: {property: raw value}})."""
    d = open(path, "rb").read()
    magic, total, off_struct, off_strings, _rsv, _ver, _last, _cpu, sz_str, sz_struct = struct.unpack(">10I", d[:40])
    if magic != 0xD00DFEED:
        raise SystemExit("%s is not a FIT / device tree image (magic 0x%08x)" % (path, magic))
    st = d[off_struct:off_struct + sz_struct]
    strs = d[off_strings:off_strings + sz_str]
    lines, nodes, path_stack, i = [], {}, [], 0
    while i < len(st):
        tag = struct.unpack(">I", st[i:i + 4])[0]
        i += 4
        if tag == 1:                                   # begin node
            j = st.index(b"\0", i)
            name = st[i:j].decode()
            i = (j + 1 + 3) & ~3
            lines.append("  " * len(path_stack) + "node " + (name or "/"))
            path_stack.append(name)
        elif tag == 2:                                 # end node
            path_stack.pop()
        elif tag == 3:                                 # property
            ln, nameoff = struct.unpack(">II", st[i:i + 8])
            i += 8
            val = st[i:i + ln]
            i = (i + ln + 3) & ~3
            pname = strs[nameoff:strs.index(b"\0", nameoff)].decode()
            if len(path_stack) == 3 and path_stack[1] == "images":
                nodes.setdefault(path_stack[2], {})[pname] = val
            if pname not in INT_PROPS and ln and val.endswith(b"\0") and all(32 <= b < 127 or b == 0 for b in val):
                text = repr(val[:-1].decode().replace("\0", "|"))
            elif ln % 4 == 0 and ln <= 32:
                text = " ".join("0x%x" % x for x in struct.unpack(">%dI" % (ln // 4), val))
            else:
                text = "<%d bytes>" % ln
            lines.append("  " * len(path_stack) + "%s = %s" % (pname, text))
        elif tag == 9:                                 # end
            break
    return d, lines, nodes


def extract(path, node, out):
    d, _lines, nodes = parse(path)
    if node not in nodes:
        raise SystemExit("no image node %r (have: %s)" % (node, ", ".join(sorted(nodes))))
    props = nodes[node]
    size = struct.unpack(">I", props["data-size"])[0]
    off = struct.unpack(">I", props["data-offset"])[0] + HEADER_PAD
    open(out, "wb").write(d[off:off + size])
    print("%s: %d bytes from file offset 0x%x, load address 0x%x" % (
        node, size, off, struct.unpack(">I", props["load"])[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--extract", nargs=2, metavar=("NODE", "OUT"), help="write the data of an image node to a file")
    a = ap.parse_args()
    if a.extract:
        extract(a.image, *a.extract)
    else:
        print("\n".join(parse(a.image)[1]))


if __name__ == "__main__":
    sys.exit(main())
