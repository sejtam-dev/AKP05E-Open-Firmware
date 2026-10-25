# Build the host tool (host-tool\akp05_tool.py) into a single executable: dist\akp05_tool.exe
#   powershell -File scripts\build_host_tool.ps1
# Independent of the firmware build. Uses the local venv .local\venv, nothing is installed system wide.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$root\scripts\lib\session.ps1"
Save-Session
try {
    Set-Location $root
    . "$PSScriptRoot\lib\checks.ps1"
    Assert-Python

    $venv = "$root\.local\venv"
    if (-not (Test-Path "$venv\Scripts\python.exe")) {
        python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw "python -m venv failed (is Python 3 installed?)" }
    }
    $py = "$venv\Scripts\python.exe"
    & $py -m pip install --quiet pyinstaller pyusb libusb-package
    if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

    & $py -m PyInstaller --noconfirm --onefile --name akp05_tool `
        --distpath "$root\dist" --workpath "$root\build\host-tool" --specpath "$root\build\host-tool" `
        --paths "$root\host-tool" --collect-all libusb_package --hidden-import usb.backend.libusb1 `
        "$root\host-tool\akp05_tool.py"
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

    & "$root\dist\akp05_tool.exe" --help | Select-Object -First 3
    Get-Item "$root\dist\akp05_tool.exe" | Select-Object Name, Length
} finally {
    Restore-Session
}
