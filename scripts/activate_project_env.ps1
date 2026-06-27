$ErrorActionPreference = "Stop"

$cmakeBin = "C:\Program Files\CMake\bin"
$pathKeys = @(
    [Environment]::GetEnvironmentVariables().Keys |
        Where-Object { [string]$_ -match '^(?i:path)$' }
)

if ($pathKeys.Count -gt 1) {
    Write-Warning "This terminal inherited duplicate PATH/Path variables. Opening a clean project shell."
    & (Join-Path $PSScriptRoot "start_clean_project_shell.ps1")
    exit $LASTEXITCODE
}

if (-not (Test-Path (Join-Path $cmakeBin "cmake.exe"))) {
    throw "CMake was not found at $cmakeBin. Install CMake or update scripts/activate_project_env.ps1 with the correct path."
}

$pathParts = $env:Path -split ';' | Where-Object { $_ -ne "" }
if ($pathParts -notcontains $cmakeBin) {
    $env:Path = "$cmakeBin;$env:Path"
}

Write-Host "Project environment active."
Write-Host "CMake: $(Join-Path $cmakeBin 'cmake.exe')"
