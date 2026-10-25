# The stock firmware

What is known about the original firmware of the AKP05E, how it was analysed, and where every rebuilt function
comes from. None of the manufacturer's binaries are part of this repository: you need **your own dump** (section 5).

## 1. Identification

| Item | Value |
|---|---|
| Update image | `d13x_AKP05_v1.2.7.img` (vendor Windows update tool, platform `d13x`, product `HSV293V3-N4`) |
| SPL | `tinySPL`, built 2025-08-05 11:12:04, "Pre-Boot Program 24-08-19 16:45 2fba910" |
| OS | RT-Thread / Luban-Lite, banner `Welcome to 1.0.6`, image version `1.0.0`, built 2025-09-02; application compiled 2025-09-03 |
| FIT | two segments: `seg0` 666616 bytes at `0x40210000` (code + data), `seg1` 10472 bytes at `0x402B2C00`; crc32 + md5 hashes, no signature |
| Application region | `0x40241740 - 0x40243ef2` (48 functions) plus the USB helpers at `0x40239xxx` |
| Console | 115200 8N1 on UART0, RT-Thread `msh` with the prompt `arx /> ` |

The stock firmware was probably built from a slightly different revision of the SDK (the line numbers in its log
messages differ from v1.0.3, e.g. `aic_find_panel()97` versus `59`), but the sources of v1.0.3 matched every piece that
was needed.

## 2. Function map

Each reconstructed function is annotated in the source with `stock 0x........`. The table collects them.
`akp05_app.h` has all the prototypes; the comparison in section 4 uses the same addresses.

| Area | Function (ours) | Stock address | File |
|---|---|---|---|
| entry | `main()` | 0x40241a2a | main.c |
| resources | `app_resources_init()` | 0x40243a18 | main.c |
| threads | `threads_start()` | 0x402418e6 | threads.c |
| threads | `usb_recv_thread_entry` / `usb_img_thread_entry` / `logo_thread_entry` | 0x40242808 / 0x40242398 / 0x40243d4c | threads.c |
| protocol | `protocol_parse()` | 0x40241ce0 | protocol.c |
| protocol | `dispatch_command()` | 0x40242494 | threads.c |
| protocol | ACK frame `ack_init()` | prologue of 0x40242ea2 | protocol.c |
| buffers | `rx_ring_init / write / read` | 0x402421cc / 0x40242220 / 0x402422e0 | ringbuf.c |
| buffers | `kfifo_init / in / out` | 0x40242052 / 0x402420a0 / 0x40242142 | ringbuf.c |
| storage | `logo_write_file()` | 0x40243bf6 | storage.c |
| USB | `custom_out_cb` / `usbd_event_handler` | 0x4023997e / 0x402399ae | usb_hid.c |
| USB | `hid_report_send` / `usb_serial_from_efuse` / `usb_hid_init` | 0x402399c8 / 0x402399d4 / 0x40239ab2 | usb_hid.c |
| keys | `pins_init` | 0x402429aa | input.c |
| keys | `scan_columns`, matrix key logic, keyboard thread | 0x40242af8, 0x40242ea2, 0x4024321c | input.c |
| encoders | `knob_list_create`, `knob_process`, `knob_buttons_scan`, ec11 thread | 0x40242c54, 0x40242b98, 0x40242cd8, 0x40242e24 | input.c |
| touch | `knob_i2c_init`, register read, event logic | 0x4024324e, 0x402432cc, 0x402433d4 | touch.c |
| display | `display_layout_init`, `display_key_image`, `display_clear`, `display_flush`, `display_logo` | 0x40241b04, 0x40241b86, 0x40241bf4, 0x40241cb8, 0x40243ca6 | display.c |
| display | fill rectangle (GE) | 0x402679d6 | display.c |
| backlight | `backlight_set` / `backlight_off` | 0x40243e00 / 0x40243e6a | backlight.c |
| watchdog | `wdt_start / wdt_stop / wdt_init / wdt_feed_hook` | 0x40243b7c / 0x40243bb4 / 0x4024396e / 0x40243938 | backlight.c |
| LEDs | `leds_apply` | 0x40241740 | leds.c |
| shell | `aicupg` command (reason 6 / 5 / 4) | 0x402489ee | main.c (`reboot_to_upgrade`) |

Data tables copied from the stock image: the USB descriptor blob (`seg1 @ 0x402b38e8`), the key-code table
(`0x402933cc`), the column pattern table (`0x402933b8`), the encoder ids (`0x402933c8`, `0x402b3a90`), the touch tile
table (`0x402b3ab0`). Interesting global addresses: backlight level `0x402b3a61`, backlight on `0x402b3a69`, host state
`0x402b3a6c`, parser state `0x402b3a70`, LED colours `0x402b8e60`.

Facts about the stock code that surprised during the reconstruction (all reproduced):

* the **image thread handles only `LOG` (10) and `BAT` (9)**; `BGPIC` is accepted by the receive thread and dropped;
* the QSPI device used for the LED strip is **never assigned**, the LED output is dead code;
* the key-to-tile table maps host key 5 to index -1 (a latent bug, such a key is skipped here);
* a dead host reboots the dock: the watchdog is fed only when the host spoke since the last 15 s tick.

## 3. How the analysis was done

1. Dump all partitions through the HID bootloader (`hid-dump`).
2. Unpack the FIT: `python scripts/analysis/fit_dump.py os.bin --extract seg0 seg0.bin` (and `seg1`).
3. Disassemble with the SDK toolchain: `riscv64-unknown-elf-objdump -D -b binary -m riscv:rv32 -M no-aliases
   --adjust-vma=0x40210000 seg0.bin`. This decodes the T-Head `xtheade` instructions correctly.
4. Decompile with **Ghidra** (headless, language `RISCV:LE:32:AndeStar_v5:gcc`, set `gp = 0x402b54e8`). Ghidra does not
   know `xtheade` (e.g. `lrbu`, `add64`, `extu`), so the decompiler output is only a map: it truncates functions at
   "bad instructions". Every rebuilt function was checked against the objdump listing.
5. Identify SDK functions by matching strings and call patterns against the sources of Luban-Lite v1.0.3
   (`mpp_*`, `aicfb_*`, `rt_*`, CherryUSB), so that only the real application code had to be rebuilt.

The exports from this work are kept in `private/analysis/` (not in git): `os_decompiled.c`, `functions.tsv`,
`strings.tsv`, `seg0.bin`, `seg1.bin`, `app_region.dis`, `spl_boot0.dis` (disassembly of the SPL's first stage; the
strap logic in boot-and-recovery.md comes from there).

## 4. Fidelity of the reconstruction

`python scripts/analysis/compare_with_stock.py` disassembles each rebuilt function and its stock counterpart and
compares the mnemonic sequences. Result for the 22 compared functions (build of 2026-10-25): **4384 of 4724 bytes
(93 %), mean sequence similarity 67 %**; most functions are within +-25 % of the original size. The weakest are
`dispatch_command` (80 % of the size, 35 % similarity: different branch layout, same behaviour) and `usb_serial_from_efuse`.
This is a regression guard, not a proof of equivalence; the behaviour was verified on the device.

## 5. Your own dump

```powershell
akp05_tool.exe enter-bootloader          # or upghid on the UART
akp05_tool.exe hid-dump --out private\stock-dump
```

The kit scripts, `compare_with_stock.py` and the restore steps expect `os.bin`, `rodata.bin`, `data.bin`, `env.bin`,
`env_r.bin`, `spl.bin` and `partition_table.txt` in `private\stock-dump\`. It is your only copy of the original firmware, in particular
of the `spl` partition that this project never writes: back it up outside the repository.
