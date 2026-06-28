param(
    [Parameter(Mandatory = $true)]
    [string]$PluginBuildDir,

    [Parameter(Mandatory = $true)]
    [string]$ObsRuntimeRoot
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

$projectRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
$pluginBuildDirResolved = Resolve-RequiredPath -Path $PluginBuildDir -Description "Plugin build directory"
$obsRuntimeRootResolved = Resolve-RequiredPath -Path $ObsRuntimeRoot -Description "OBS runtime root"

$pluginDll = Resolve-RequiredPath -Path (Join-Path $pluginBuildDirResolved "obs-auto-framing.dll") -Description "Plugin DLL"
$onnxRuntimeDll = Resolve-RequiredPath -Path (Join-Path $pluginBuildDirResolved "onnxruntime.dll") -Description "ONNX Runtime DLL"

$dataRoot = Resolve-RequiredPath -Path (Join-Path $projectRoot "data") -Description "Project data directory"
$effectsRoot = Resolve-RequiredPath -Path (Join-Path $dataRoot "effects") -Description "Effects directory"
$localeRoot = Resolve-RequiredPath -Path (Join-Path $dataRoot "locale") -Description "Locale directory"
$modelsRoot = Resolve-RequiredPath -Path (Join-Path $dataRoot "models") -Description "Models directory"
$modelFiles = @(Get-ChildItem -LiteralPath $modelsRoot -Filter "*.onnx" -File)
if ($modelFiles.Count -eq 0) {
    throw "No ONNX model files found in $modelsRoot. Run scripts\download_yolox_model.ps1 -Model tiny."
}

$requiredDataFiles = @(
    (Join-Path $effectsRoot "crop.effect"),
    (Join-Path $localeRoot "en-US.ini")
)

foreach ($file in $requiredDataFiles) {
    Resolve-RequiredPath -Path $file -Description "Required runtime data file" | Out-Null
}

$pluginDestinationDir = Join-Path $obsRuntimeRootResolved "obs-plugins\64bit"
$dataDestinationRoot = Join-Path $obsRuntimeRootResolved "data\obs-plugins\obs-auto-framing"
$effectsDestination = Join-Path $dataDestinationRoot "effects"
$localeDestination = Join-Path $dataDestinationRoot "locale"
$modelsDestination = Join-Path $dataDestinationRoot "models"

New-Item -ItemType Directory -Force -Path $pluginDestinationDir, $effectsDestination, $localeDestination, $modelsDestination | Out-Null

$installedPluginDll = Join-Path $pluginDestinationDir "obs-auto-framing.dll"
$installedOnnxRuntimeDll = Join-Path $pluginDestinationDir "onnxruntime.dll"

Copy-Item -LiteralPath $pluginDll -Destination $installedPluginDll -Force
Copy-Item -LiteralPath $onnxRuntimeDll -Destination $installedOnnxRuntimeDll -Force
Copy-Item -Path (Join-Path $effectsRoot "*") -Destination $effectsDestination -Recurse -Force
Copy-Item -Path (Join-Path $localeRoot "*") -Destination $localeDestination -Recurse -Force
foreach ($modelFile in $modelFiles) {
    Copy-Item -LiteralPath $modelFile.FullName -Destination (Join-Path $modelsDestination $modelFile.Name) -Force
}

$installedFiles = @(
    $installedPluginDll,
    $installedOnnxRuntimeDll,
    (Join-Path $effectsDestination "crop.effect"),
    (Join-Path $localeDestination "en-US.ini")
)
$installedFiles += $modelFiles | ForEach-Object { Join-Path $modelsDestination $_.Name }

Write-Host "Installed obs-auto-framing runtime files:"
foreach ($file in $installedFiles) {
    $resolved = Resolve-RequiredPath -Path $file -Description "Installed file"
    Write-Host "  $resolved"
}
