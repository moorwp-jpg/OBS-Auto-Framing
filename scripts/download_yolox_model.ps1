param(
    [string]$Destination = "data/models/yolox_nano.onnx"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$destinationPath = Join-Path $repoRoot $Destination
$destinationDir = Split-Path $destinationPath -Parent
$url = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.onnx"

New-Item -ItemType Directory -Force $destinationDir | Out-Null

Write-Host "Downloading YOLOX-Nano ONNX model..."
Invoke-WebRequest -Uri $url -OutFile $destinationPath

if (-not (Test-Path $destinationPath)) {
    throw "YOLOX-Nano model download failed: $destinationPath"
}

if ((Get-Item $destinationPath).Length -le 0) {
    throw "YOLOX-Nano model is empty: $destinationPath"
}

Write-Host "YOLOX-Nano model installed at $destinationPath"

