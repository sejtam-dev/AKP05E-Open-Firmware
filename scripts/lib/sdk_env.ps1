# Dot-source this file to get the SDK's bundled build tools (SCons, Python 2.7/3.9, make) on PATH.
#   . .\scripts\lib\sdk_env.ps1 ; Enter-Sdk ; scons --apply-def=<defconfig>
$script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)       # repository root (scripts\lib\..\..)
$script:Sdk  = Join-Path $script:Root "sdk"
if ($script:Sdk -match ' ') { throw "The SDK path must not contain spaces: $script:Sdk" }

function Enter-Sdk {
    $t = "$script:Sdk\tools\env\tools"
    # Python39 must come first so that 'python3' is the bundled interpreter and not the Microsoft Store stub.
    $env:PATH = "$script:Sdk\toolchain\bin;$t\Python39;$t\Python27;$t\Python27\Scripts;$t\bin;" +
                "C:\Program Files\CMake\bin;$script:Root\.local\venv\Scripts;$env:PATH"
    $env:PYTHONPATH = "$t\Python27"
    $env:ENV_ROOT   = "$script:Sdk\tools\env"
    $env:PKGS_ROOT  = "$script:Sdk\tools\env\packages"
    Set-Location $script:Sdk
}
