@echo off
rem WHAT:    akp05_tool.exe hid-dump --transport bulk - reads partitions through the BootROM-loaded updater into
rem          dump_bulk_DATE_TIME (partition table first, then the partitions you choose) and compares every file with the
rem          newest HID dump (step 08, folder dump_2...) byte for byte.
rem NEEDS:   the loaded updater: BootROM mode (pad PA.0 shorted to GND through 1 kOhm while USB was plugged in), then
rem          step 10 (boot_stage=1). WRITES: only files on the PC, nothing to the device (read only).
rem EXPECT:  "IDENTICAL" for every file that exists in both dumps.
cd /d "%~dp0"
set OUT=dump_bulk_%DATE:~-4%%DATE:~3,2%%DATE:~0,2%_%TIME:~0,2%%TIME:~3,2%
set OUT=%OUT: =0%
echo ==== Read partitions through the updater (read only) into %OUT% ====
echo The updater must be loaded (step 10, boot_stage=1). Do not unplug the device.
akp05_tool.exe usb-info
echo.
akp05_tool.exe hid-dump --transport bulk --out "%OUT%" --list-only
echo.
set PARTS=env
set /p PARTS=Partitions to read, comma separated (spl,env,env_r,os,rodata,data), or "all" for the whole flash [env]:
if /i "%PARTS%"=="all" (
  akp05_tool.exe hid-dump --transport bulk --out "%OUT%"
) else (
  akp05_tool.exe hid-dump --transport bulk --out "%OUT%" --partitions %PARTS%
)
echo.
set REF=
for /f "delims=" %%d in ('dir /b /ad /o-d dump_2* 2^>nul') do if not defined REF set REF=%%d
if not defined REF (
  echo No HID dump to compare with ^(step 08 makes the folder dump_2...^): compare the CRC32 values above yourself.
) else (
  echo Comparing with the HID dump %REF%:
  for %%f in ("%OUT%\*.bin") do (
    if exist "%REF%\%%~nxf" (
      fc /b "%%f" "%REF%\%%~nxf" >nul && echo   %%~nxf  IDENTICAL || echo   %%~nxf  DIFFERENT - do not trust this dump
    ) else (
      echo   %%~nxf  no counterpart in %REF%
    )
  )
)
echo.
pause
