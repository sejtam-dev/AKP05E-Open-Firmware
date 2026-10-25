@echo off
rem WHAT:    akp05_tool.exe flash --os firmware\d13x_os.itb --rodata firmware\rodata.fatfs --data firmware\data.lfs
rem          --env firmware\env.bin --write   - writes os, rodata (1 MiB FAT), data (10 MiB littlefs, holds the boot logo)
rem          and env (written to env and env_r), each one verified by read-back.
rem NEEDS:   the HID bootloader (step 02). You type FLASH to confirm. WRITES: os, rodata, data, env, env_r (never spl).
rem NOTE:    much longer than step 05 (the data partition alone is 10 MiB). Replaces the boot logo with the one built into the kit.
rem          Verified on a device (2026-10-25).
cd /d "%~dp0"
echo ==== Flash os + rodata + data + env from firmware\ (rodata/data carry the boot logo) ====
echo USE AT YOUR OWN RISK: no warranty, the authors accept no responsibility for bricked or damaged devices.
echo The device must be in the HID bootloader (step 02). Type FLASH when asked.
akp05_tool.exe flash --os firmware\d13x_os.itb --rodata firmware\rodata.fatfs --data firmware\data.lfs --env firmware\env.bin --write
echo.
pause
