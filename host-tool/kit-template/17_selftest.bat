@echo off
rem WHAT:    akp05_tool.exe selftest
rem          sends the host command CRT SELFT: the device enters the standalone hardware selftest mode.
rem          The selftest runs entirely on the device; no further host interaction is required.
rem          Navigate with the physical keys (menu on keys 1-4), exit with L+R encoder hold (3 s).
rem NEEDS:   the RUNNING firmware (0300:3004); every other program that has the device open must be closed.
rem          WRITES: nothing.
rem NOTE:    not yet verified on a device.
cd /d "%~dp0"
echo ==== Enter standalone hardware selftest ====
echo Close your own application first: it must not hold the device open.
echo.
echo The device will enter selftest mode. Use the physical controls to run the tests:
echo   Key 1: BUTTONS test  (all 14 keys, exit via touch strip tile 4)
echo   Key 2: DISPLAY test  (colour phases and bars, auto-exits)
echo   Key 3: TOUCH test    (strip tiles and swipe direction, exit via key 10)
echo   Key 4: ENCODERS test (rotation counters and buttons, exit via key 10)
echo   Key 10: exit selftest and return to the logo screen
echo   L+R encoder hold 3 s: exit selftest from anywhere
echo.
akp05_tool.exe selftest
echo.
pause
