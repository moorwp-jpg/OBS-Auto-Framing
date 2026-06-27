param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ObsRoot,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ObsBuildRoot,

    [string]$Destination = "third_party/obs"
)

$ErrorActionPreference = "Stop"

function Resolve-RequiredDirectory {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description does not exist: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Find-PreferredBuildFile {
    param(
        [string]$Root,
        [string]$Name
    )

    $sortProperties = @(
        @{ Expression = { if ($_.DirectoryName -match '(?i)[/\\]libobs([/\\]|$)') { 0 } else { 1 } } }
        @{ Expression = { if ($_.DirectoryName -match '(?i)[/\\]RelWithDebInfo([/\\]|$)') { 0 } else { 1 } } }
        "FullName"
    )
    $candidates = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $Name |
        Sort-Object -Property $sortProperties)

    if ($candidates.Count -eq 0) {
        return $null
    }

    return $candidates[0].FullName
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$obsRootPath = Resolve-RequiredDirectory -Path $ObsRoot -Description "OBS source tree"
$obsBuildRootPath = Resolve-RequiredDirectory -Path $ObsBuildRoot -Description "OBS build tree"
$libobsSourcePath = Join-Path $obsRootPath "libobs"

if (-not (Test-Path -LiteralPath $libobsSourcePath -PathType Container)) {
    throw "OBS source tree is missing libobs: $libobsSourcePath. Pass -ObsRoot pointing at the root of an OBS source checkout."
}

foreach ($header in @("obs-module.h", "obs.h")) {
    $headerPath = Join-Path $libobsSourcePath $header
    if (-not (Test-Path -LiteralPath $headerPath -PathType Leaf)) {
        throw "OBS source tree is missing libobs/${header}: $headerPath. Ensure -ObsRoot points at an OBS source checkout."
    }
}

foreach ($directory in @("graphics", "media-io", "util")) {
    $directoryPath = Join-Path $libobsSourcePath $directory
    if (-not (Test-Path -LiteralPath $directoryPath -PathType Container)) {
        throw "OBS source tree is missing libobs/${directory}: $directoryPath. The source checkout is incomplete."
    }
}

$libraryPath = Find-PreferredBuildFile -Root $obsBuildRootPath -Name "obs.lib"
if (-not $libraryPath) {
    throw "Could not find obs.lib below $obsBuildRootPath. Build OBS Studio first and pass its build_x64 directory as -ObsBuildRoot."
}

$configHeaderPath = Find-PreferredBuildFile -Root $obsBuildRootPath -Name "obs-config.h"
if (-not $configHeaderPath) {
    $sourceConfigHeaderPath = Join-Path $libobsSourcePath "obs-config.h"
    if (Test-Path -LiteralPath $sourceConfigHeaderPath -PathType Leaf) {
        $configHeaderPath = $sourceConfigHeaderPath
    } else {
        throw "Could not find obs-config.h below $obsBuildRootPath or $libobsSourcePath. Configure and build OBS Studio before importing its development files."
    }
}

$obsConfigHeaderPath = Find-PreferredBuildFile -Root $obsBuildRootPath -Name "obsconfig.h"
if (-not $obsConfigHeaderPath) {
    throw "Could not find generated obsconfig.h below $obsBuildRootPath. Configure OBS Studio before importing its development files."
}

$destinationPath = Join-Path $repoRoot $Destination
$stagingPath = "$destinationPath.importing"
$includePath = Join-Path $stagingPath "include"
$libobsDestinationPath = Join-Path $includePath "libobs"

if (Test-Path -LiteralPath $stagingPath) {
    Remove-Item -LiteralPath $stagingPath -Recurse -Force
}

New-Item -ItemType Directory -Force $libobsDestinationPath | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stagingPath "lib") | Out-Null

Copy-Item -Path (Join-Path $libobsSourcePath "*") -Destination $libobsDestinationPath -Recurse -Force
Copy-Item -LiteralPath $configHeaderPath -Destination (Join-Path $libobsDestinationPath "obs-config.h") -Force
Copy-Item -LiteralPath $obsConfigHeaderPath -Destination (Join-Path $libobsDestinationPath "obsconfig.h") -Force
Copy-Item -LiteralPath $libraryPath -Destination (Join-Path $stagingPath "lib/obs.lib") -Force

foreach ($relativePath in @(
    "include/libobs/obs-module.h",
    "include/libobs/obs.h",
    "include/libobs/obs-config.h",
    "include/libobs/obsconfig.h",
    "include/libobs/graphics",
    "include/libobs/media-io",
    "include/libobs/util",
    "lib/obs.lib")) {
    if (-not (Test-Path -LiteralPath (Join-Path $stagingPath $relativePath))) {
        throw "OBS import validation failed; missing $relativePath in $stagingPath."
    }
}

if (Test-Path -LiteralPath $destinationPath) {
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
Move-Item -LiteralPath $stagingPath -Destination $destinationPath

Write-Host "OBS development files imported to $destinationPath"
Write-Host "Headers: $libobsSourcePath -> $(Join-Path $destinationPath 'include/libobs')"
Write-Host "Generated configuration: $configHeaderPath"
Write-Host "Generated build configuration: $obsConfigHeaderPath"
Write-Host "Import library: $libraryPath -> $(Join-Path $destinationPath 'lib/obs.lib')"
