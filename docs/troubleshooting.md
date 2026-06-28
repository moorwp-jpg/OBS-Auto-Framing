# Troubleshooting

## Auto Framing Does Not Appear In OBS

Confirm the plugin DLL was installed to:

```text
<ObsRuntimeRoot>\obs-plugins\64bit\obs-auto-framing.dll
```

Confirm you launched OBS from the same runtime root you installed into. If using an OBS source build, start the OBS executable under the matching `rundir\<Config>` folder.

Open the OBS log and search for `obs-auto-framing`. You should see plugin load and filter registration messages, including `version 0.1.0 Preview`.

## Missing onnxruntime.dll

OBS must find `onnxruntime.dll` beside the plugin DLL:

```text
<ObsRuntimeRoot>\obs-plugins\64bit\onnxruntime.dll
```

For a release install, close OBS and extract the release zip into the OBS root again. The zip includes `onnxruntime.dll`.

For a local development install, run:

```powershell
.\scripts\install_to_obs.ps1 -PluginBuildDir .\build_x64\bin\RelWithDebInfo -ObsRuntimeRoot <ObsRuntimeRoot>
```

If OBS still reports a missing DLL, make sure you installed into the same OBS root you are launching.

## Missing Model File

For bundled model qualities, the plugin resolves the selected model in this order:

1. Plugin data path: `<ObsRuntimeRoot>\data\obs-plugins\obs-auto-framing\models\<model file>`.
2. Development path: `data\models\<model file>` from the build project.

The bundled model filenames are:

- Fast - YOLOX-Nano: `yolox_nano.onnx`
- Balanced - YOLOX-Tiny: `yolox_tiny.onnx`
- Accurate / Slower - YOLOX-S: `yolox_s.onnx`

Custom ONNX uses only the Custom Model Path selected in the filter settings.

If no model exists, OBS should not crash. The filter status will show `Model missing`, and the log will include the failed model path resolution. A normal v0.1.0 Preview release zip should include `yolox_tiny.onnx`.

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
- The log shows ONNX Runtime initialization succeeded and the resolved `yolox_tiny.onnx` path.
- Detection Model points to an installed model. `Balanced - YOLOX-Tiny` is the default and recommended release model.
- In Simple IoU mode, the Simple IoU Detection Confidence threshold is not too high. Try `0.25` to `0.35`.
- In ByteTrack mode, Detector Score Floor (Advanced ByteTrack) controls how low the ONNX detector output can go before the tracker sees it. Try `0.05` to `0.10` if weak boxes are missing.
- The source contains a visible full or upper body person.
- OBS is receiving a frame format supported by the capture path. Unsupported formats are logged once.

## False Detections On Chairs, Bottles, Or Background Objects

The ONNX postprocessor now checks all YOLOX class scores before creating a person detection. It requires the person class score to be strong enough and to be the best or near-best class, then still applies the detector score floor and person-only NMS.

If false detections persist:

- Use `Balanced - YOLOX-Tiny` or `Accurate / Slower - YOLOX-S` instead of Nano.
- Raise New Subject Confidence so ByteTrack is more selective when creating new tracks.
- Raise Detector Score Floor (Advanced ByteTrack) slightly if weak boxes are entering the tracker.
- Use Subject Lock once the intended presenter or group is framed.

## Track IDs Change Too Often

Try these items:

- Set Tracking Algorithm to `ByteTrack`.
- Use Tracking Sensitivity `Balanced` first, then `Persistent` if people are briefly occluded or detections flicker.
- Lower Detector Score Floor (Advanced ByteTrack) so ByteTrack can use low-confidence detections for recovery.
- Avoid setting ByteTrack High Threshold, Low Threshold, New Track Threshold, Match Threshold, or Track Buffer Frames unless you need manual tuning. A value of `0` uses the selected sensitivity preset.
- If a new person is incorrectly attached to an existing ID, switch to `Conservative` or raise ByteTrack Match Threshold slightly.

## Subject Lock Does Not Behave As Expected

- `Off` disables lock behavior.
- `Auto-lock First Subject` waits until the first active track appears, then locks the presenter track or current group depending on Tracking Mode.
- `Manual` waits for you to click `Lock Current Subject`.
- In Presenter mode, manual lock stores the current dominant presenter track ID.
- In Group mode, manual lock stores all current active track IDs.
- `Unlock Subject` clears the locked IDs. In Auto-lock mode, it waits for the frame to become empty before auto-locking again.

Subject Lock resets when the detector backend, selected model, or tracking algorithm changes. Once locked, new tracks are not created and non-locked tracks are not sent to the crop controller.

## Lost Tracks Stay Too Long Or Vanish Too Quickly

ByteTrack keeps unmatched tracks in a lost state for Track Buffer Frames. If boxes linger after a person leaves, lower the buffer or use `Conservative`. If IDs are lost during brief occlusions, use `Persistent` or increase Track Buffer Frames.

The debug overlay shows active and lost counts plus per-track state labels: `NEW`, `TRK`, and `LOST`.

## Bad Or Offset Detections

Bad boxes usually mean a preprocessing or model-output mismatch. Confirm the log reports the expected model and tensor shape. For the bundled YOLOX models at 416, the input should be shaped like `1, 3, 416, 416`. Preprocessing uses top-left letterbox placement: resized pixels start at `x=0, y=0`, with padding only on the bottom and/or right.

If boxes are consistently shifted, verify that the source is not being transformed before the filter in a way that changes the frame dimensions unexpectedly.

## OBS Is Stuttering

ONNX inference runs on a worker thread, but CPU inference can still compete with OBS for CPU time.

Try:

- Use `Balanced - YOLOX-Tiny`, the recommended default for CPU tracking.
- If you choose `Accurate / Slower - YOLOX-S`, increase Detection Interval to around `300` ms because it is more accurate but slower on CPU.
- Increase Detection Interval to `200` to `500` ms.
- Lower the source resolution or frame rate.
- Use a lower confidence threshold only if needed.
- Disable Debug Overlay after validation.
- Close other CPU-heavy applications.

The filter status shows Detection age, Tracker prediction, and Performance. If YOLOX-S inference is slow, Performance recommends switching to YOLOX-Tiny or increasing Detection Interval. The OBS log also includes throttled detection counts, inference time, and stale detection-age messages. Search for `detections=` to see whether inference time is too high.

## YOLOX-S Tracking Boxes Lag

`Accurate / Slower - YOLOX-S` is more accurate than Tiny in some scenes, but CPU inference can be slow enough that detector results arrive below the OBS frame rate. If YOLOX-S tracking boxes lag, use `Balanced - YOLOX-Tiny`, increase Detection Interval to about `300` ms so prediction can carry the crop between results, or enable a future GPU backend when one is available.

## Detection Age Keeps Growing

Detection age is the time since the last detector result was accepted by the tracker. While the detector worker is still running, ByteTrack prediction keeps active tracks alive and does not mark them lost just because a new result is late. If detection age becomes stale, prediction velocity decays and then holds the last predicted box to avoid drift.

If this happens often:

- Use the `Presenter Smooth`, `Group`, or `Low CPU` preset.
- Use `Balanced - YOLOX-Tiny` instead of YOLOX-S.
- Increase Detection Interval to `200` to `500` ms.
- Lower source resolution or frame rate.

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

- Plugin load with version `0.1.0 Preview` and filter registration.
- Detector backend selection.
- Resolved model path.
- ONNX Runtime initialization success or failure.
- Input and output tensor shapes.
- Throttled detection counts and inference time.
- Throttled crop rectangle logs when Debug Overlay is enabled.
