@echo off
rem WHAT:    akp05_tool.exe usb-info, then load-updater --img updater\default.img, then usb-info again.
rem          load-updater writes two programs into RAM through the BootROM (PSRAM init to 0x30044000, the bootloader to
rem          0x40100000) and runs them (exec offset 0). NOTHING is written to flash.
rem NEEDS:   BootROM mode: pad PA.0 (UART TX) shorted to GND through 1 kOhm while USB was plugged in; Windows shows
rem          "Artinchip SoC" (33C3:6677). You type LOAD to confirm.
rem EXPECT:  the second usb-info shows boot_stage=1. Do not unplug; continue with step 11 or 12.
cd /d "%~dp0"
echo ==== BootROM recovery, step 1: load the updater into RAM (nothing is written to flash) ====
echo Precondition: pad PA.0 (UART TX) was shorted to GND through 1 kOhm while USB was plugged in, and Windows
echo shows "Artinchip SoC" (USB 33C3:6677). Type LOAD when asked.
akp05_tool.exe usb-info
echo.
akp05_tool.exe load-updater --img updater\default.img
echo.
echo The updater now answers on 33C3:6677 with boot_stage=1:
akp05_tool.exe usb-info
echo.
echo Next: 11_brom_flash_os.bat or 12_brom_restore_stock_os_dry_run.bat. Do NOT unplug the device.
pause
