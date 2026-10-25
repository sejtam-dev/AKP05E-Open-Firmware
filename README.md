# AKP05E open firmware

An open, rebuildable firmware for the **Ajazz AKP05E** stream dock (ArtInChip **D13x** SoC, RISC-V, RT-Thread /
Luban-Lite), plus the Windows tool that dumps, flashes and recovers the device over USB.

The application of the original firmware was dumped from a real device, analysed and **rebuilt from source on top of the
unmodified ArtInChip SDK**. The result boots on the real hardware and speaks the same host protocol as the original
(displays pictures on the keys, reports keys, knobs and touches), so existing host software keeps working.
The original bootloader is never touched, which keeps the device recoverable.

> **Disclaimer - use at your own risk.** This is unofficial software, provided "as is", without any warranty.
> Writing firmware to a device can brick it and void its warranty. **The authors and contributors accept no
> responsibility or liability for bricked or otherwise damaged devices, lost data or any other consequence**; everything
> is done entirely at your own risk.
>
> **Status: working, still young.** It was brought up on one device. See [docs/testing.md](docs/testing.md) for what
> is verified and what is not. Flashing always carries a risk: read [docs/boot-and-recovery.md](docs/boot-and-recovery.md)
> (especially the recovery through the PA.0 strap) before you write anything.

## What it contains

| Part | Where | Notes |
|---|---|---|
| firmware: application, board support, defconfig, image layout | `firmware/` | C, built with CMake + Ninja against the Luban-Lite v1.0.3 SDK |
| host tool: dump, flash, recovery, talk to the device | `host-tool/` | Python 3 / Windows, also as one-file `akp05_tool.exe` |
| build, kit and analysis scripts | `scripts/` | PowerShell and Python |
| documentation | `docs/` | architecture, hardware, boot chain, protocols, build system, troubleshooting |

Features of the firmware: boot logo from flash (replaceable over USB), key images (`BAT`), backlight control, 4 rotary
encoders with push buttons and 2 x 5 LCD keys reported with press and release events, touch strip with tiles and
swipes, keyboard reports, the stock USB descriptors and commands, an enumeration watchdog, and a UART shell
(`upghid` reboots into the bootloader). Not implemented, exactly as in the stock firmware: the RGB LED strip output
and the background picture. Encoder pushes and the touch strip report real down / up events, which differs from the
stock firmware and can affect plugins that rely on the old behaviour: see
[docs/host-protocol.md](docs/host-protocol.md) section 5.

## Quick start

**Requirements** (Windows only: the scripts, the HID access in the host tool and the SDK toolchain are Windows-specific)

| You install | Used for |
|---|---|
| Git | cloning the SDK (and this repository) |
| Python 3.9+ on `PATH` | creating the helper venv `.local\venv`; running the host tool from source and the tests |
| CMake 3.20+ (on `PATH` or in `C:\Program Files\CMake\bin`) | configuring the firmware build |
| network access for the first `setup.ps1` | fetches Luban-Lite v1.0.3 (about 0.5 GB download, 3.3 GB on disk) and runs `pip install` (Ninja; PyInstaller, pyusb, libusb-package for the host tool exe) |
| a project path **without spaces**, about 4 GB free | the SDK scripts do not quote paths |

Everything else comes with the SDK or the venv and is not installed system wide: the RISC-V toolchain
(`riscv64-unknown-elf-gcc`, T-Head), SCons, Python 2.7 / 3.9, `mkimage`; Ninja, PyInstaller and `pyusb` live in `.local\venv`.

To **use** (not build) the device you additionally need: the vendor's `Artinchip_SoC.inf` driver (libusbK) for the BootROM
recovery mode `33C3:6677`, your own dump and the vendor updater image in `private\`, and, for the UART console, a 3.3 V USB-UART
adapter (see [docs/hardware.md](docs/hardware.md)).

```powershell
powershell -File scripts\setup.ps1        # once: clones the SDK (pinned), overlays the board, installs Ninja
powershell -File scripts\build.ps1        # builds build\images\d13x_os.itb (+ rodata.fatfs, data.lfs, env.bin)
powershell -File scripts\build_host_tool.ps1   # dist\akp05_tool.exe
```

Put your boot logo (800x480 JPEG) at `firmware/rootfs/data/logo_jpg_dir/test_logo.jpg` before the build if you want
the `data` partition to carry one (git ignores it; without it the screen stays dark until a logo is uploaded).

Before you flash, take a **dump of your own device** and keep it:

```powershell
akp05_tool.exe enter-bootloader                  # running firmware -> HID bootloader (or type "upghid" on the UART)
akp05_tool.exe hid-hwinfo -v                     # boot_stage must be 1
akp05_tool.exe hid-dump --out private\stock-dump
akp05_tool.exe flash --os build\images\d13x_os.itb            # dry run
akp05_tool.exe flash --os build\images\d13x_os.itb --write    # type FLASH; the tool reads back and compares
```

Replug USB afterwards. If the new image does not boot, short the UART TX pad (PA.0) to GND while plugging USB in and
restore through the BootROM, [docs/boot-and-recovery.md](docs/boot-and-recovery.md) shows how, and
`scripts\make_test_kit.ps1` packs everything (tool, images, your backup, numbered `.bat` launchers) into one folder for a
test PC.

> **Never share a kit you built yourself, and never upload your dump.** A locally built kit contains your own dump of the
> stock firmware and the vendor updater image (copied from the git-ignored `private\` folder). They belong to the
> manufacturer and to your device: do not attach them to a release, an issue or a pull request, and do not send them on.
> The kit in a GitHub release is built from the repository only and has neither of them.

## Repository layout

```
CMakeLists.txt  cmake/      firmware build, toolchain file, generated SDK source list
firmware/
  app/                      the application (protocol, display, input, touch, LEDs, USB)
  board/akp05-nor/          SDK board overlay (pinmux, clocks, linker script)
  config/                   SDK defconfig        image/  partition layout + bootloader env
  rootfs/                   files of the rodata / data partitions
host-tool/                  akp05_tool.py, upg_proto.py, winhid.py, tests/, kit-template/
scripts/                    setup.ps1 build.ps1 build_host_tool.ps1 make_test_kit.ps1 clean.ps1 pack_image.py
  lib/ maintenance/ analysis/
docs/                       documentation (start with docs/README.md)
private/                    git-ignored: your dump, the vendor updater, analysis exports
```

`sdk/`, `build/`, `dist/` and `.local/` are generated and git-ignored.

## Documentation

Start at [docs/README.md](docs/README.md). Highlights: [architecture](docs/architecture.md),
[hardware](docs/hardware.md), [boot chain and recovery](docs/boot-and-recovery.md),
[host protocol](docs/host-protocol.md), [upgrade protocol](docs/bootloader-protocol.md),
[build system](docs/build-system.md), [host tool](docs/host-tool.md), [troubleshooting](docs/troubleshooting.md).
Contributors and AI assistants: read [CLAUDE.md](CLAUDE.md) first.

## Legal

* The application and board code are derived from the ArtInChip Luban-Lite SDK (Apache-2.0) and from the observed
  behaviour of the stock firmware; this repository is licensed **Apache-2.0** ([LICENSE](LICENSE), [NOTICE](NOTICE)).
* **No manufacturer binaries are included**: no dump, no bootloader, no updater image, no logo. The scripts expect you
  to supply your own in `private/` (git-ignored). A kit built with those files is for your own use and must not be
  published or passed on.
* "Ajazz", "AKP05E" and "ArtInChip" belong to their owners; this project is not affiliated with them.
* **No warranty, no liability.** Use at your own risk; you alone are responsible for what you flash to your device. The
  authors and contributors are not responsible for bricked or damaged devices (see also Apache-2.0, sections 7 and 8).
