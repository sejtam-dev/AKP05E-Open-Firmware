@echo off
rem WHAT:    akp05_tool.exe logo FILE.jpg   - uploads a boot logo with the host command LOG (header report + 1024-byte
rem          chunks). The firmware writes it to /data/logo_jpg_dir/test_logo.jpg and redraws it.
rem NEEDS:   the RUNNING firmware (not the bootloader) and a baseline JPEG, 800x480 (other sizes only give a warning),
rem          FFD8...FFD9, 512 KiB at most. Drag the file onto this .bat. WRITES: the logo file in the data partition.
rem NOTE:    verified on a device (2026-10-25).
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag a baseline JPEG file ^(800x480^) onto this .bat file.
  echo It changes the boot logo of the RUNNING firmware, no flashing needed.
  pause
  exit /b 1
)
akp05_tool.exe logo "%~1"
echo.
pause
