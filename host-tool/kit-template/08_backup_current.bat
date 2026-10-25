@echo off
rem WHAT:    akp05_tool.exe hid-dump --out dump_DATE_TIME   - reads every partition (spl, env, env_r, os, rodata, data)
rem          into one .bin file each, plus partition_table.txt, and prints the CRC32 of each file.
rem NEEDS:   the HID bootloader (step 02). WRITES: only files on the PC, nothing to the device (read only).
rem NOTE:    reads the whole 16 MiB of flash. Keep the dump: it is your only copy of the stock firmware.
cd /d "%~dp0"
set OUT=dump_%DATE:~-4%%DATE:~3,2%%DATE:~0,2%_%TIME:~0,2%%TIME:~3,2%
set OUT=%OUT: =0%
echo ==== Read every partition of the device (read only) into %OUT% ====
echo The device must be in the HID bootloader (step 02).
akp05_tool.exe hid-dump --out "%OUT%"
echo.
pause
