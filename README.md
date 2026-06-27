# OBS Auto Framing

`obs-auto-framing` is a native OBS Studio C++ video filter that performs a virtual crop and pan to keep people framed. It registers as an OBS video filter, exposes user settings, drives a smooth crop controller, renders through a crop shader, and can use either a mock target or ONNX Runtime CPU detection with YOLOX-Nano.

## Status

- OBS filter: `OBS_SOURCE_TYPE_FILTER` with `OBS_SOURCE_VIDEO`.
- Settings UI: detector backend, model path, detection confidence, NMS threshold, tracking speed, max zoom, framing preset, presenter/group mode, detection interval, dead zone, debug overlay.
- Detector: `PersonDetector` interface with `MockPersonDetector`, `NullPersonDetector`, and `OnnxPersonDetector`.
- ONNX Runtime CPU: YOLOX-Nano preprocessing, inference, person filtering, and single-class NMS.
- Frame capture: async source frames are copied to RGBA through `filter_video`; inference runs on a worker thread and never inside `video_render`.
- Tracker: simple IoU tracker with a TODO for ByteTrack replacement.
- Crop controller: subject selection, group union, margins, aspect preservation, max zoom enforcement, source bounds clamping, dead zone, exponential smoothing, no-target return-to-full-frame.
- Renderer: OBS filter processing path using `data/effects/crop.effect`.

## Build

Install Visual Studio 2026 or Build Tools for Visual Studio 2026 with the `Desktop development with C++` workload and a Windows 10/11 SDK. The `windows-x64` preset uses the `Visual Studio 18 2026` generator.

Install the OBS Studio development dependencies used by the official OBS plugin template. To open a project shell with CMake on `PATH`, run:

```powershell
.\scripts\project_shell.cmd
```

This opens a clean child PowerShell with one canonical `Path` variable. It avoids the duplicate `PATH`/`Path` environment inherited by some terminal hosts, which otherwise makes MSBuild fail before compilation starts.

`activate_project_env.ps1` detects the same condition and opens this clean shell automatically.

For a single command without opening an interactive shell, use `start_clean_project_shell.ps1 -Command`, for example:

```powershell
.\scripts\start_clean_project_shell.ps1 -Command "cmake --preset windows-x64"
```

Then download the ONNX Runtime dependencies and build OBS Studio from source. Run the OBS commands from the OBS source checkout, not this plugin directory:

```powershell
git clone --recursive --branch 32.1.2 https://github.com/obsproject/obs-studio.git C:\src\obs-studio
Set-Location C:\src\obs-studio
# Use these commands with the OBS 32.1.2 Visual Studio 2022 / Windows SDK 10.0.22621.0 toolchain:
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

With Visual Studio 2026 and a newer Windows SDK, configure `libobs` directly instead; the OBS 32.1.2 preset is pinned to the older toolchain and SDK:

```powershell
Set-Location C:\src\obs-studio
cmake -S . -B build_x64 -G "Visual Studio 18 2026" -A x64 -DENABLE_BROWSER=OFF
cmake --build build_x64 --target libobs --config RelWithDebInfo
```

Return to this plugin directory and run:

```powershell
.\scripts\download_onnxruntime.ps1
.\scripts\download_yolox_model.ps1
.\scripts\import_obs_dev_files.ps1 -ObsRoot C:\src\obs-studio -ObsBuildRoot C:\src\obs-studio\build_x64
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

If you prefer to stay in the current PowerShell window, run this instead:

```powershell
$env:Path = "C:\Program Files\CMake\bin;$env:Path"
```

The plugin build intentionally fails early if ONNX Runtime, the YOLOX model, or OBS development files are missing. `onnxruntime.dll` is copied beside the plugin binary after build so OBS can load the module without a missing-DLL error.

`import_obs_dev_files.ps1` requires both an OBS source tree and its Windows build directory. It copies the `libobs` header tree to `third_party/obs/include/libobs`, its configuration headers (`obs-config.h` and generated `obsconfig.h`), and `obs.lib` to `third_party/obs/lib/obs.lib`. A regular OBS Studio installation normally does not include the import library required to build plugins.

To build only the core crop-controller tests without OBS development files:

```powershell
cmake --preset windows-core-tests
cmake --build --preset windows-core-tests
ctest --test-dir build_tests -C RelWithDebInfo
```

## Notes

- The ONNX backend expects YOLOX-style output with COCO class index `0` as person.
- Supported capture formats include RGBA, BGRA/BGRX, BGR3, Y800, NV12, I420, YUY2, UYVY, and YVYU.
- If ONNX initialization or inference fails, detections become empty and the crop eases back to full frame.
- The next tracker upgrade is replacing `IouTracker` with a ByteTrack-style tracker.
