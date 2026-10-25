@echo off
rem WHAT:    akp05_tool.exe load-updater --img updater\default.img --only-psram --exec-offset N --wait 20 -v
rem          runs ONLY the first updater stage (PSRAM init), prints every USB transfer and waits up to 20 s for the
rem          BootROM to answer again. Debugging aid for a failing step 10; nothing is written to flash.
rem NEEDS:   BootROM mode as for step 10 (redo the PA.0 short before every attempt). Optional argument: the exec offset
rem          (default 0; 0x108 and 0x100 hang the BootROM on the AKP05E).
cd /d "%~dp0"
set OFF=%1
if "%OFF%"=="" set OFF=0
echo Exec offset: %OFF%
akp05_tool.exe load-updater --img updater\default.img --only-psram --exec-offset %OFF% --wait 20 -v
echo.
pause
