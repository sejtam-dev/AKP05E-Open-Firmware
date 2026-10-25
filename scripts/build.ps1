# Configure and build the firmware with CMake + Ninja (run scripts\setup.ps1 once before).
#   powershell -File scripts\build.ps1 [-Clean] [-UsbDebug] [-AppLog 0|1|2]
#   -Clean     delete the build tree first
#   -UsbDebug  log every USB control request to the UART (CherryUSB debug level)
#   -AppLog    UART log level of the application: 0 errors only, 1 events (default), 2 verbose (every command,
#              status line every 5 s)
# Output: build\images\ (d13x_os.itb, rodata.fatfs, data.lfs, env.bin, d13x.elf, akp05_fw.map)
param([switch]$Clean, [switch]$UsbDebug, [ValidateRange(0, 2)][int]$AppLog = 1)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$root\scripts\lib\session.ps1"
Save-Session
try {
    Set-Location $root
    . "$PSScriptRoot\lib\checks.ps1"
    Assert-SetupDone $root
    $cmake = if (Get-Command cmake -ErrorAction SilentlyContinue) { "cmake" } else { "C:\Program Files\CMake\bin\cmake.exe" }
    $env:PATH = "$root\.local\venv\Scripts;$root\sdk\toolchain\bin;$env:PATH"
    if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build }
    $dbg = if ($UsbDebug) { "ON" } else { "OFF" }
    & $cmake -S . -B build -G Ninja "-DAKP05_USB_DEBUG=$dbg" "-DAKP05_APP_LOG=$AppLog"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & $cmake --build build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    Get-ChildItem build\images\d13x_os.itb, build\images\rodata.fatfs, build\images\data.lfs, build\images\env.bin |
        Select-Object Name, Length | Format-Table -AutoSize
} finally {
    Restore-Session
}
