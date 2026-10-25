@echo off
rem WHAT:    akp05_tool.exe flash --transport bulk --os backup_original\os.bin --write   - writes YOUR stock os back
rem          through the updater and verifies it by read-back.
rem NEEDS:   the updater running (step 10) and backup_original\. You type FLASH. WRITES: the os partition only.
rem AFTER:   unplug USB, remove the PA.0 short, plug USB again: the stock firmware must start.
cd /d "%~dp0"
if not exist backup_original\os.bin (
  echo backup_original\os.bin is missing: this kit was built without a backup of the stock firmware.
  pause
  exit /b 1
)
echo ==== BootROM recovery: restore ONLY the stock os partition from backup_original\os.bin ====
echo USE AT YOUR OWN RISK: no warranty, the authors accept no responsibility for bricked or damaged devices.
echo The updater must be running (step 10). Type FLASH when asked.
akp05_tool.exe flash --transport bulk --os backup_original\os.bin --write
echo.
echo Afterwards: unplug USB, remove the PA.0 short, plug USB again.
pause
