# Regenerate cmake/sdk_sources.cmake after the defconfig changed (run this whenever firmware\config\* is edited).
#   powershell -File scripts\maintenance\refresh_sdk_sources.ps1 [-Defconfig <name>]
# It runs a full verbose SCons build of the chosen defconfig (the SDK's own build is the reference for the compile
# flags, the include paths and the link order) and converts the log with gen_sdk_cmake.py. Takes several minutes.
param([string]$Defconfig = "d13x_akp05-nor_rt-thread_akp05fw_defconfig")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)       # repository root (scripts\maintenance\..\..)
. "$root\scripts\lib\session.ps1"
Save-Session
try {
    . "$root\scripts\lib\sdk_env.ps1"
    Enter-Sdk
    Copy-Item "$root\firmware\config\$Defconfig" "$root\sdk\target\configs" -Force
    scons --warn=no-visual-c-missing "--apply-def=$Defconfig" | Out-Null
    scons --warn=no-visual-c-missing -c | Out-Null
    New-Item -ItemType Directory -Force "$root\build\logs" | Out-Null
    $ErrorActionPreference = "Continue"      # scons writes warnings to stderr; PowerShell 5.1 would turn them into errors
    cmd /c "scons --warn=no-visual-c-missing --verbose -j8 > ""$root\build\logs\scons_full_verbose.log"" 2>&1"
    $ErrorActionPreference = "Stop"
    # SCons ends with an error in its mk_image post action (there is no vendor updater in image_cfg.json, expected).
    # What matters is that every source compiled and the ELF was linked.
    $log = Get-Content "$root\build\logs\scons_full_verbose.log" -Raw
    if ($log -cmatch "\.[ch]:\d+:\d+: (fatal )?error:" -or $log -notmatch "-o output/\S+/images/d13x\.elf") {
        throw "scons build failed (compile or link error), see build\logs\scons_full_verbose.log"
    }
    Set-Location $root
    # not `python`: Enter-Sdk puts the SDK's Python 2.7 first on PATH
    & "$root\sdk\tools\env\tools\Python39\python3.exe" "$root\scripts\maintenance\gen_sdk_cmake.py"
} finally {
    Restore-Session
}
