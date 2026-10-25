# Keep the caller's PowerShell session clean. A script that runs in the caller's session (".\scripts\build.ps1") must
# not leave the working directory or the environment changed. Usage in a script, right after $root is known:
#   . "$root\scripts\lib\session.ps1"; Save-Session
#   try { ...body... } finally { Restore-Session }
$script:SessionVars = "PATH", "PYTHONPATH", "ENV_ROOT", "PKGS_ROOT"

function Save-Session {
    $script:SessionOrigin = (Get-Location).Path
    $script:SessionEnv = @{}
    foreach ($n in $script:SessionVars) {
        $script:SessionEnv[$n] = [Environment]::GetEnvironmentVariable($n, "Process")
    }
}

function Restore-Session {
    foreach ($n in $script:SessionVars) {
        [Environment]::SetEnvironmentVariable($n, $script:SessionEnv[$n], "Process")
    }
    if ($script:SessionOrigin -and (Test-Path $script:SessionOrigin)) { Set-Location $script:SessionOrigin }
}
