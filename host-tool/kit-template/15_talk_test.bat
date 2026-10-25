@echo off
rem WHAT:    akp05_tool.exe hid-list, then hid-talk --cmd DIS --cmd LIG:30 --listen 20
rem          sends the host commands DIS (connect: clears the screen, starts the watchdog) and LIG 30 (backlight level 30),
rem          then prints for 20 s every report the firmware sends: key (1..10), knob, knob button and touch events.
rem NEEDS:   the RUNNING firmware; every other program that has the device open must be closed. WRITES: nothing.
rem NOTE:    verified on a device (2026-10-25). The device resets about 20 s after the last host
rem          report while connected (watchdog), which is stock behaviour.
cd /d "%~dp0"
echo ==== Talk to the RUNNING firmware without any other program ====
echo Close your own application first: it must not hold the device open.
akp05_tool.exe hid-list
echo.
echo Sends DIS (wake) and LIG:30 (backlight), then prints key / knob / touch events for 20 s.
echo Press keys, turn and press the knobs, touch the display meanwhile.
akp05_tool.exe hid-talk --cmd DIS --cmd LIG:30 --listen 20
echo.
pause
