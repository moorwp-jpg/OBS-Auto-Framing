param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$BuildConfig = "RelWithDebInfo",

    [string]$Preset = "windows-x64",

    [switch]$SkipConfigure,

    [switch]$RunTests,

    [switch]$Package
)

$ErrorActionPreference = "Stop"

$cleanShell = Join-Path $PSScriptRoot "start_clean_project_shell.ps1"
if (-not (Test-Path -LiteralPath $cleanShell -PathType Leaf)) {
    throw "Clean project shell helper not found: $cleanShell"
}

$commandLines = @(
    '$ErrorActionPreference = "Stop"'
)

if (-not $SkipConfigure) {
    $commandLines += "cmake --preset $Preset"
}

$commandLines += "cmake --build --preset $Preset --config $BuildConfig"

if ($RunTests) {
    if ($Preset -ne "windows-x64") {
        throw "-RunTests currently expects the windows-x64 preset because it uses build_x64 as the test directory."
    }
    $commandLines += "ctest --test-dir build_x64 -C $BuildConfig"
}

if ($Package) {
    $commandLines += "powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_release.ps1 -BuildConfig $BuildConfig"
}

& $cleanShell -Command ($commandLines -join "`r`n")
exit $LASTEXITCODE
