param(
    [string]$Version = "1.18.1",
    [string]$Destination = "third_party/onnxruntime"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$destinationPath = Join-Path $repoRoot $Destination
$downloadDir = Join-Path $repoRoot "third_party/_downloads"
$zipName = "onnxruntime-win-x64-$Version.zip"
$url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$zipName"
$zipPath = Join-Path $downloadDir $zipName
$extractRoot = Join-Path $downloadDir "onnxruntime-win-x64-$Version"

New-Item -ItemType Directory -Force $downloadDir | Out-Null
New-Item -ItemType Directory -Force (Split-Path $destinationPath -Parent) | Out-Null

Write-Host "Downloading ONNX Runtime $Version..."
Invoke-WebRequest -Uri $url -OutFile $zipPath

if (Test-Path $extractRoot) {
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}

Write-Host "Extracting $zipName..."
Expand-Archive -LiteralPath $zipPath -DestinationPath $downloadDir -Force

if (Test-Path $destinationPath) {
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}

Move-Item -LiteralPath $extractRoot -Destination $destinationPath

$required = @(
    (Join-Path $destinationPath "include/onnxruntime_cxx_api.h"),
    (Join-Path $destinationPath "lib/onnxruntime.lib"),
    (Join-Path $destinationPath "lib/onnxruntime.dll")
)

foreach ($path in $required) {
    if (-not (Test-Path $path)) {
        throw "ONNX Runtime download did not produce required file: $path"
    }
}

Write-Host "ONNX Runtime installed at $destinationPath"

