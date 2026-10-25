# Preflight checks shared by the scripts. Dot-source it:  . "$PSScriptRoot\lib\checks.ps1"
# Each function throws one clear message that names the script to run first.

function Assert-SetupDone {
    # scripts\setup.ps1 must have run: SDK cloned, board overlaid, Kconfig applied, Ninja installed
    param([string]$Root)
    $missing = @()
    foreach ($f in "sdk\SConstruct", "sdk\rtconfig.h", "sdk\cconfig.h", "sdk\partition_table.h",
                   "sdk\toolchain\bin\riscv64-unknown-elf-gcc.exe",
                   "sdk\target\d13x\akp05-nor\lds\link_akp05.ld",
                   "sdk\tools\env\tools\Python39\python3.exe",
                   ".local\venv\Scripts\ninja.exe") {
        if (-not (Test-Path (Join-Path $Root $f))) { $missing += $f }
    }
    if ($missing.Count -gt 0) {
        throw ("The setup has not been done (missing: " + ($missing -join ", ") +
               ").`nRun first:  powershell -File scripts\setup.ps1")
    }
    # the overlay in the SDK must match the repository, otherwise the build uses stale board / config files
    $pairs = @(
        @("firmware\config\d13x_akp05-nor_rt-thread_akp05fw_defconfig", "sdk\target\configs\d13x_akp05-nor_rt-thread_akp05fw_defconfig"),
        @("firmware\image\image_cfg.json", "sdk\target\d13x\akp05-nor\pack\image_cfg.json"),
        @("firmware\board\akp05-nor\pinmux.c", "sdk\target\d13x\akp05-nor\pinmux.c"))
    foreach ($p in $pairs) {
        $a = Join-Path $Root $p[0]; $b = Join-Path $Root $p[1]
        if (-not (Test-Path $b) -or (Get-FileHash $a).Hash -ne (Get-FileHash $b).Hash) {
            throw ("The SDK overlay is out of date: $($p[0]) differs from the copy in sdk\.`n" +
                   "Run again:  powershell -File scripts\setup.ps1")
        }
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and -not (Test-Path "C:\Program Files\CMake\bin\cmake.exe")) {
        throw "CMake was not found (PATH or C:\Program Files\CMake\bin). Install CMake 3.20 or newer."
    }
}

function Assert-BuildDone {
    # scripts\build.ps1 must have produced the firmware components
    param([string]$Root)
    $img = Join-Path $Root "build\images"
    foreach ($f in "d13x_os.itb", "rodata.fatfs", "data.lfs", "env.bin") {
        $p = Join-Path $img $f
        if (-not (Test-Path $p) -or (Get-Item $p).Length -eq 0) {
            throw "The firmware has not been built (missing or empty: build\images\$f).`nRun first:  powershell -File scripts\build.ps1"
        }
    }
    $magic = [IO.File]::ReadAllBytes((Join-Path $img "d13x_os.itb"))[0..3]
    if (($magic | ForEach-Object { $_.ToString("x2") }) -join "" -ne "d00dfeed") {
        throw "build\images\d13x_os.itb is not a FIT image. Run:  powershell -File scripts\build.ps1 -Clean"
    }
    # warn when a source is newer than the image: the kit would carry an old firmware
    $newest = Get-ChildItem (Join-Path $Root "firmware") -Recurse -File -Exclude "*.jpg", "*.png" |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newest -and $newest.LastWriteTime -gt (Get-Item (Join-Path $img "d13x_os.itb")).LastWriteTime) {
        Write-Warning ("firmware\ was changed after the last build ($($newest.FullName.Substring($Root.Length + 1))): " +
                       "run scripts\build.ps1 again unless you want the old image.")
    }
}

function Assert-Python {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        throw "Python 3 was not found on PATH. Install Python 3.9 or newer."
    }
}
