@echo off
rem WHAT:    akp05_tool.exe flash --os firmware\d13x_os.itb   - validates the image and prints the plan. NO --write.
rem          Checks: FIT magic, size against the partition, the partition table of the device against the stock layout.
rem NEEDS:   the HID bootloader (step 02). WRITES: nothing.
rem EXPECT:  "flash plan (spl is never touched):" with one line for os and "Dry run only. Nothing was written."
cd /d "%~dp0"
echo ==== DRY RUN: validate firmware\d13x_os.itb against the device, write NOTHING ====
akp05_tool.exe flash --os firmware\d13x_os.itb
echo.
pause
