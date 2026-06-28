<#
.SYNOPSIS
Creates an OBS-compatible Windows x64 release zip for obs-auto-framing.

.DESCRIPTION
The package is assembled from explicit runtime files only. By default it bundles
YOLOX-Tiny, the recommended CPU model. Use -IncludeNano for a lightweight
fallback model or -IncludeSmall for the larger YOLOX-S model.
#>
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$BuildConfig,

    [string]$BuildDir = "build_x64",

    [string]$OutputDir = "out/release",

    [switch]$IncludeNano,

    [switch]$IncludeSmall,

    [switch]$NoChecksum
)

$ErrorActionPreference = "Stop"

function ConvertTo-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $script:ProjectRoot $Path))
}

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

function Test-IsUnderPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ChildPath,

        [Parameter(Mandatory = $true)]
        [string]$ParentPath
    )

    $child = [System.IO.Path]::GetFullPath($ChildPath).TrimEnd('\', '/')
    $parent = [System.IO.Path]::GetFullPath($ParentPath).TrimEnd('\', '/')

    return $child.Equals($parent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $child.StartsWith($parent + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function Copy-ReleaseFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$RelativeDestination
    )

    $destination = Join-Path $script:StagingRoot $RelativeDestination
    $destinationDir = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

function Format-RelativePathForDisplay {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    $rootPrefix = $rootFull + [System.IO.Path]::DirectorySeparatorChar

    if ($pathFull.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $pathFull.Substring($rootPrefix.Length)
    }

    return $pathFull
}

function Find-BuildOutputDir {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,

        [Parameter(Mandatory = $true)]
        [string]$Config,

        [Parameter(Mandatory = $true)]
        [string]$PluginName
    )

    $multiConfigDir = Join-Path $BuildRoot "bin\$Config"
    $singleConfigDir = Join-Path $BuildRoot "bin"

    if (Test-Path -LiteralPath (Join-Path $multiConfigDir "$PluginName.dll")) {
        return (Resolve-Path -LiteralPath $multiConfigDir).Path
    }

    if (Test-Path -LiteralPath (Join-Path $singleConfigDir "$PluginName.dll")) {
        return (Resolve-Path -LiteralPath $singleConfigDir).Path
    }

    throw "Plugin DLL was not found for $Config. Build first with: cmake --build --preset windows-x64 --config $Config"
}

function Validate-StagedFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedRelativePaths
    )

    foreach ($relativePath in $ExpectedRelativePaths) {
        $path = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Package validation failed; missing expected file: $relativePath"
        }
    }

    $blockedExtensions = @(".exe", ".pdb", ".ilk", ".exp", ".lib", ".obj", ".iobj", ".ipdb")
    $blockedFiles = Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $blockedExtensions -contains $_.Extension.ToLowerInvariant() }

    if ($blockedFiles.Count -gt 0) {
        $relativeBlockedFiles = $blockedFiles | ForEach-Object {
            Format-RelativePathForDisplay -Root $Root -Path $_.FullName
        }
        throw "Package validation failed; build artifacts entered the release staging area: $($relativeBlockedFiles -join ', ')"
    }
}

function Validate-ZipEntries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ZipPath,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedRelativePaths
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        foreach ($relativePath in $ExpectedRelativePaths) {
            $expected = $relativePath.Replace('\', '/')
            if ($entryNames -notcontains $expected) {
                throw "Package validation failed; zip is missing expected entry: $expected"
            }
        }

        $blockedExtensions = @(".exe", ".pdb", ".ilk", ".exp", ".lib", ".obj", ".iobj", ".ipdb")
        $blockedEntries = @($archive.Entries | Where-Object {
            $extension = [System.IO.Path]::GetExtension($_.FullName).ToLowerInvariant()
            $blockedExtensions -contains $extension
        })

        if ($blockedEntries.Count -gt 0) {
            throw "Package validation failed; build artifacts entered the release zip: $($blockedEntries.FullName -join ', ')"
        }
    }
    finally {
        $archive.Dispose()
    }
}

$script:ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildspecPath = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "buildspec.json") -Description "Build metadata"
$buildspec = Get-Content -Raw -LiteralPath $buildspecPath | ConvertFrom-Json

$pluginName = [string]$buildspec.name
$version = [string]$buildspec.version
if ([string]::IsNullOrWhiteSpace($pluginName)) {
    throw "Build metadata is missing name."
}
if ([string]::IsNullOrWhiteSpace($version)) {
    throw "Build metadata is missing version."
}

$buildRoot = ConvertTo-ProjectPath -Path $BuildDir

if (-not $PSBoundParameters.ContainsKey("BuildConfig") -or [string]::IsNullOrWhiteSpace($BuildConfig)) {
    foreach ($candidateConfig in @("RelWithDebInfo", "Release")) {
        if (Test-Path -LiteralPath (Join-Path $buildRoot "bin\$candidateConfig\$pluginName.dll")) {
            $BuildConfig = $candidateConfig
            break
        }
    }

    if ([string]::IsNullOrWhiteSpace($BuildConfig)) {
        $BuildConfig = "RelWithDebInfo"
    }
}

$buildOutputDir = Find-BuildOutputDir -BuildRoot $buildRoot -Config $BuildConfig -PluginName $pluginName
$pluginDll = Resolve-RequiredPath -Path (Join-Path $buildOutputDir "$pluginName.dll") -Description "Plugin DLL"

$onnxRuntimeDllCandidate = Join-Path $buildOutputDir "onnxruntime.dll"
if (Test-Path -LiteralPath $onnxRuntimeDllCandidate) {
    $onnxRuntimeDll = (Resolve-Path -LiteralPath $onnxRuntimeDllCandidate).Path
}
else {
    $onnxRuntimeDll = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "third_party\onnxruntime\lib\onnxruntime.dll") -Description "ONNX Runtime DLL"
}

$cropEffect = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "data\effects\crop.effect") -Description "Crop effect"
$localeFile = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "data\locale\en-US.ini") -Description "English locale"
$readmeFile = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "README.md") -Description "README"
$installGuide = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "docs\install.md") -Description "Install guide"

$modelNames = @("yolox_tiny.onnx")
if ($IncludeNano) {
    $modelNames += "yolox_nano.onnx"
}
if ($IncludeSmall) {
    $modelNames += "yolox_s.onnx"
}

$modelFiles = @{}
foreach ($modelName in $modelNames) {
    $modelFiles[$modelName] = Resolve-RequiredPath -Path (Join-Path $ProjectRoot "data\models\$modelName") -Description "Model file"
}

$outputDirPath = ConvertTo-ProjectPath -Path $OutputDir
New-Item -ItemType Directory -Force -Path $outputDirPath | Out-Null
$outputDirResolved = (Resolve-Path -LiteralPath $outputDirPath).Path

$packageName = "$pluginName-v$version-windows-x64"
$zipPath = Join-Path $outputDirResolved "$packageName.zip"
$checksumPath = "$zipPath.sha256"
$stagingParent = Join-Path $outputDirResolved "_staging"
$script:StagingRoot = Join-Path $stagingParent $packageName

if (-not (Test-IsUnderPath -ChildPath $StagingRoot -ParentPath $outputDirResolved)) {
    throw "Refusing to stage outside the output directory: $StagingRoot"
}

if (Test-Path -LiteralPath $StagingRoot) {
    Remove-Item -LiteralPath $StagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $StagingRoot | Out-Null

$expectedLayout = @(
    "obs-plugins\64bit\$pluginName.dll",
    "obs-plugins\64bit\onnxruntime.dll",
    "data\obs-plugins\$pluginName\effects\crop.effect",
    "data\obs-plugins\$pluginName\locale\en-US.ini",
    "data\obs-plugins\$pluginName\models\yolox_tiny.onnx",
    "README.md",
    "docs\install.md"
)

Copy-ReleaseFile -Source $pluginDll -RelativeDestination "obs-plugins\64bit\$pluginName.dll"
Copy-ReleaseFile -Source $onnxRuntimeDll -RelativeDestination "obs-plugins\64bit\onnxruntime.dll"
Copy-ReleaseFile -Source $cropEffect -RelativeDestination "data\obs-plugins\$pluginName\effects\crop.effect"
Copy-ReleaseFile -Source $localeFile -RelativeDestination "data\obs-plugins\$pluginName\locale\en-US.ini"
Copy-ReleaseFile -Source $readmeFile -RelativeDestination "README.md"
Copy-ReleaseFile -Source $installGuide -RelativeDestination "docs\install.md"

foreach ($modelName in $modelNames) {
    $relativeModelPath = "data\obs-plugins\$pluginName\models\$modelName"
    Copy-ReleaseFile -Source $modelFiles[$modelName] -RelativeDestination $relativeModelPath
    if ($expectedLayout -notcontains $relativeModelPath) {
        $expectedLayout += $relativeModelPath
    }
}

Validate-StagedFiles -Root $StagingRoot -ExpectedRelativePaths $expectedLayout

if (Test-Path -LiteralPath $zipPath) {
    if (-not (Test-IsUnderPath -ChildPath $zipPath -ParentPath $outputDirResolved)) {
        throw "Refusing to overwrite a zip outside the output directory: $zipPath"
    }
    Remove-Item -LiteralPath $zipPath -Force
}

Compress-Archive -Path (Join-Path $StagingRoot "*") -DestinationPath $zipPath -Force
Validate-ZipEntries -ZipPath $zipPath -ExpectedRelativePaths $expectedLayout

if (-not $NoChecksum) {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath
    "$($hash.Hash.ToLowerInvariant()) *$([System.IO.Path]::GetFileName($zipPath))" |
        Set-Content -LiteralPath $checksumPath -NoNewline -Encoding ASCII
}

Write-Host "Created release package:"
Write-Host "  $zipPath"
if (-not $NoChecksum) {
    Write-Host "  $checksumPath"
}
Write-Host "Bundled models:"
foreach ($modelName in $modelNames) {
    Write-Host "  $modelName"
}
