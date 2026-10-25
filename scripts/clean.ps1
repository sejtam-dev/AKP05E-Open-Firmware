# Delete everything the scripts generate, e.g. before you copy or archive the project.
#   powershell -File scripts\clean.ps1          removes build\, dist\ and every __pycache__ (cheap to recreate)
#   powershell -File scripts\clean.ps1 -All     additionally removes sdk\ (3.3 GB) and .local\ (needs setup.ps1 again)
# Never touches private\ (your stock dump and the vendor updater) or any tracked file.
param([switch]$All)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$targets = @("$root\build", "$root\dist")
if ($All) { $targets += "$root\sdk", "$root\.local" }
foreach ($t in $targets) {
    if (Test-Path $t) {
        Write-Host "removing $t"
        Remove-Item -Recurse -Force $t
    }
}
Get-ChildItem $root -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike "$root\sdk\*" -and $_.FullName -notlike "$root\.local\*" } |
    ForEach-Object { Write-Host "removing $($_.FullName)"; Remove-Item -Recurse -Force $_.FullName }
Write-Host "Done."
