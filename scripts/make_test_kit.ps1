# Collect everything needed to test on another PC into ONE folder (and a zip):
#   dist\akp05_test_kit\  = akp05_tool.exe + firmware\ + backup_original\ + updater\ + .bat launchers + README.txt
#   powershell -File scripts\make_test_kit.ps1 [-Backup <dir>] [-Updater <file>] [-NoBackup] [-SkipBuild]
# Run scripts\build.ps1 first (firmware images). The host tool exe is built here unless -SkipBuild.
# The backup (YOUR dump of the stock firmware) and the vendor updater image are private: they are copied into the
# kit only, never into the repository. Default locations are the ignored private\ folder.
param(
    [string]$Backup = "",
    [string]$Updater = "",
    [switch]$NoBackup,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$root\scripts\lib\session.ps1"
Save-Session
try {
    Set-Location $root
    . "$PSScriptRoot\lib\checks.ps1"
    Assert-BuildDone $root                  # build.ps1 must have produced the firmware components
    if ($Backup -eq "")  { $Backup  = "$root\private\stock-dump" }
    if ($Updater -eq "") { $Updater = "$root\private\vendor\updater.img" }

    if (-not $SkipBuild) {
        # the last step of build_host_tool.ps1 pipes --help into Select-Object, so its exit code is not checked here
        & "$root\scripts\build_host_tool.ps1"
    }
    $exe = "$root\dist\akp05_tool.exe"
    if (-not (Test-Path $exe)) { throw "missing $exe (run scripts\build_host_tool.ps1)" }

    $img = "$root\build\images"
    $parts = "d13x_os.itb", "rodata.fatfs", "data.lfs", "env.bin"
    foreach ($p in $parts) { if (-not (Test-Path "$img\$p")) { throw "missing $img\$p (run scripts\build.ps1)" } }

    $kit = "$root\dist\akp05_test_kit"
    if (Test-Path $kit) { Remove-Item -Recurse -Force $kit }
    New-Item -ItemType Directory -Force "$kit\firmware" | Out-Null

    Copy-Item $exe $kit
    foreach ($p in $parts) { Copy-Item "$img\$p" "$kit\firmware" }
    foreach ($f in Get-ChildItem "$root\host-tool\kit-template" -File) {
        if ($f.Extension -eq ".bat") {       # CRLF so that cmd.exe is happy whatever the git checkout did
            $text = (Get-Content $f.FullName -Raw) -replace "`r?`n", "`r`n"
            [IO.File]::WriteAllText("$kit\$($f.Name)", $text, (New-Object Text.UTF8Encoding($false)))
        } else {
            Copy-Item $f.FullName $kit
        }
    }
    if (Test-Path $Updater) {
        New-Item -ItemType Directory -Force "$kit\updater" | Out-Null
        Copy-Item $Updater "$kit\updater\default.img"
    } else {
        Write-Warning "no vendor updater image at ${Updater}: the BootROM recovery (steps 10-14) will not work (pass -Updater)"
    }

    if (-not $NoBackup) {
        $need = "os.bin", "rodata.bin", "data.bin", "env.bin", "env_r.bin", "spl.bin", "partition_table.txt"
        $missing = $need | Where-Object { -not (Test-Path "$Backup\$_") }
        if ($missing) {
            Write-Warning "backup incomplete in $Backup (missing: $($missing -join ', ')); the kit has no restore"
        } else {
            New-Item -ItemType Directory -Force "$kit\backup_original" | Out-Null
            foreach ($n in $need) { Copy-Item "$Backup\$n" "$kit\backup_original" }
        }
    } else {
        Write-Warning "-NoBackup: the kit has no restore, the restore scripts will refuse to run"
    }

    Get-ChildItem $kit -Recurse -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
        $rel = $_.FullName.Substring($kit.Length + 1)
        "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $rel
    } | Set-Content "$kit\SHA256SUMS.txt" -Encoding ASCII

    $zip = "$root\dist\akp05_test_kit.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $kit -DestinationPath $zip
    Write-Host ""
    Write-Host "Kit folder: $kit"
    Write-Host "Kit zip:    $zip  ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
} finally {
    Restore-Session
}
