#!/usr/bin/env python3
"""Pack the linked firmware into the SDK's flash image (the part SCons does after linking).

Mirrors tools/scripts/aic_build.py (mkimage_prebuild):
  copy firmware/image/* + *.pbp  ->  makefatfs (rodata)  ->  makelittlefs (data)
  ->  elf_parse (seg*.bin + d13x_os.its)  ->  add the hashes and the `version` property the stock bootloader needs
  ->  mk_image (env.bin, d13x_os.itb; a full flash .img is only possible with a vendor updater and is not built)

Run with the SDK's bundled Python 3.9 (the CMake target `image` does that).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys


def run(cmd, cwd, env, check=True):
    print("  $ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, env=env)
    if check and p.returncode != 0:
        sys.exit("FAILED (%d): %s" % (p.returncode, cmd[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--pack", required=True, help="directory with image_cfg.json and env.txt (firmware/image)")
    ap.add_argument("--fs", required=True, help="directory with the rodata/ and data/ source trees (firmware/rootfs)")
    ap.add_argument("--elf", required=True)
    ap.add_argument("--out", required=True, help="images dir")
    ap.add_argument("--prefix", default="riscv64-unknown-elf-")
    a = ap.parse_args()

    sdk = os.path.abspath(a.sdk)
    out = os.path.abspath(a.out)
    scripts = os.path.join(sdk, "tools", "scripts") + os.sep
    tools = os.path.join(sdk, "tools", "env", "tools")
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([
        os.path.join(sdk, "toolchain", "bin"), os.path.join(tools, "Python39"), os.path.join(tools, "bin"),
        env.get("PATH", "")])
    env["PRJ_OUT_DIR"] = out + os.sep
    py = [os.path.join(tools, "Python39", "python3.exe")] if os.name == "nt" else ["python3"]
    os.makedirs(out, exist_ok=True)

    # the SDK scripts derive names from the ELF: <name>.elf -> <name>_os.its (image_cfg.json expects d13x_os.its)
    elf = os.path.join(out, "d13x.elf")
    if os.path.abspath(a.elf) != elf:
        shutil.copyfile(a.elf, elf)

    print("[1/5] copy pack files")
    for f in glob.glob(os.path.join(a.pack, "*")):
        shutil.copy(f, out)
    for f in glob.glob(os.path.join(sdk, "bsp", "artinchip", "sys", "d13x", "*.pbp")):
        shutil.copy(f, out)

    print("[2/5] rodata (FAT) and data (littlefs)")
    run(py + [scripts + "makefatfs.py", "--auto", "--cluster", "8", "--sector", "512", "--tooldir", scripts,
              "--inputdir", os.path.join(a.fs, "rodata"), "--outfile", os.path.join(out, "rodata.fatfs")], sdk, env)
    run(py + [scripts + "makelittlefs.py", "--pagesize", "256", "--blocksize", "4096", "--tooldir", scripts,
              "--inputdir", os.path.join(a.fs, "data"), "--outfile", os.path.join(out, "data.lfs")], sdk, env)

    print("[3/5] ELF segments -> seg*.bin + d13x_os.its")
    run(py + [scripts + "elf_parse.py", elf, out + os.sep, a.prefix], out, env)

    # The stock FIT carries crc32 + md5 hashes per segment (the bootloader prints "CRC32 verify OK"); the
    # SDK's elf_parse.py writes none, so add them: mkimage computes the values when the nodes are present.
    its = os.path.join(out, "d13x_os.its")
    import re
    text = open(its, encoding="utf-8").read()
    if "hash-1" not in text:
        hashes = ("\t\t\thash-1 {\n\t\t\t\talgo = \"crc32\";\n\t\t\t};\n"
                  "\t\t\thash-2 {\n\t\t\t\talgo = \"md5\";\n\t\t\t};\n")
        text = re.sub(r"(entry = <[^>]*>;\n)", lambda m: m.group(1) + hashes, text)
    # The stock FIT has `version` in the images node and the stock SPL reads it unchecked: without it the SPL
    # faults right after "Selecting default config" (CPU exception 5 with mtval 0, a strlen(NULL)).
    if not re.search(r"^\s*version\s*=", text, re.M):
        cfg = open(os.path.join(a.pack, "image_cfg.json"), encoding="utf-8").read()
        version = re.search(r'"version"\s*:\s*"([^"]+)"', cfg).group(1)
        text = re.sub(r"(images \{\n)", lambda m: m.group(1) + '\t\tversion = "%s";\n' % version, text, count=1)
    open(its, "w", encoding="utf-8").write(text)

    print("[4/5] mk_image (env.bin, d13x_os.itb, flash image)")
    # mkimage resolves ./seg0.bin from the cwd. mk_image only emits the full flash .img when the vendor updater
    # items are configured; this repository has none, so a missing .img is fine as long as the components exist.
    cmd = [scripts + "mk_image.exe", "-v", "-c", os.path.join(out, "image_cfg.json"), "-d", out + os.sep]
    print("  $ " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=out, env=env, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        if "KeyError: 'updater'" in p.stderr:
            # expected: the components are written before mk_image looks for the (missing) updater section
            print("  note: no flash .img was generated because image_cfg.json has no vendor updater (expected);\n"
                  "        the components below are what akp05_tool.py flashes.")
        else:
            sys.stderr.write(p.stderr)
            sys.exit("mk_image failed (%d)" % p.returncode)
    for comp in ("d13x_os.itb", "env.bin", "rodata.fatfs", "data.lfs"):
        if not os.path.exists(os.path.join(out, comp)):
            sys.exit("missing component: " + comp)

    print("[5/5] result")
    for pat in ("*.itb", "*.img"):
        for f in sorted(glob.glob(os.path.join(out, pat))):
            print("  %-60s %9d bytes" % (f, os.path.getsize(f)))


if __name__ == "__main__":
    main()
