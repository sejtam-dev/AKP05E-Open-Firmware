@echo off
rem WHAT:    akp05_tool.exe enter-bootloader   - sends the host command "CRT 00 00 APPNEW" (hex 43525400004150504e4557)
rem          on the vendor HID interface. The running firmware reboots with reason 6 and the SPL starts the HID bootloader.
rem NEEDS:   the RUNNING firmware, USB 0300:3004. You type ENTER to confirm. WRITES: nothing to flash (the device reboots).
rem EXPECT:  the screen goes dark, the device re-enumerates as 33C3:8899 within about a second (check with step 03).
rem ALTERNATIVE: type "upghid" on the UART console of the device.
cd /d "%~dp0"
echo ==== Reboot the RUNNING firmware into the HID bootloader (command CRT APPNEW) ====
echo Close every program that has the device open first. Type ENTER when asked.
akp05_tool.exe enter-bootloader --packet-hex 43525400004150504e4557
echo.
echo If this does not work, type "upghid" on the UART console of the device.
pause
