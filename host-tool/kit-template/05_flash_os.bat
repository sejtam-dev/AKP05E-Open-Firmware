@echo off
rem WHAT:    akp05_tool.exe flash --os firmware\d13x_os.itb --write
rem          sends the image.info component, writes the "os" partition (2 MiB at flash offset 0xC0000), then reads the
rem          partition back and compares it byte for byte.
rem NEEDS:   the HID bootloader (step 02). You type FLASH to confirm. WRITES: the os partition only (never spl).
rem EXPECT:  "device crc ... (image ...), burn result 0", "os OK", "Flash complete and verified". Then replug USB.
rem          "MISMATCH" or an error: do NOT reboot, run the step again; the device stays in the bootloader.
cd /d "%~dp0"
echo ==== Flash ONLY the "os" partition from firmware\d13x_os.itb (never touches spl) ====
echo USE AT YOUR OWN RISK: no warranty, the authors accept no responsibility for bricked or damaged devices.
echo The device must be in the HID bootloader (step 02). Type FLASH when asked.
akp05_tool.exe flash --os firmware\d13x_os.itb --write
echo.
echo Afterwards unplug and replug USB.
pause
