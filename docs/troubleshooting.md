# Troubleshooting

## Auto Framing Does Not Appear In OBS

Confirm the plugin DLL was installed to:

```text
<ObsRuntimeRoot>\obs-plugins\64bit\obs-auto-framing.dll
```

Confirm you launched OBS from the same runtime root you installed into. If using an OBS source build, start the OBS executable under the matching `rundir\<Config>` folder.

Open the OBS log and search for `obs-auto-framing`. You should see plugin load and filter registration messages.

## Missing onnxruntime.dll

OBS must find `onnxruntime.dll` beside the plugin DLL:

```text
<ObsRuntimeRoot>\obs-plugins\64bit\onnxruntime.dll
```

Run:

```powershell
.\scripts\install_to_obs.ps1 -PluginBuildDir .\build_x64\bin\RelWithDebInfo -ObsRuntimeRoot <ObsRuntimeRoot>
```

If OBS still reports a missing DLL, make sure you installed the same build configuration you are launching.

## Missing Model File

The plugin resolves the ONNX model in this order:

1. Explicit Model Path selected in the filter settings.
2. Plugin data path: `<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\models\yolox_nano.onnx`.
3. Development path: `data\models\yolox_nano.onnx` from the build project.

If no model exists, OBS should not crash. The filter status will show `Model missing`, and the log will include the failed model path resolution.

## crop.effect Not Loading

Confirm the effect file exists at:

```text
<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\effects\crop.effect
```

If the file is missing, rerun `scripts\install_to_obs.ps1`. If the file exists but fails to compile, the OBS log should show `crop effect messages` or `failed to load crop effect`.

## No Detections

Check these items:

- Detector Backend is set to `ONNX Runtime CPU`.
- Debug Overlay is enabled.
- The status field says `Running`.
- The log shows ONNX Runtime initialization succeeded.
- The confidence threshold is not too high. Try `0.25` to `0.35`.
- The source contains a visible full or upper body person.
- OBS is receiving a frame format supported by the capture path. Unsupported formats are logged once.

## Bad Or Offset Detections

Bad boxes usually mean a preprocessing or model-output mismatch. Confirm the log reports the expected model and tensor shape. For YOLOX-Nano at 416, the input should be shaped like `1, 3, 416, 416`. YOLOX-Nano preprocessing uses top-left letterbox placement: resized pixels start at `x=0, y=0`, with padding only on the bottom and/or right.

If boxes are consistently shifted, verify that the source is not being transformed before the filter in a way that changes the frame dimensions unexpectedly.

## OBS Is Stuttering

ONNX inference runs on a worker thread, but CPU inference can still compete with OBS for CPU time.

Try:

- Increase Detection Interval to `200` to `500` ms.
- Lower the source resolution or frame rate.
- Use a lower confidence threshold only if needed.
- Disable Debug Overlay after validation.
- Close other CPU-heavy applications.

The OBS log includes throttled detection counts and inference time. Search for `detections=` to see whether inference time is too high.

## Find And Read The OBS Log

In OBS, use:

```text
Help > Log Files > View Current Log
```

Search for:

```text
obs-auto-framing
```

Useful log lines include:

- Plugin load and filter registration.
- Detector backend selection.
- Resolved model path.
- ONNX Runtime initialization success or failure.
- Input and output tensor shapes.
- Throttled detection counts and inference time.
- Throttled crop rectangle logs when Debug Overlay is enabled.
