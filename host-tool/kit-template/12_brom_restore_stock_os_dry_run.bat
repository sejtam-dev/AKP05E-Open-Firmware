@echo off
rem WHAT:    akp05_tool.exe flash --transport bulk --os backup_original\os.bin   - dry run of step 13.
rem NEEDS:   the updater running (step 10) and backup_original\. WRITES: nothing.
rem EXPECT:  the partition table of the updater, a plan with one line for os (2097152 bytes) and "Dry run only".
cd /d "%~dp0"
echo ==== DRY RUN of the stock os restore through the BootROM updater, writes NOTHING ====
akp05_tool.exe flash --transport bulk --os backup_original\os.bin
echo.
pause
