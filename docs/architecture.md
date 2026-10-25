# Architecture

This document explains how the firmware is put together: which parts come from where, how the application is
structured at run time, and how everything ends up as a flashable image. Read [hardware.md](hardware.md) for the
board and [boot-and-recovery.md](boot-and-recovery.md) for the boot chain.

## 1. What is rebuilt and what is not

The AKP05E runs ArtInChip's Luban-Lite SDK (RT-Thread) with a small vendor application on top. The flash holds:

| Partition | Content | Who provides it |
|---|---|---|
| `spl` (512 KiB @ 0x000000) | tinySPL boot loader, also contains the HID upgrade bootloader | **stock, never touched** |
| `env`, `env_r` (128 KiB each) | U-Boot style environment (A/B state, boot counters) | `firmware/image/env.txt` |
| `os` (2 MiB) | FIT image with the RT-Thread firmware: kernel, drivers, **the application** | **this project** |
| `rodata` (1 MiB, FAT) | read-only files | `firmware/rootfs/rodata` |
| `data` (10 MiB, littlefs) | `/data/logo_jpg_dir/test_logo.jpg` (boot logo) | `firmware/rootfs/data` |

This project rebuilds the `os` partition from source:

```
stock bootloader (spl)  ->  FIT "os"  =  Luban-Lite v1.0.3 kernel + drivers (SDK, unmodified)
                                          + AKP05E board support  (firmware/board/akp05-nor)
                                          + application           (firmware/app, reconstructed from the stock firmware)
```

* The **SDK** (ArtInChip Luban-Lite v1.0.3, Apache-2.0) is not stored in this repository. `scripts/setup.ps1` clones it
  into `sdk/` at a pinned commit and overlays the board, the defconfig and the image configuration.
* The **board support** (`firmware/board/akp05-nor`) is the SDK's board template with the pinmux and clocks recovered
  from the stock firmware.
* The **application** (`firmware/app`) re-implements what the stock application does: the HID command protocol, the
  LCD, the key matrix, the encoders, the touch strip, the LEDs and the backlight. Each function carries a
  `stock 0x........` note with the address of the routine in the original firmware it was rebuilt from
  (see [stock-firmware.md](stock-firmware.md)).

## 2. Repository layout

```
CMakeLists.txt            the firmware build (CMake + Ninja); see build-system.md
cmake/                    toolchain file and the generated SDK source list (sdk_sources.cmake)
firmware/
  app/                    the application (C, about 2000 lines)
  board/akp05-nor/        SDK board overlay: pinmux, clocks, linker script template, Kconfig
  config/                 the SDK defconfig of the firmware
  image/                  image_cfg.json (partition layout) and env.txt (bootloader environment)
  rootfs/                 files for the rodata / data partitions (the boot logo is ignored by git)
host-tool/                akp05_tool.py: dump, flash, recovery, talk to the device (Python, Windows)
  kit-template/           launchers and README of the test kit built by scripts/make_test_kit.ps1
  tests/                  unit tests with software models of the device protocols
scripts/                  setup / build / kit scripts, maintenance and analysis helpers
docs/                     this documentation
private/                  (git-ignored) your stock dump, the vendor updater, analysis exports
sdk/  build/  dist/  .local/   (git-ignored) SDK clone, build tree, built tool + kit, local Python venv
```

## 3. Run-time structure of the application

### Threads

| Thread | Priority | Entry (file) | Role |
|---|---|---|---|
| `usb recv buf` | 0 (highest) | `usb_recv_thread_entry` (threads.c) | takes 1024-byte OUT reports from the receive ring, parses the command, runs it, or collects the data of an upload |
| `usb img event` | 1 | `usb_img_thread_entry` (threads.c) | takes finished uploads from a message queue: draws key images (`BAT`), stores the logo (`LOG`) |
| `ec11 scan` | 11 | `input_threads_entry_ec11` (input.c) | rotary encoders and their push buttons, every 1 ms |
| `keyboard scan` | 12 | `input_threads_entry_keyboard` (input.c) | key matrix, and the touch strip (every 30 ms, only while a host is connected) |
| `logo read` | 15 | `logo_thread_entry` (threads.c) | draws the boot logo, switches the backlight on, starts USB 1.5 s later |
| RT-Thread `main` | SDK default | `main()` (main.c) | creates the resources and the threads, then loops every 100 ms: boot requests, USB watchdog |

### Data flow

```
host ---- OUT 0x03 (1024 B) ----> custom_out_cb (ISR) -> rx ring (1 MiB) -> [usb recv buf]
                                                                               |  protocol_parse() -> command id
                                                      plain command  <---------+  dispatch_command()
                                                      upload header  ---------> stage buffer (512 KiB), one report per chunk
                                                                               |  complete + end marker (JPEG FFD9 / PNG IEND)
                                                                               v
                                                                 kfifo (512 KiB) + message queue -> [usb img event]
                                                                                 BAT -> display_key_image()   LOG -> logo file
host <--- IN 0x82 (512 B) ---- "ACK" frames: key, knob and touch events, upload acknowledgements
host <--- IN 0x81 (8 B)   ---- keyboard report (command KEY)
```

The wire format is described in [host-protocol.md](host-protocol.md).

### Global state (threads.c, akp05_app.h)

| Variable | Meaning |
|---|---|
| `g_host_state` | -1 no host seen yet, 0 host gone / logo mode, 1 host connected (set by `DIS`, cleared by `CLE C`) |
| `g_backlight_on`, `g_backlight_level` | backlight state; without a host the keys toggle the backlight |
| `g_clear_toggle` | frame-buffer page that is drawn to (0 or 1) |
| `g_stp_pending` | a `CLE` happened, the next `STP` flips the displayed page |
| `g_boot_request` | set by `APPNEW`; `main()` reboots into the bootloader |
| `g_led_*`, `g_leds_on` | LED colours, brightness and mode |

### Display

800x480, RGB565, panel `panel-rgb` driven by the SDK display engine (`aicfb`). The frame buffer is allocated by the
SDK in PSRAM as two pages of 768000 bytes. Drawing goes to a page with the graphics engine (GE); the page that is
visible is selected with `AICFB_PAN_DISPLAY`. Pictures are decoded by the hardware JPEG/PNG decoder (`mpp_decoder`)
straight into RGB565 (the GE of this chip cannot blit YUV).

* Boot logo: `display_logo()` follows the SDK's `decode_pic()` - draw to the hidden page, pan to it, wait for the
  vertical blank, copy it to the other page.
* Key images (`BAT`): drawn to page `g_clear_toggle` into a 112x112 tile; the page becomes visible on `STP`.
* `fb_power_on_once()` clears both pages and powers the panel on before anything is drawn (the allocator does not
  clear memory, an uncleared page shows garbage).

### Memory (see also hardware.md)

| Buffer | Size | Allocated from |
|---|---|---|
| receive ring | 1 MiB | PSRAM CMA region (`aicos_malloc(MEM_CMA)`) |
| `kfifo` | 512 KiB | PSRAM CMA |
| upload staging buffer | 512 KiB | PSRAM CMA |
| picture buffer `img_buf` | 512 KiB | RT-Thread heap (`rt_malloc`) |
| frame buffer, GE command queue | 1.5 MiB + small | PSRAM CMA, allocated by the SDK display driver |

The split between `rt_malloc` and CMA mirrors the stock firmware on purpose, see
[troubleshooting.md](troubleshooting.md) ("image upload hangs" and "USB is not recognized").

### Differences from the stock application

The reconstruction is faithful except where the stock behaviour was a limitation. The list of changes that are visible
to host software or plugins (encoder, touch, event delivery) is in [host-protocol.md](host-protocol.md) section 5.

| Area | Stock | This project |
|---|---|---|
| encoder push buttons | press only, then blocks scanning until released | separate down (1) and up (0) frames, non-blocking, 10 ms debounce |
| touch | fake tap (down + up 50 ms after the finger left); swipes and touches outside the tiles without an up | real down / up edges with x and y; swipes as an extra frame |
| USB | - | enumeration watchdog: re-attach when the host resets the bus but the device is not configured within 5 s |
| logging | few messages | `app_log()` events, level selectable at build time (`AKP05_APP_LOG`) |
| UART shell | prompt `arx` | same, plus the commands `upghid`, `fbinfo`, `fbfill`, `showlogo` |
| `BGPIC` (background) | received and ignored | received and ignored (same) |

## 4. From ELF to flash image

`scripts/pack_image.py` (called by the CMake target `image`) reproduces what the SDK's SCons build does after linking:

1. copy `firmware/image/*` and the SDK's `*.pbp` into `build/images/`,
2. `makefatfs.py` -> `rodata.fatfs`, `makelittlefs.py` -> `data.lfs`,
3. `elf_parse.py` -> `seg0.bin` + `d13x_os.its` (the device-tree source of the FIT),
4. add the `hash-1` (crc32) / `hash-2` (md5) nodes and the `version` property to the ITS (the stock bootloader
   needs both, see boot-and-recovery.md),
5. `mk_image.exe` -> `d13x_os.itb` (the FIT) and `env.bin`. It also tries to build a complete flash `.img` and stops
   with `KeyError: 'updater'`; that is expected because the vendor updater is deliberately not part of
   `image_cfg.json`, the components are written before the error.

Result in `build/images/`: `d13x_os.itb`, `rodata.fatfs`, `data.lfs`, `env.bin`, `d13x.elf`, `akp05_fw.map`.

The ELF is linked for **0x40210000** (`AIC_PSRAM_SW_SIZE = 0x210000`), exactly like the stock image; the first 2 MiB
of PSRAM belong to the bootloader / updater.
