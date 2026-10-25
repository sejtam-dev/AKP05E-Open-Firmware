# Boot chain and recovery

> **Use at your own risk.** The recovery described here worked on one device. It is not a guarantee: a damaged `spl`
> partition, a wrong pad or a short can still destroy the device. The authors accept no responsibility or liability for
> bricked or damaged devices.

How the device starts, how it enters the different upgrade modes, and **how to get a device back that does not boot**.
This knowledge came from the stock bootloader (`spl.bin`, disassembled) and from experiments on a real device.

## 1. Boot chain

```
power on
  BootROM (in the chip, USB id 33C3:6677 when it waits for a host)
    -> loads the "Pre-Boot Program" from the spl partition: initialises the PSRAM ("Psram_init done")
       -> tinySPL (built 2025-08-05) runs from PSRAM
            checks, in this order:
              1. reboot reason (register 0x19030100, bits 7:4)
              2. strap pin PA.0
            otherwise: reads the "os" partition (FIT), verifies it, jumps to 0x40210000
              -> RT-Thread starts, main() runs the application ("Run APP")
```

UART log of a normal start (115200 8N1, PA.0 pad):

```
Pre-Boot Program ... (24-08-19 16:45 2fba910)
SPINOR
Psram_init done.
goto run SPL
tinySPL [Built on Aug  5 2025 11:12:04]
Startup reason: Power-On-Reset
Start-up from os
Selecting default config 'new firmware'
spl read: 457216 byte ... CRC32 verify OK.
Failed to get mtd config            <- normal, also in the stock firmware
 1274265        : Run APP
```

## 2. How the bootloader decides what to do

### Reboot reasons

`aicupg <arg>` (RT-Thread shell command of the application) stores a reason in the reboot-reason register and resets
the chip with the watchdog. The stock command distinguishes the argument; the SDK's own `aicupg` does not:

| `aicupg` argument | Reason | What the SPL does |
|---|---|---|
| `hid` | **6** | runs the **HID upgrade at once**: USB `33C3:8899`, partition upgrade protocol (`Reboot action: Watchdog-Reset, reason: Unknown(6)`) |
| `gotobl` | 5 | unknown, not used by this project |
| anything else | 4 | "Enter upgrade mode by software": resets into the **BootROM** upgrade mode (`33C3:6677`), slow |

This project implements the stock behaviour in `main.c` (`reboot_to_upgrade(6)`): the host command `APPNEW` and the
UART command `upghid` both reboot into the HID bootloader within a second. The SDK `aicupg` command still exists in
the image but always uses reason 4, **do not use it for flashing**.

### Hardware strap: PA.0

In the SPL (`spl.bin`, function at 0x4416, strings "Enter upgrade mode by software / by hardware PIN") the pin
configuration comes from a resource section of the SPL image (type 4 at offset 0x6edc):

* write `0x10321` (GPIO input, pull-up) to the pin config register `0x18700080`,
* wait 500 us, read the port register `0x18700000`, mask bit 0,
* all four samples low -> **enter BootROM upgrade mode**.

PA.0 is the **UART0 TX pad**. Holding it low while power is applied makes the SPL hand over to the BootROM upgrade
mode no matter what is in the `os`, `rodata` and `data` partitions. This is the recovery entry. It is evaluated by the
first stage of the SPL, which is read from flash, so recovery works as long as the start of the `spl` partition is
intact (which is why this project never writes it).

## 3. The FIT image the SPL accepts

The `os` partition holds a FIT (flattened image tree, device-tree format) made by `mkimage`. Requirements learned the
hard way (inspect any image with `scripts/analysis/fit_dump.py`):

* `images { version = "1.0.0"; ... }` - **mandatory.** The SPL reads this string without a NULL check; a missing
  property makes it crash right after `Selecting default config` with `CPU Exception NO.5 ... mtval 0`
  (a `strlen(NULL)`).
* every image node needs `data-size`, `data-offset` (external data, relative to the 0x800-byte header), `load`, `entry`;
  `load = entry = 0x40210000` for the firmware segment;
* `hash-1 { algo = "crc32"; }` and `hash-2 { algo = "md5"; }` nodes (mkimage fills in the values); the SPL prints
  `CRC32 verify OK`;
* `configurations { default = "conf-1"; conf-1 { firmware = "seg0[,seg1]"; }; }`.

The stock image has two segments (code, and a small data part at 0x402b2c00); this project's linker script produces a
single segment, which the SPL loads just as well.

## 4. Recovery through the BootROM (device does not boot or does not enumerate)

Prerequisites: the host tool (`akp05_tool.exe`), your dump of the stock firmware, the vendor updater image
(`updater\default.img`, from the vendor's update tool, **not** in this repository), the Windows driver for
`33C3:6677` ("Artinchip SoC", comes with the vendor tool as `Artinchip_SoC.inf`, libusbK).

1. **Unplug USB.** Short the **TX pad (PA.0)** to **GND through a 1 kOhm resistor** (a bare wire works but a resistor
   is safer in case you picked the wrong pad).
2. **Plug USB in**, remove the resistor after 2-3 s. The device manager shows **"Artinchip SoC"**.
   The screen stays dark.
3. `akp05_tool.exe load-updater --img updater\default.img` (type `LOAD`). This writes the vendor's two RAM programs:
   the PSRAM initialiser (`0x30044000`) and the bootloader (`0x40100000`). **Nothing is written to flash.**
   Use `--exec-offset 0` (the default); `0x108` and `0x100` hang the BootROM.
4. The updater now answers on `33C3:6677` with `boot_stage = 1` (`akp05_tool.exe usb-info`). Do **not** unplug.
5. Write an image through it: `akp05_tool.exe flash --transport bulk --os <image> --write` (type `FLASH`).
   The updater of the vendor image has a different partition table (`rodata 5m, data 5m`); the tool therefore refuses
   to write anything but `os` and `env` through it (their offsets are identical).
6. Unplug USB, remove any short, plug USB again.

The test kit (`scripts/make_test_kit.ps1`) wraps these steps in `10_brom_load_updater.bat` ...
`13_brom_restore_stock_os.bat`.

If step 3 fails with timeouts: unplug, short PA.0 again and retry; `14_brom_diagnose_psram.bat` runs only the first
stage and checks that the BootROM still answers.

## 5. Normal flashing without a short

1. Running firmware -> HID bootloader: host command `CRT APPNEW` (`akp05_tool.exe enter-bootloader`, kit step 02) or
   `upghid` on the UART.
2. `akp05_tool.exe hid-hwinfo -v` must report `boot_stage=1`.
3. `akp05_tool.exe flash --os build\images\d13x_os.itb` (dry run), then add `--write`.
4. Replug USB.

The HID bootloader is part of the stock SPL and carries the real partition table (`rodata 1m, data 10m`), so
`rodata`, `data` and `env` can be written there as well.

## 6. Rules that keep the device recoverable

* **Never write the `spl` partition.** The tool has no way to do it.
* Before the first flash of a new build, make a dump (`hid-dump`) and keep it.
* Make sure you can reach the UART and the PA.0 pad before testing a risky build.
* After changing the image configuration or the defconfig, boot the result and look at the UART before you trust it.
* A build that crashes in the SPL (see section 3) or in early boot never reaches USB: only the BootROM path helps.
