# OBS Runtime Test Plan

This plan validates `obs-auto-framing` inside a real OBS runtime after the CMake build succeeds.

## Install

Build the plugin first:

```powershell
.\scripts\start_clean_project_shell.ps1 -Command "cmake --build --preset windows-x64 --config RelWithDebInfo"
```

Install into an OBS runtime folder:

```powershell
.\scripts\install_to_obs.ps1 -PluginBuildDir .\build_x64\bin\RelWithDebInfo -ObsRuntimeRoot C:\src\obs-studio\build_x64\rundir\RelWithDebInfo
```

Start OBS from that runtime folder so it can find the copied plugin, data files, and DLLs.

## Test Cases

1. Plugin loading
   - Launch OBS.
   - Open the OBS log.
   - Confirm log lines for plugin load and Auto Framing filter registration.
   - Confirm there are no missing DLL, model, or `crop.effect` errors.

2. Add the filter
   - Add a webcam or video source.
   - Open Filters for the source.
   - Add `Auto Framing`.
   - Confirm the filter appears and the source still renders.

3. Mock detector mode
   - Set Detector Backend to `Mock`.
   - Enable Debug Overlay.
   - Confirm the crop visibly pans/zooms toward the moving mock target.
   - Confirm the status fields show `Running`.

4. ONNX detector mode
   - Set Detector Backend to `ONNX Runtime CPU`.
   - Leave Model Path empty to test plugin data-path fallback.
   - Confirm the log shows the resolved `yolox_nano.onnx` path, ONNX Runtime initialization success, and tensor shapes.
   - Stand in front of the webcam or play a video with people.
   - Confirm detection boxes appear in the debug overlay.

5. Presenter mode
   - Set Tracking Mode to `Presenter`.
   - Move one person across the camera.
   - Confirm the crop follows smoothly without jumping.
   - Confirm the crop does not exceed source bounds or zoom beyond Max Zoom.

6. Group mode
   - Set Tracking Mode to `Group`.
   - Put two or more people in frame.
   - Confirm the target crop expands to include all active tracks.
   - Confirm lost or weak detections do not cause immediate violent crop changes.

7. Lost-subject behavior
   - In ONNX mode, leave the frame or cover the camera.
   - Confirm detections drop to zero.
   - Confirm the crop eases back to full frame.
   - Confirm OBS does not freeze or crash.

8. Debug overlay toggle
   - Enable and disable Debug Overlay while the source is live.
   - Confirm boxes, IDs, confidence values, current crop, target crop, and dead-zone outlines disappear when disabled.

9. 10-minute stability test
   - Run ONNX mode for 10 minutes on webcam or video input.
   - Watch OBS CPU usage and frame rendering.
   - Confirm the OBS log is not flooded.
   - Confirm no crashes, stalls, or memory growth are obvious.

## Pass Criteria

- OBS launches without plugin load errors.
- `Auto Framing` appears as a video filter.
- Mock mode visibly moves the crop.
- ONNX Runtime CPU mode loads YOLOX-Nano successfully.
- Real person detections appear in the debug overlay.
- Presenter and Group modes behave differently and correctly.
- Failure cases show clear status text and log messages without crashing OBS.
