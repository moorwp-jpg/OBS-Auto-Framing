# Release Layout

Use this layout for a Windows OBS runtime package. `<ObsRuntimeRoot>` is the OBS install or build runtime directory that contains `bin\64bit\obs64.exe`.

## Required Runtime Files

```text
<ObsRuntimeRoot>\obs-plugins\64bit\obs-auto-framing.dll
<ObsRuntimeRoot>\obs-plugins\64bit\onnxruntime.dll
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\effects\crop.effect
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\locale\en-US.ini
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\models\yolox_tiny.onnx
<ObsRuntimeRoot>\THIRD_PARTY_NOTICES.md
```

`yolox_tiny.onnx` is the recommended bundled model. The default preset uses ONNX Runtime CPU, YOLOX-Tiny, and ByteTrack.

## Optional Model Files

These files may be included for extra model choices:

```text
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\models\yolox_nano.onnx
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\models\yolox_s.onnx
```

YOLOX-Nano is fastest but less stable in harder scenes. YOLOX-S is more accurate but slower on CPU; users should switch back to YOLOX-Tiny or increase Detection Interval if status shows slow inference.

## Install Script

After building the plugin, this script installs the DLL, ONNX Runtime DLL, effects, locale, and all `.onnx` files currently present in `data\models` for local testing:

```powershell
.\scripts\install_to_obs.ps1 -PluginBuildDir .\build_x64\bin\RelWithDebInfo -ObsRuntimeRoot <ObsRuntimeRoot>
```

For the recommended release package, ensure `data\models\yolox_tiny.onnx` exists before running the script:

```powershell
.\scripts\download_yolox_model.ps1 -Model tiny
```

## Release Package Script

Use `scripts\package_release.ps1` for distribution packages:

```powershell
.\scripts\package_release.ps1
```

If PowerShell blocks unsigned local scripts, run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_release.ps1
```

The script reads the package version from `buildspec.json` and creates:

```text
out\release\obs-auto-framing-v0.1.0-windows-x64.zip
out\release\obs-auto-framing-v0.1.0-windows-x64.zip.sha256
```

By default it bundles only `yolox_tiny.onnx`. Use `-IncludeNano` to add `yolox_nano.onnx`, or `-IncludeSmall` to add `yolox_s.onnx`. It validates required runtime files, rejects unexpected files, blocked directories, and accidental build artifacts such as `.pdb`, `.exe`, `.lib`, and `.obj`, and verifies the zip contains the expected OBS layout before reporting success.
