# Install OBS Auto Framing

`obs-auto-framing` is packaged as a normal OBS plugin zip. The zip already uses the OBS directory layout, so installation is an extract-and-merge operation.

## Normal OBS Install

1. Close OBS Studio.
2. Find the OBS install root, usually `C:\Program Files\obs-studio`. This is the folder that contains `bin\64bit\obs64.exe`.
3. Extract `obs-auto-framing-v0.1.0-windows-x64.zip` into that OBS install root.
4. Start OBS.
5. Add or select a video source, open Filters, then add the `Auto Framing` video filter.

The release package installs these runtime files:

```text
obs-plugins\64bit\obs-auto-framing.dll
obs-plugins\64bit\onnxruntime.dll
data\obs-plugins\obs-auto-framing\effects\crop.effect
data\obs-plugins\obs-auto-framing\locale\en-US.ini
data\obs-plugins\obs-auto-framing\models\yolox_tiny.onnx
```

## OBS Source Or Build Runtime

For an OBS build tree, use the runtime root that contains `bin\64bit\obs64.exe`. With a standard Windows OBS build this is commonly:

```text
C:\src\obs-studio\build_x64\rundir\RelWithDebInfo
```

Extract the release zip into that runtime root. For local development, keep using:

```powershell
.\scripts\install_to_obs.ps1 -PluginBuildDir .\build_x64\bin\RelWithDebInfo -ObsRuntimeRoot <ObsRuntimeRoot>
```

`install_to_obs.ps1` is for local testing. `scripts\package_release.ps1` is for making distribution zips.

## Default Model Policy

Release packages bundle `YOLOX-Tiny` by default. It is the recommended CPU model and is selected by the default filter preset, so users do not need to choose a model path manually.

Optional models can be downloaded before packaging:

```powershell
.\scripts\download_yolox_model.ps1 -Model nano
.\scripts\download_yolox_model.ps1 -Model s
```

Then include them in a release package only when needed:

```powershell
.\scripts\package_release.ps1 -IncludeNano
.\scripts\package_release.ps1 -IncludeSmall
```

`YOLOX-Nano` is a lightweight fallback. `YOLOX-S` can be more accurate, but it is larger and slower on CPU.

## Recommended First Settings

- Detection Model: `Balanced - YOLOX-Tiny`
- Tracking Algorithm: `ByteTrack`
- Framing Preset: `Headroom` or the `Presenter Smooth` user preset

## Uninstall

Close OBS, then remove:

```text
obs-plugins\64bit\obs-auto-framing.dll
data\obs-plugins\obs-auto-framing
```

Remove `obs-plugins\64bit\onnxruntime.dll` only if no other plugin in that OBS install depends on it.

## Troubleshooting

If the plugin does not appear:

- Confirm the files were extracted into the OBS root that contains `bin\64bit\obs64.exe`, not into `bin\64bit` directly.
- Confirm OBS is the 64-bit Windows build and the plugin DLL is at `obs-plugins\64bit\obs-auto-framing.dll`.
- Confirm `onnxruntime.dll` is next to the plugin DLL at `obs-plugins\64bit\onnxruntime.dll`.
- Check `Help > Log Files > View Current Log` for messages containing `obs-auto-framing` or module load errors.
- If Windows blocked a downloaded zip, right-click the zip or DLL, open Properties, choose Unblock, then extract again.
- Install the current Microsoft Visual C++ Redistributable if the OBS log reports a missing C++ runtime DLL.

If the filter appears but detection does not run:

- Confirm `data\obs-plugins\obs-auto-framing\models\yolox_tiny.onnx` exists.
- In the filter properties, click Refresh Runtime Status and check the model path/status lines.
- Use `Balanced - YOLOX-Tiny` first. If `YOLOX-S` is selected and inference is slow, switch back to Tiny or increase Detection Interval.
