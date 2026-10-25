# Host tool (`akp05_tool`)

A single Python program for Windows that dumps, flashes, recovers and talks to the AKP05E. Source in `host-tool/`:

| File | Role |
|---|---|
| `akp05_tool.py` | the command line (argparse), the flash logic and its safety checks |
| `upg_proto.py` | the upgrade protocol: framing, commands, partition table, dump and flash helpers ([bootloader-protocol.md](bootloader-protocol.md)) |
| `winhid.py` | Windows HID access with ctypes (enumeration, overlapped read / write); no third-party package |
| `tests/` | unit tests with software models of the device |
| `kit-template/` | the launchers and README that `scripts/make_test_kit.ps1` copies into the test kit |

Run it as `python host-tool\akp05_tool.py <command>` or as the executable `dist\akp05_tool.exe`
(`scripts\build_host_tool.ps1`, PyInstaller one-file, about 13 MB, includes libusb for the `bulk` transport).
The default transport needs only the standard library; `--transport bulk` / `libusb` need `pyusb` and `libusb-package`
(`host-tool/requirements.txt`; the exe already contains them).

## Commands

| Command | Device state | Writes? | What it does |
|---|---|---|---|
| `hid-list [--all] [--timing]` | any | no | list the HID interfaces of the Stream Dock (`--all`: every vendor); `--timing` prints the total time and the slowest steps |
| `hid-talk --cmd DIS --cmd LIG:30 --listen 20` | running firmware | display only | sends host commands (`DIS`, `HAN`, `STP`, `CON`, `LIG:<n>`, `CLE:<c>`, `hex:<bytes>`) and prints every event it reports |
| `logo <file.jpg>` | running firmware | boot logo file | `LOG` upload: baseline JPEG, FFD8...FFD9, 512 KiB at most; warns if not 800x480 |
| `enter-bootloader [--packet-hex]` | running firmware | no | sends `CRT APPNEW` (asks you to type `ENTER`): reboot into the HID bootloader |
| `hid-hwinfo` | HID bootloader | no | `GET_HWINFO`, `boot_stage` must be 1 |
| `hid-dump --out <dir> [--partitions os,env] [--list-only]` | HID bootloader (updater: `--transport bulk`) | no | read the partitions into `<dir>\<name>.bin` plus `partition_table.txt`, prints the CRC32 of each |
| `flash [--os f] [--rodata f] [--data f] [--env f] [--write] [--no-verify]` | HID bootloader or updater | **yes** | dry run unless `--write` (then type `FLASH`) |
| `usb-info` | BootROM / updater | no | `GET_HWINFO` of `33C3:6677` |
| `load-updater --img <vendor.img> [--exec-offset 0] [--only-psram]` | BootROM | RAM only | load the vendor updater into RAM (type `LOAD`) |

Common options: `--vid/--pid`, `--timeout`, `-v` (print every transfer), `--transport {winhid,libusb,bulk}`
(`winhid` = Windows HID API, default, for `33C3:8899`; `bulk` = the BootROM updater `33C3:6677`),
`--arg-layout {rv144,rv32}`.

Examples:

```powershell
akp05_tool.exe hid-list
akp05_tool.exe enter-bootloader                      # or type upghid on the UART
akp05_tool.exe hid-hwinfo -v                         # boot_stage=1
akp05_tool.exe hid-dump --out backup                 # keep this dump, it is your stock firmware
akp05_tool.exe flash --os build\images\d13x_os.itb                 # dry run
akp05_tool.exe flash --os build\images\d13x_os.itb --write         # type FLASH; reads back and compares
akp05_tool.exe flash --os d13x_os.itb --rodata rodata.fatfs --data data.lfs --env env.bin --write
```

BootROM recovery (see [boot-and-recovery.md](boot-and-recovery.md)):

```powershell
akp05_tool.exe load-updater --img private\vendor\updater.img      # type LOAD
akp05_tool.exe flash --transport bulk --os private\stock-dump\os.bin --write
```

> **Use at your own risk.** The safety rules below reduce the risk of a bad flash, they do not remove it. The authors
> accept no responsibility or liability for bricked or damaged devices.

## Safety rules built into `flash`

* Writable partitions: `os`, `rodata`, `data`, `env` (the `env` image goes to `env` and `env_r`). **`spl` is not in the list
  and the code has no path that writes it.**
* The image is checked before anything is sent: `os` must be a FIT (`d00dfeed` magic), every image must fit its
  partition, and the device's partition table must have the stock offsets and sizes for every partition about to be
  written (otherwise the write would land at the wrong flash offset, as with the vendor updater's `rodata 5m / data 5m`).
* Without `--write` it prints the plan and stops. With `--write` you must type `FLASH`.
* After writing, every written partition is read back and compared byte for byte; on a mismatch the tool says so and
  tells you not to reboot (the device stays in the bootloader and can be flashed again).
* The `image.info` component selects the SPI NOR medium before the first data component (without it, a fresh updater
  writes into RAM, see bootloader-protocol.md).

## Test kit for another PC

`scripts\make_test_kit.ps1` collects everything into `dist\akp05_test_kit\` (and a zip): `akp05_tool.exe`, the firmware
components, a backup of your stock firmware (from `private\stock-dump`), the vendor updater (from
`private\vendor\updater.img`), numbered `.bat` launchers and a `README.txt`. The kit needs no installation. The backup
and the updater are private, so the kit must not be published, attached to an issue or a release, or passed on
(the kit of a GitHub release is built without them, see [releasing.md](releasing.md)).
The kit's `README.txt` (source: `host-tool/kit-template/README.txt`) has a section "Which steps to use when" (first install, new firmware, logo only, back to stock, dead device, dump check) and lists every file and, for each numbered step, the exact
command it runs, the device state it needs and what it writes; every `.bat` repeats this in `rem` lines at its top.

## Tests

`PYTHONPATH=host-tool python -m unittest discover -s host-tool/tests -v` (no hardware needed).
`test_sim.py` and `test_flash.py` contain a software model of the bootloader (it implements the rules of the real one:
304-byte read argument, CSW status codes, RAM medium until `image.info`), `test_layout.py` the partition checks,
`test_logo.py` the JPEG validation and `test_talk.py` the command / event encoding. The models only prove that the host
code is consistent with the device-side source; the real device was used to check the protocol itself.
