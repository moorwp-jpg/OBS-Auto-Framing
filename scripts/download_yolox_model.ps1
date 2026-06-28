param(
    [ValidateSet("nano", "tiny", "s", "all")]
    [string]$Model = "tiny",

    [string]$DestinationDir = "data/models"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$destinationDirPath = Join-Path $repoRoot $DestinationDir

$models = @{
    nano = @{
        File = "yolox_nano.onnx"
        Url = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.onnx"
        Label = "YOLOX-Nano"
    }
    tiny = @{
        File = "yolox_tiny.onnx"
        Url = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_tiny.onnx"
        Label = "YOLOX-Tiny"
    }
    s = @{
        File = "yolox_s.onnx"
        Url = "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_s.onnx"
        Label = "YOLOX-S"
    }
}

$selectedModels = if ($Model -eq "all") {
    @("nano", "tiny", "s")
} else {
    @($Model)
}

New-Item -ItemType Directory -Force $destinationDirPath | Out-Null

foreach ($modelName in $selectedModels) {
    $entry = $models[$modelName]
    $destinationPath = Join-Path $destinationDirPath $entry.File

    Write-Host "Downloading $($entry.Label) ONNX model..."
    Invoke-WebRequest -Uri $entry.Url -OutFile $destinationPath

    if (-not (Test-Path $destinationPath)) {
        throw "$($entry.Label) model download failed: $destinationPath"
    }

    if ((Get-Item $destinationPath).Length -le 0) {
        throw "$($entry.Label) model is empty: $destinationPath"
    }

    Write-Host "$($entry.Label) model installed at $destinationPath"
}
