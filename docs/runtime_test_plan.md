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
   - Set Detection Model to `Balanced - YOLOX-Tiny`.
   - Leave Custom Model Path empty to test plugin data-path fallback.
   - Confirm the log shows the resolved `yolox_tiny.onnx` path, ONNX Runtime initialization success, and tensor shapes.
   - Stand in front of the webcam or play a video with people.
   - Confirm detection boxes appear in the debug overlay.

5. Presenter mode
   - Set Tracking Algorithm to `ByteTrack`.
   - Set Tracking Sensitivity to `Balanced`.
   - Set Tracking Mode to `Presenter`.
   - Move one person across the camera.
   - Confirm the crop follows smoothly without jumping.
   - Confirm the debug overlay shows the tracker algorithm, track ID, track state, and confidence.
   - Confirm the crop does not exceed source bounds or zoom beyond Max Zoom.

6. Group mode
   - Set Tracking Mode to `Group`.
   - Put two or more people in frame.
   - Confirm the target crop expands to include all active tracks.
   - Confirm lost or weak detections do not cause immediate violent crop changes.

7. ByteTrack low-confidence recovery
   - Use ONNX detector mode with Debug Overlay enabled.
   - Lower Detector Score Floor (Advanced ByteTrack) below the ByteTrack Low Threshold preset, or leave both at their defaults.
   - Partially occlude the person or move through poor lighting until confidence visibly dips.
   - Confirm a briefly low-confidence detection keeps the same track ID instead of creating a new one.
   - Confirm the status fields show active and lost track counts.

8. Model quality switching
   - Switch Detection Model between `Fast - YOLOX-Nano`, `Balanced - YOLOX-Tiny`, and `Accurate / Slower - YOLOX-S` if those files are installed.
   - Confirm the runtime Detection model and Model path fields update after Refresh Runtime Status.
   - Set Detection Model to `Custom ONNX`, select a valid model path, and confirm it loads.
   - Confirm OBS does not crash if a selected model file is missing; status should show `Model missing`.

9. Subject lock in Presenter mode
   - Set Tracking Mode to `Presenter`.
   - Set Subject Lock Mode to `Off`, stand one person in frame, then click `Lock Current Subject`.
   - Confirm the runtime status shows Manual-style subject lock behavior and lists the locked track ID.
   - Click `Unlock Subject`.
   - Set Subject Lock Mode to `Manual`.
   - Stand one person in frame, wait for a stable track ID, then click `Lock Current Subject`.
   - Add another person or move an object into frame.
   - Confirm the crop remains on the locked track ID and new detections do not become crop targets.
   - Click `Unlock Subject` and confirm new subjects can become targets again.

10. Auto-lock first subject
   - Set Subject Lock Mode to `Auto-lock First Subject`.
   - Start with an empty frame, then have one person enter.
   - Confirm the runtime status changes from waiting to locked and the crop stays on that first presenter even if another person enters.
   - Click `Unlock Subject`; confirm it does not immediately relock until the frame has gone empty and a subject appears again.

11. Group lock
   - Set Tracking Mode to `Group`.
   - Put two or more people in frame.
   - Set Subject Lock Mode to `Manual` and click `Lock Current Subject`.
   - Confirm the runtime status lists multiple locked IDs and the crop tracks only that group.
   - Add another person and confirm they are ignored until unlock.

12. Tracker switching
   - Switch Tracking Algorithm between `ByteTrack` and `Simple IoU` while the source is live.
   - Confirm OBS does not crash or freeze.
   - Confirm track IDs reset cleanly and the runtime Tracker field updates after reopening the properties window.
   - Confirm Subject Lock resets when the tracking algorithm changes.

13. Lost-subject behavior
   - In ONNX mode, leave the frame or cover the camera.
   - Confirm detections drop to zero.
   - Confirm ByteTrack lost count rises briefly and then returns to zero after the configured buffer.
   - Confirm the crop eases back to full frame.
   - Confirm OBS does not freeze or crash.

14. Debug overlay toggle
   - Enable and disable Debug Overlay while the source is live.
   - Confirm boxes, IDs, states, confidence values, tracker summary, subject-lock status, current crop, target crop, and dead-zone outlines disappear when disabled.

15. 10-minute stability test
   - Run ONNX mode for 10 minutes on webcam or video input.
   - Watch OBS CPU usage and frame rendering.
   - Confirm the OBS log is not flooded.
   - Confirm no crashes, stalls, or memory growth are obvious.

## Pass Criteria

- OBS launches without plugin load errors.
- `Auto Framing` appears as a video filter.
- Mock mode visibly moves the crop.
- ONNX Runtime CPU mode loads YOLOX-Tiny successfully by default.
- Real person detections appear in the debug overlay.
- Presenter and Group modes behave differently and correctly.
- Subject Lock prevents new detections from becoming crop targets after lock.
- Failure cases show clear status text and log messages without crashing OBS.
