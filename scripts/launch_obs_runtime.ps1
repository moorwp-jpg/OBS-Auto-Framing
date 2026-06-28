param(
    [string]$ObsRuntimeRoot = "C:\src\obs-studio\build_x64\rundir\RelWithDebInfo",
    [switch]$DisableRender,
    [switch]$DisableWorker
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ObsRuntimeRoot)) {
    throw "OBS runtime root not found: $ObsRuntimeRoot"
}

$obsExe = Join-Path $ObsRuntimeRoot "bin\64bit\obs64.exe"
if (-not (Test-Path -LiteralPath $obsExe)) {
    throw "OBS executable not found: $obsExe. Build the OBS obs-studio target first."
}

$requiredFiles = @(
    (Join-Path $ObsRuntimeRoot "obs-plugins\64bit\obs-auto-framing.dll"),
    (Join-Path $ObsRuntimeRoot "obs-plugins\64bit\onnxruntime.dll"),
    (Join-Path $ObsRuntimeRoot "data\obs-plugins\obs-auto-framing\effects\crop.effect"),
    (Join-Path $ObsRuntimeRoot "data\obs-plugins\obs-auto-framing\locale\en-US.ini")
)
$modelsDir = Join-Path $ObsRuntimeRoot "data\obs-plugins\obs-auto-framing\models"

foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Required obs-auto-framing runtime file is missing: $file"
    }
}

if (-not (Test-Path -LiteralPath $modelsDir) -or @(Get-ChildItem -LiteralPath $modelsDir -Filter "*.onnx" -File).Count -eq 0) {
    throw "No obs-auto-framing ONNX model files found in: $modelsDir"
}

$workingDirectory = Split-Path -Parent $obsExe

$oldDisableRender = [Environment]::GetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_RENDER", "Process")
$oldDisableWorker = [Environment]::GetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_WORKER", "Process")

try {
    if ($DisableRender) {
        [Environment]::SetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_RENDER", "1", "Process")
    }
    if ($DisableWorker) {
        [Environment]::SetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_WORKER", "1", "Process")
    }

    $process = Start-Process -FilePath $obsExe -WorkingDirectory $workingDirectory -PassThru
} finally {
    [Environment]::SetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_RENDER", $oldDisableRender, "Process")
    [Environment]::SetEnvironmentVariable("OBS_AUTO_FRAMING_DISABLE_WORKER", $oldDisableWorker, "Process")
}

Write-Host "Launched OBS from $obsExe"
Write-Host "Process ID: $($process.Id)"
if ($DisableRender) {
    Write-Host "Auto Framing render path disabled for this launch."
}
if ($DisableWorker) {
    Write-Host "Auto Framing worker path disabled for this launch."
}
