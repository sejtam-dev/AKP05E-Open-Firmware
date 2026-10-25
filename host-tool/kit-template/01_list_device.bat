@echo off
rem WHAT:    akp05_tool.exe hid-list   - lists the HID interfaces of the Stream Dock (VID 0x0300, any PID).
rem NEEDS:   nothing; the device in any USB state. WRITES: nothing (read only).
rem EXPECT:  running firmware: two lines for 0300:3004 (interface 0 usage page ffa0 = vendor commands, interface 1 = keyboard).
rem          No line: the device is in the HID bootloader (33C3:8899, not listed here) or not enumerated.
cd /d "%~dp0"
echo ==== List the AKP05 HID interfaces (running firmware: 0300:3004, bootloader: 33C3:8899) ====
akp05_tool.exe hid-list
echo.
pause
