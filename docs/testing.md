# Testing and verification status

## Automated tests (no hardware)

```powershell
$env:PYTHONPATH = "host-tool"
python -m unittest discover -s host-tool/tests -v
```

24 tests cover the host tool: protocol framing, the dump and flash sequences against a software model of the bootloader
(including the rules of the real device: argument sizes, RAM medium until `image.info`), the partition layout checks,
JPEG validation for `logo`, command and event encoding for `hid-talk`. CI runs them on every push.

The firmware has no unit tests; it is verified on the device (below). `scripts/analysis/compare_with_stock.py` is a
regression guard against the stock code (see [stock-firmware.md](stock-firmware.md)).

## Verified on a real AKP05E

Status as of the end of the bring-up (2026-10-25).

| Area | Result |
|---|---|
| clean build from a fresh `setup.ps1` | works; the produced image boots |
| SPL accepts the image (FIT with `version`, hashes, load / entry `0x40210000`) | works |
| application start, UART shell `arx />`, `upghid`, `fbinfo`, `fbfill`, `showlogo` | works |
| display: boot logo, key images from a host application, backlight PWM | works |
| USB enumeration with two HID interfaces, no other program involved | works (CPU mode of the CherryUSB driver) |
| host protocol: `DIS`, `LIG`, `BAT` uploads, `STP`, `CLE`, key / knob / touch events (own C# test program) | works |
| encoder push buttons and touch with real down / up edges | works |
| `enter-bootloader`, `hid-hwinfo`, `hid-dump`, `flash` through the HID bootloader (os) | works |
| BootROM recovery: PA.0 strap, `load-updater`, `flash --transport bulk --os` (stock os restored, new os written) | works |
| `logo` command (kit step 09): boot logo changed over USB | works (2026-10-25) |
| `hid-talk` (kit step 15): `DIS` / `LIG`, key, knob and touch events printed | works (2026-10-25) |
| `hid-dump --transport bulk` through the loaded updater (kit step 16), compared with the HID dump | works (2026-10-25) |
| plugging in, many replugs, fast knob turning and heavy upload traffic: every enumeration succeeds, no bus reset (fixes for the duplicate SETUP report and the unserialised IN frames, 2026-10-25) | works |
| `flash` of os + rodata + data + env through the HID bootloader (kit step 06) | works (2026-10-25) |

## Not verified / known gaps

| Area | State |
|---|---|
| LED strip | the stock firmware never drives it either (see hardware.md); commands are stored only |
| `BGPIC` background picture | ignored, as in the stock firmware |
| touch swipe thresholds | follow the stock numbers but the state machine is simplified |
| GitHub Actions: `ci` (firmware job) and `release` (tag `v*`) | not run yet |
| the intermittent "USB device not recognized" seen once with a wrong memory split | fixed by restoring the stock allocation; the 5 s re-attach watchdog is a safety net |

## Hardware checklist for a new build

1. Connect the UART (115200, PA.0 / PA.1 pads, see hardware.md) before flashing.
2. Dump the stock firmware once (`hid-dump`) and keep it.
3. `flash --os ... --write`; replug; the UART must show the SPL, `CRC32 verify OK`, `Run APP`, the panel and USB lines,
   the logo appears on the display.
4. `hid-list`: two HID interfaces of `0300:3004`; `akp05_tool hid-talk --cmd DIS --cmd LIG:30 --listen 20`: events when
   keys, knobs and the touch strip are used.
5. Send key images with your host program; check that `[app] upload complete` appears for every `BAT`.
6. If anything is wrong: UART log first, then [troubleshooting.md](troubleshooting.md); recovery is the PA.0 strap.
