AKP05E test kit
===============

Everything needed to test the open firmware on a PC. No installation, Windows 10/11 only. Copy the whole folder to a
path without spaces (e.g. C:\akp05_test_kit) and start the numbered .bat files by double click. Every .bat starts with
`rem` lines that state, in the same words as below, WHAT it runs, what it NEEDS and what it WRITES.

USE AT YOUR OWN RISK. No warranty. Flashing can brick a device; the authors accept no responsibility or liability for
bricked or damaged devices or any other consequence.

DO NOT SHARE this folder or its zip if it contains backup_original\ or updater\: they hold your own dump of the stock
firmware and the vendor's update image. Do not upload them to a release, an issue or a chat.

STATUS: experimental. The flashing steps write to the flash of a real device. The "spl" partition (the bootloader) is
never written, so a bad "os" can always be repaired with the BootROM recovery (steps 10-13).

1. What is in the folder
------------------------
akp05_tool.exe        the host tool, one file (Python + PyInstaller). Every .bat below is just a call of it.
                      Run "akp05_tool.exe --help" and "akp05_tool.exe <command> --help" for all options.
firmware\             the build to test:
    d13x_os.itb         the "os" partition: a FIT image with the RT-Thread firmware (about 0.46 MB, flashed to 0xC0000, 2 MiB slot)
    rodata.fatfs        FAT image for the "rodata" partition (flash 0x2C0000, 1 MiB)
    data.lfs            littlefs image for the "data" partition (flash 0x3C0000, 10 MiB); holds the boot logo
    env.bin             bootloader environment (4 KiB), written to both "env" and "env_r"
backup_original\      YOUR dump of the stock firmware, only if the kit was built with one (private, do not share):
    os.bin rodata.bin data.bin env.bin env_r.bin spl.bin partition_table.txt
                      "spl.bin" is only a record; no step writes it.
updater\default.img   the vendor update image; only its two RAM programs (PSRAM init + bootloader) are used, by steps 10-14
SHA256SUMS.txt        SHA-256 of every file above (PowerShell: Get-FileHash -Algorithm SHA256 <file>)

2. The steps, exactly
---------------------
Device states used below:  RUNNING = the firmware runs, USB 0300:3004.  HID BOOTLOADER = USB 33C3:8899, boot_stage 1.
BOOTROM = USB 33C3:6677 "Artinchip SoC" (PA.0 shorted to GND at power-on).  UPDATER = BOOTROM after step 10.

Step  Runs (akp05_tool.exe ...)                               Device state   Writes to the device
----  ------------------------------------------------------  -------------  --------------------------------------------
01    hid-list                                                any            nothing (read only)
02    enter-bootloader  (host command CRT 00 00 APPNEW)       RUNNING        nothing (the device reboots into the bootloader)
03    hid-hwinfo -v                                           HID BOOTLOADER nothing (read only); must print boot_stage=1
04    flash --os firmware\d13x_os.itb                         HID BOOTLOADER nothing (dry run: validates and prints the plan)
05    flash --os firmware\d13x_os.itb --write                 HID BOOTLOADER os partition, then read back and compared
06    flash --os --rodata --data --env (all from firmware\)   HID BOOTLOADER os, rodata, data, env, env_r; each read back
07    flash --os --rodata --data --env (all from backup_original\)  HID BOOTLOADER  os, rodata, data, env, env_r (stock restore)
08    hid-dump --out dump_<date>_<time>                       HID BOOTLOADER nothing to the device; writes .bin files to the PC
09    logo <your.jpg>  (host command LOG)                     RUNNING        the logo file /data/logo_jpg_dir/test_logo.jpg
10    usb-info; load-updater --img updater\default.img        BOOTROM        RAM only (updater); flash untouched
11    flash --transport bulk --os firmware\d13x_os.itb --write         UPDATER  os partition, then read back and compared
12    flash --transport bulk --os backup_original\os.bin              UPDATER  nothing (dry run of step 13)
13    flash --transport bulk --os backup_original\os.bin --write      UPDATER  os partition (stock), read back and compared
14    load-updater --only-psram --exec-offset N -v            BOOTROM        RAM only; debugging aid for step 10
15    hid-list; hid-talk --cmd DIS --cmd LIG:30 --listen 20   RUNNING        nothing to flash (changes backlight, connects)
16    hid-dump --transport bulk --out dump_bulk_<date>_<time> UPDATER        nothing to the device; writes .bin files to the PC, compares them with the HID dump
17    selftest  (host command CRT SELFT)                       RUNNING        nothing; device enters standalone selftest mode

Confirmations: steps 02 (ENTER), 05, 06, 07, 11, 13 (FLASH) and 10 (LOAD) ask you to type the word.
Every writing step reads the partition back and compares it byte for byte; "MISMATCH" or an error means: do not reboot,
run the step again (the device stays in the bootloader).

What each step is for
  01  Check that the PC sees the device. The running firmware shows two interfaces of 0300:3004.
  02  Switch from the running firmware to the HID bootloader without touching the board. Same effect: type "upghid"
      on the UART console. After it the screen is dark and 0300:3004 disappears.
  03  Confirm the bootloader answers; the line "boot_stage=1" is the go-ahead for all flash steps.
  04  Safe rehearsal of 05: checks the FIT magic, that the image fits, and that the device's partition table has the
      stock offsets. Nothing is sent to the flash.
  05  The normal way to install a new firmware: first the image.info component (selects the SPI NOR medium), then the
      os image, then a read-back. Replug USB afterwards.
  06  Same as 05 plus the other partitions (also replaces the boot logo with the one built into the kit).
  07  Go back to the stock firmware: writes your own dump from backup_original\ (spl is not part of it).
  08  Make a fresh dump of the whole flash (spl, env, env_r, os, rodata, data) before experimenting.
  09  Change the boot logo of the running firmware without flashing: drag a baseline JPEG (800x480) onto the .bat.
  10  Start of the BootROM recovery: runs the vendor's PSRAM initialiser (RAM 0x30044000) and bootloader (RAM 0x40100000)
      through the BootROM. Afterwards usb-info must show boot_stage=1. Do not unplug.
  11  Write a new os through the loaded updater (use when the new os crashes and the HID bootloader is unreachable).
  12  Rehearsal of 13.
  13  Put the stock os back through the updater.
  14  Only for debugging a failing step 10: runs just the PSRAM stage and checks that the BootROM answers afterwards.
      Optional argument: the exec offset (default 0; 0x108 and 0x100 hang the BootROM).
  15  Talk to the running firmware without any other program: DIS (connect, starts the 20 s watchdog), LIG 30 (backlight),
      then prints key 1..10, knob, knob-button and touch events for 20 s. Close your own application first.
  16  Dump partitions through the loaded updater (after step 10): first the partition table, then the partitions you type
      (default env, "all" = the whole flash), into dump_bulk_<date>_<time>; afterwards every file is compared byte for byte
      with the newest HID dump (step 08, folder dump_2...). Every line must say IDENTICAL.
  17  Enter the standalone hardware selftest mode (no host needed after this step). The device shows a menu on screen;
      navigate with the physical keys (keys 1-4: BUTTONS / DISPLAY / TOUCH / ENCODERS test, key 10: exit to menu,
      L+R encoder hold 3 s: exit selftest). Close your own application first.

3. Which steps to use when
--------------------------
Find your situation, run the steps in this order. "replug" = unplug USB, wait 2 s, plug it in again.

A. First time with this device (do this before anything else)
   01 -> 02 -> 03 -> 08
   See the device, switch to the HID bootloader, check boot_stage=1, make a dump of the whole flash. Keep the dump
   folder (dump_<date>_<time>): it is the only copy of your stock firmware. Then replug to start the firmware again.

B. Install a new firmware (the device works and shows up as 0300:3004)
   02 -> 03 -> 04 -> 05, then replug
   04 is the dry run of 05 and writes nothing. 05 writes only the os partition (the normal case). Use 06 instead of 05
   if you also want the kit's rodata, data (boot logo) and env: it writes all of them. Never needed for a plain update.

C. Change only the boot logo
   09 (drag a baseline JPEG, 800x480, onto the .bat)
   Works on the running firmware, no bootloader and no flashing of the os. The logo is stored on the device.

D. Look at what the running firmware does
   01 and 15
   15 connects like a host application (DIS, backlight 30) and prints key, knob and touch events for 20 s. Close your
   own application first, only one program can have the device open.

D2. Run the hardware selftest (verify buttons, display, touch strip and encoders without a host application)
   17
   The device enters selftest mode; use the physical keys and the touch strip to navigate and run each test.
   Can also be entered without a PC: hold the left and right encoder buttons for 3 s on the idle screen.

E. Go back to the stock firmware (the device still reaches the HID bootloader)
   02 -> 03 -> 07, then replug
   07 needs backup_original\ (your own dump, only present if the kit was built with one) and writes os, rodata, data and
   env from it. The bootloader (spl) is never written.

F. The device does not start, does not show up on USB, or the screen stays dark (bad os)
   BootROM recovery, details in section 4:
   short PA.0 to GND while plugging USB in -> 10 -> 11 (write the new os) or 12 -> 13 (write the stock os; 12 is its dry run),
   then remove the short and replug
   Use 11 to repair with a fixed build of the kit, 13 to put the stock os back (needs backup_original\). 14 only if 10
   fails with timeouts.

G. Check that a dump is complete, or dump without the HID bootloader
   short PA.0 to GND while plugging USB in -> 10 -> 16, then replug
   16 reads partitions through the updater and compares them with the newest dump from step 08 (every line must say
   IDENTICAL).

Quick map: 01 15 17 = look only / test only. 02 03 04 12 = prepare, nothing written. 08 16 = read the flash into files.
05 06 07 = write through the HID bootloader. 10 14 = BootROM, RAM only. 11 13 = write through the BootROM updater.
09 = logo only.

4. BootROM recovery in order
----------------------------
The SPL checks pin PA.0 (the UART TX pad) at power-on. While it is low the device enters the BootROM upgrade mode,
whatever is in os / rodata / data.
1. Unplug USB. Short the TX pad to GND through a 1 kOhm resistor.
2. Plug USB in, remove the resistor after 2-3 s. Windows shows "Artinchip SoC".
3. Step 10 (type LOAD), then step 11 (new os) or step 13 (stock os); steps 12 and 04 are the dry runs.
4. Unplug USB, make sure the short is gone, plug USB in again.
The vendor updater has another partition table (rodata 5m / data 5m): the tool refuses to write anything but os and
env through it.

5. Status of the steps on a real device
---------------------------------------
Every step (01-16) was run on a real AKP05E (09, 15, 06 and 16 on 2026-10-25). The occasional "HID does not come up after
plug-in" seen earlier was a defect of the USB driver and is fixed in the firmware. Details: docs/testing.md in the repository.

6. If something goes wrong
--------------------------
Keep the console output and the UART log (115200 8N1 on the TX pad, see docs/hardware.md and docs/boot-and-recovery.md)
and compare with docs/troubleshooting.md.
