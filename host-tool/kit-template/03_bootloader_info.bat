@echo off
rem WHAT:    akp05_tool.exe hid-hwinfo -v   - sends GET_HWINFO to the HID bootloader (33C3:8899) and prints every transfer.
rem NEEDS:   the HID bootloader (step 02). WRITES: nothing (read only).
rem EXPECT:  a HWINFO block ending with "boot_stage=1". Anything else: do not flash, see README.txt.
cd /d "%~dp0"
echo ==== Ask the HID bootloader for its hardware info (boot_stage must be 1) ====
akp05_tool.exe hid-hwinfo -v
echo.
pause
