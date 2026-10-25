# One-time setup of the build tree: fetch the SDK, overlay the AKP05 board, apply the defconfig, install Ninja.
#   powershell -File scripts\setup.ps1 [-SdkSource <url or local path>] [-Ref <commit>]
# Everything it creates is ignored by git: sdk\ (about 3.3 GB with the toolchain) and .local\ (Python venv with Ninja).
param(
    [string]$SdkSource = "https://github.com/artinchip/luban-lite.git",
    [string]$Ref = "77fbe30dd2de366fae702ece84e1694153d94591",   # Luban-Lite v1.0.3, the version the stock firmware is based on
    [string]$Defconfig = "d13x_akp05-nor_rt-thread_akp05fw_defconfig"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$root\scripts\lib\session.ps1"
Save-Session
try {
    if ($root -match ' ') { throw "The project path must not contain spaces: $root" }
    Set-Location $root
    $sdk = Join-Path $root "sdk"

    # 1) SDK
    if (-not (Test-Path "$sdk\SConstruct")) {
        # Only the pinned commit, without the history (about 0.5 GB instead of 1 GB of git data, less to cache in CI).
        # SdkSource may also be a local clone of Luban-Lite (fast, no network): -SdkSource C:\path\to\luban-lite
        Write-Host "Fetching the SDK ($SdkSource, commit $Ref only) ..."
        $ErrorActionPreference = "Continue"      # git writes warnings to stderr, which PowerShell 5.1 treats as errors
        if (-not (Test-Path "$sdk\.git")) {
            git init --quiet $sdk
            if ($LASTEXITCODE -ne 0) { throw "git init failed" }
            git -C $sdk remote add origin $SdkSource
            if ($LASTEXITCODE -ne 0) { throw "git remote add failed" }
        }
        git -C $sdk fetch --quiet --depth 1 origin $Ref
        if ($LASTEXITCODE -ne 0) { throw "git fetch of $Ref from $SdkSource failed" }
        $ErrorActionPreference = "Stop"
    }
    git -C $sdk checkout --quiet $Ref
    if ($LASTEXITCODE -ne 0) { throw "cannot check out $Ref" }

    # 2) overlay: board, defconfig and image configuration (the SDK's own copies are replaced)
    $board = "$sdk\target\d13x\akp05-nor"
    New-Item -ItemType Directory -Force "$board\pack", "$board\lds" | Out-Null
    Copy-Item "$root\firmware\board\akp05-nor\*" $board -Recurse -Force
    Copy-Item "$root\firmware\image\image_cfg.json", "$root\firmware\image\env.txt" "$board\pack" -Force
    Copy-Item "$root\firmware\config\$Defconfig" "$sdk\target\configs" -Force

    # 3) Ninja for CMake (local venv, nothing is installed system wide). Done before the SDK environment is put on
    #    PATH, because that environment shadows "python" with the SDK's own (venv-less) Python.
    if (-not (Test-Path "$root\.local\venv\Scripts\ninja.exe")) {
        python -m venv "$root\.local\venv"
        if ($LASTEXITCODE -ne 0) { throw "python -m venv failed (is Python 3 installed?)" }
        & "$root\.local\venv\Scripts\python.exe" -m pip install --quiet ninja
        if ($LASTEXITCODE -ne 0) { throw "pip install ninja failed" }
    }

    # 4) Kconfig step: --apply-def writes .config / rtconfig.h and unpacks the toolchain that ships inside the SDK;
    #    a SCons dry run (a few seconds, nothing is compiled) then generates cconfig.h, partition_table.h and the
    #    linker script link_akp05.ld.
    . "$PSScriptRoot\lib\sdk_env.ps1"
    Enter-Sdk
    scons --warn=no-visual-c-missing "--apply-def=$Defconfig" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "scons --apply-def failed" }
    scons --warn=no-visual-c-missing -n | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "scons dry run failed" }
    foreach ($f in "rtconfig.h", "cconfig.h", "partition_table.h", "toolchain\bin\riscv64-unknown-elf-gcc.exe",
                   "target\d13x\akp05-nor\lds\link_akp05.ld") {
        if (-not (Test-Path "$sdk\$f")) { throw "setup incomplete: $f is missing" }
    }

    Write-Host "Setup done. Build with: powershell -File scripts\build.ps1"
} finally {
    Restore-Session
}
