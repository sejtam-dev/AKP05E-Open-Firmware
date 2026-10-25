@echo off
rem WHAT:    akp05_tool.exe flash --os backup_original\os.bin --rodata backup_original\rodata.bin
rem          --data backup_original\data.bin --env backup_original\env.bin --write   - puts YOUR stock dump back.
rem NEEDS:   the HID bootloader (step 02) and backup_original\ (present only if the kit was built with a dump).
rem          You type FLASH to confirm. WRITES: os, rodata, data, env, env_r (never spl).
rem USE:     to go back to the original firmware after testing. If the device does not enumerate at all, use steps 10-13.
cd /d "%~dp0"
if not exist backup_original\os.bin (
  echo backup_original\os.bin is missing: this kit was built without a backup of the stock firmware.
  pause
  exit /b 1
)
echo ==== Restore the STOCK os + rodata + data + env from backup_original\ (spl is never written) ====
echo USE AT YOUR OWN RISK: no warranty, the authors accept no responsibility for bricked or damaged devices.
echo The device must be in the HID bootloader (step 02). Type FLASH when asked.
akp05_tool.exe flash --os backup_original\os.bin --rodata backup_original\rodata.bin --data backup_original\data.bin --env backup_original\env.bin --write
echo.
pause
