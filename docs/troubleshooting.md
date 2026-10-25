# Troubleshooting and lessons learned

Every entry below was a real problem during the bring-up of this firmware. **Look at the UART log first** (115200 8N1,
PA.0 / PA.1 pads, [hardware.md](hardware.md)); most causes are visible there. The recovery of last resort is the
PA.0 strap, see [boot-and-recovery.md](boot-and-recovery.md).

## Device does not boot or crashes early

| Symptom | Cause | Fix |
|---|---|---|
| `Selecting default config 'new firmware'` then `CPU Exception NO.5 ... mepc 4xxxxxxx mtval 00000000`, device stuck | the FIT has no `version` property in `images`; the SPL does a `strlen` on the missing value | `pack_image.py` inserts `version` (from `image_cfg.json`); check with `scripts/analysis/fit_dump.py` |
| nothing after `Start-up from os`, no `CRC32 verify OK` | hashes missing, wrong `load` / `entry`, or the image does not start at `0x40210000` | `fit_dump.py`; `AIC_PSRAM_SW_SIZE=0x210000`, linker script object name (build-system.md) |
| app starts but the screen stays black, `aicfb_probe() failed to find di component` | `CONFIG_AIC_DI_TYPE=2` (LVDS) in the defconfig, the panel is RGB | `CONFIG_AIC_DI_TYPE=1`, then `refresh_sdk_sources.ps1` |
| `pwm sample run failed! can't find device!` when a key is pressed | PWM driver not compiled (derived Kconfig options missing in the defconfig) | add `AIC_PWM_DRV`, `_V10`, `_VER`, `AIC_PWM_CH_NUM`, refresh |
| `efuse init fail` | the check was inverted: `drv_efuse_init()` returns `RT_TRUE` (1) on success | `if (!drv_efuse_init())` |
| `bitblt not support yuv format` | the decoder produced NV12 and the GE of the D13x cannot blit YUV | decode straight to the frame-buffer format (RGB565) |

## Display

| Symptom | Cause | Fix |
|---|---|---|
| grey lines at the top / left, a small grey box with white digits at the bottom left, flicker, page flipping by itself | the SDK's LVGL package with the vendor "meter demo" and performance monitor is enabled in the inherited defconfig and starts by itself (also prints `can't find device:gt911`) | `# CONFIG_LPKG_USING_LVGL is not set` (and the demo options), refresh the source list |
| logo drawn but invisible, garbage appears when a key lights the backlight | the panel was powered on lazily (only after the first host `CLE`), the page was never panned, frame-buffer memory is not cleared | `fb_power_on_once()` clears both pages and powers on, `display_logo()` pans to the page it drew |
| logo flickers between two images | page flipping by a second drawer (LVGL, above) or drawing to the visible page | draw to the hidden page, pan, wait for vsync, copy to the other page |

## USB

| Symptom | Cause | Fix |
|---|---|---|
| `hid-list` takes 5-10 s, UART shows `EP0 recv -120 bytes, 121 remained` and `Alarm! Invalid cache out of range` | CherryUSB AIC port in DMA mode mishandles the EP0 data stage of `SET_REPORT`, the control transfer never completes | `CONFIG_LPKG_CHERRYUSB_DEVICE_AIC_CPU=y` (and DMA off) |
| `USB device not recognized` on Windows, UART shows `Open ep ...` / `read hid report descriptor` only **once** / `usbd_ep_close: timeout` | the host aborts the enumeration after interface 0; seen when the image buffers were allocated from the wrong memory | allocate like the stock firmware (below); `usb_watchdog_tick()` re-attaches if the host resets the bus and the device is not configured within 5 s |
| Fast knob turning (or many keys at once) is followed by a bus reset of the host and a failed reconnect | the encoder, keyboard / touch and image threads each built a 512 byte frame on their own stack and called `usbd_ep_start_write()` for IN 0x82 at the same moment; the call is not atomic and the controller reads the frame later from the TX FIFO interrupt, so bursts raced each other (also the stock design). Reconnects then hit the SETUP problem in the next row | all events go through one queue (64 entries) and one static IN buffer in `usb_hid.c`, handed to the controller from the IN completion callback, never two at once; found 2026-10-25; verified on hardware the same day (heavy knob turning and key use, no reset, no disconnect) |
| After plug-in the device is not recognised, or only after the UART shows `USB: not configured 5 s after a bus reset, re-attaching`; the status line counts `ep0-bogus` / `stale` / `ep0-in fail`; the `usb trace` block shows the same request (typically `S 21 0a ...` SET_IDLE) twice | the ArtInChip CherryUSB port reports one SETUP packet through two interrupts. For a request without a data stage the status stage ends and the SETUP read is re-armed before the second report is served, so it is taken for a new request: the request runs twice (a stray status ZLP, EP0 IN stays enabled, error -2) and the real next SETUP is ignored; the host waits 5 s and resets the bus. Found 2026-10-25 from the request trace | `usb_hid.c` marks the setup buffer after every request and drops a report that finds the mark intact, then restores `ep0_ctrl_stage` (`--wrap=usbd_event_ep0_setup_complete_handler`); a second filter drops the "-8 bytes" EP0 OUT completion. Verified on hardware 2026-10-25: every enumeration succeeds at the first attempt |
| `hid-list` takes about 5 s per string request, only the **first** time after plugging in | Windows reads and caches the string descriptors of the interfaces on first use; later runs are fast. Not a firmware fault (observed 2026-10-25) | run it twice |
| `hid-list` is slow although nothing is wrong on the UART | run `akp05_tool.exe hid-list --timing`: it prints the total and the slowest steps. A slow `product string` / `manufacturer string` of `0300:3004` means the firmware answers the string request slowly (typical: a **USB debug build** that logs every EP0 request to the 115200 baud UART; reflash a normal build). A slow `open` means another program holds the device. Since 2026-10-25 the listing only opens devices of the requested vendor id | see the left column |
| the vendor HID interface (`usage page ffa0`) is missing in `hid-list` | the enumeration above failed half way | same |
| `hid-talk` / your own program times out when writing | another program has the device open (HidSharp / hidapi keep the handle) | close it |

## Host protocol

| Symptom | Cause | Fix |
|---|---|---|
| after `upload start: cmd 9 kind 5 length N` nothing more, the host times out on the next write | `stage_buf` was `NULL`: two 512 KiB `rt_malloc()` blocks do not fit the RT-Thread heap (it lives in the 1 MiB SRAM), the first data packet wrote to address 0 and hung the highest-priority thread | `img_buf = rt_malloc`, `stage_buf = aicos_malloc(MEM_CMA)` exactly like stock |
| the same fix applied to **both** buffers (both from CMA) | the CMA region (about 5.5 MB) gets more load than in stock and USB enumeration became unreliable | keep the stock split |
| `BGPIC` does nothing | the stock firmware ignores it too | not a bug |
| the device resets about 20 s after the host program stopped | the watchdog is fed only when the host sent something within the last 15 s | send any report (`CON`) periodically, or `CLE C` before closing |

## Flashing and recovery

| Symptom | Cause | Fix |
|---|---|---|
| first flash reports success but the read-back shows `MISMATCH`; the UART shows `Media: RAM(0)`; the second flash works | a fresh updater writes to its RAM medium until an `image.info` component selects `spi-nor` | the tool sends `image.info` first |
| `aicupg hid` / `APPNEW` takes seconds and the HID bootloader never appears; the device shows up as `Artinchip SoC` | the SDK's `aicupg` always uses reboot reason 4 (BootROM mode); stock uses reason 6 for `hid` | `reboot_to_upgrade(6)` in `main.c`, UART command `upghid` |
| `load-updater` hangs after `exec 0x30044108` | wrong entry offset | `--exec-offset 0` (default) |
| data writes time out | CBW command block length must be 1 (not 0) | `upg_proto.py` |
| `READ_FWC_DATA` fails with CSW status 1 | the argument must be exactly 304 bytes | `media_partition()` in `upg_proto.py` |
| `refusing to write rodata: the device's partition table has it at ...` | you are talking to the vendor updater with its `rodata 5m / data 5m` table | write only `os` / `env` through it; use the HID bootloader for the rest |

## Build and tooling

| Symptom | Cause | Fix |
|---|---|---|
| `undefined reference to sin / cos` | libraries before the objects (SCons uses `g++`) | `-Wl,--start-group -lm -lc -lgcc -Wl,--end-group` after the objects |
| entry point `0x40210534` instead of `0x40210000` | the linker script matches the startup object by name (`startup_gcc.o`) | CMake patches the script copy to `startup_gcc.S.obj` |
| load address `0x40000000` | `AIC_PSRAM_SW_SIZE` is 0 | `0x210000` |
| `cconfig.h` missing | only `--apply-def` ran | the dry run `scons -n` generates it (`setup.ps1`) |
| `scons --apply-def` / venv problems | the venv was created with the SDK's Python 2.7 | create `.local/venv` **before** `Enter-Sdk` (done in `setup.ps1`) |
| `KeyError: 'updater'` printed by `mk_image` | no vendor updater in `image_cfg.json`; the components are written before the error | expected, `pack_image.py` checks the results |
| `image_cfg.json` fails to parse | the SDK's JSON reader accepts only `//` comments | no `/* */` |
| `refresh_sdk_sources.ps1` stops with "NativeCommandError" | Windows PowerShell 5.1 turns native stderr into errors with `$ErrorActionPreference = 'Stop'` | the SCons run is wrapped in `cmd /c`; `python` after `Enter-Sdk` is Python 2.7, so the scripts call the SDK's Python 3.9 explicitly |
| CMake / the SDK scripts fail in a path with a space | the SDK scripts do not quote paths | keep the project path free of spaces |
| a changed defconfig has no effect / fails to compile | `--apply-def` does not re-evaluate Kconfig defaults; the source list is generated | list derived options explicitly, run `refresh_sdk_sources.ps1` |
| the banner line "Firmware compile time" does not change after a rebuild | it is compiled into `main.c` only | use `[app] display build ...` (printed when the panel is powered on) to identify a build |

## Working notes (for people and tools editing the repository)

* Do not generate scripts or sources with shell here-documents that contain backslash escapes (`\n` inside C strings,
  `\\` in regular expressions): the escapes get mangled. Write the file with an editor tool and check the result.
* `.gitignore` does not allow trailing comments on a pattern line.
* PowerShell here-strings must keep the closing `'@` in column 0.

## Reading the status line (`-AppLog 2`)

A build made with `scripts\build.ps1 -AppLog 2` prints every received command and, every 5 s, a line like

```
[app] status: host 1, rx 120 (bad 0, stray 0) cmds 30, uploads ok 15 bad 0 fifo-trunc 0, tx ok 40 fail 0 (last err 0),
      ring 0 B, fifo 0 B, mq 0, wdt-nofeed 0, usb resets 1 cfg 1 disc 0
```

(at level 1 the line only appears while a counter changes). Meaning:

| Field | Growing / non-zero means |
|---|---|
| `rx` / `bad` | OUT reports queued / rejected by the ring buffer (`bad` > 0: a report was lost, the stream is desynchronised) |
| `stray` | reports outside an upload that do not start with `CRT` (the first five are dumped): the host and the device disagree about where a command starts |
| `uploads bad`, `fifo-trunc` | an upload was rejected, or did not fit into the 512 KiB image fifo, so the **next** image is corrupt (the host sends faster than the pictures are decoded) |
| `tx fail`, `drop`, `last err` | `fail`: the controller refused a queued IN frame (retried later; -2 = IN endpoint still busy because the host does not read it, -4 = unaligned buffer). `drop`: the 64 entry event queue was full and a new event was lost (the host reads slower than keys / knobs produce events) |
| `ring`, `fifo`, `mq` | bytes / messages waiting; they must go back to 0 when the host is idle, a constant non-zero value means a stuck consumer thread |
| `wdt-nofeed` | watchdog ticks without host traffic; the device resets about 20 s after the last fed tick |
| `usb resets`, `cfg`, `disc` | bus resets, SET_CONFIGURATION requests and disconnects since boot; fast growth means the host keeps re-enumerating the device |

If the device stops answering while the host application runs, copy the last status lines and the output of the shell
commands `ps` and `free` (type them on the UART after the stop) into the bug report.
