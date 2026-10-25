@echo off
rem WHAT:    akp05_tool.exe flash --transport bulk --os firmware\d13x_os.itb --write   - same as step 05, but through the
rem          updater that step 10 loaded into RAM (bulk pipes of 33C3:6677 instead of the HID bootloader).
rem NEEDS:   the updater running (step 10). You type FLASH to confirm. WRITES: the os partition only.
rem NOTE:    the vendor updater has another partition table (rodata 5m / data 5m), so the tool refuses rodata / data here.
rem AFTER:   unplug USB, remove the PA.0 short, plug USB again.
cd /d "%~dp0"
echo ==== BootROM recovery, step 2: write the NEW os from firmware\d13x_os.itb ====
echo USE AT YOUR OWN RISK: no warranty, the authors accept no responsibility for bricked or damaged devices.
echo The updater must be running (step 10). Type FLASH when asked.
akp05_tool.exe flash --transport bulk --os firmware\d13x_os.itb --write
echo.
echo Afterwards: unplug USB, remove the PA.0 short, plug USB again.
pause
