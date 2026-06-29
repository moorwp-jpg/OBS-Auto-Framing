# OBS Auto Framing

`obs-auto-framing` is a native OBS Studio C++ video filter that performs a virtual crop and pan to keep people framed. It registers as an OBS video filter, exposes user settings, drives a smooth crop controller, renders through a crop shader, and can use either a mock target or ONNX Runtime CPU detection with YOLOX-Nano, YOLOX-Tiny, YOLOX-S, or a custom YOLOX-style ONNX model.

## Status

- OBS filter: `OBS_SOURCE_TYPE_FILTER` with `OBS_SOURCE_VIDEO`.
- Settings UI: user presets, detector backend, detection model, presenter/group tracking mode, framing preset, tracking speed, max zoom, subject lock, and debug overlay. Advanced contains custom model path, tracking algorithm, tracking sensitivity, detection interval, dead zone, detector score floor, NMS threshold, Simple IoU confidence, and ByteTrack thresholds.
- Detector: `PersonDetector` interface with `MockPersonDetector`, `NullPersonDetector`, and `OnnxPersonDetector`.
- ONNX Runtime CPU: YOLOX preprocessing, inference, person-best-class filtering, and single-class person NMS. YOLOX-Tiny is the recommended/default bundled model.
- Frame capture: async source frames are copied to RGBA through `filter_video`; inference runs on a worker thread and never inside `video_render`.
- Tracker: selectable ByteTrack-style tracker with low-confidence detection recovery and prediction between detector results, plus the original simple IoU tracker as a fallback.
- Crop controller: subject selection, group union, margins, aspect preservation, max zoom enforcement, source bounds clamping, dead zone, exponential smoothing, no-target return-to-full-frame.
- Renderer: OBS filter processing path using `data/effects/crop.effect`.

## Quick Install

1. Close OBS Studio.
2. Extract `obs-auto-framing-v0.1.1-windows-x64.zip` into the OBS install root, usually `C:\Program Files\obs-studio`.
3. Start OBS, select a video source, open Filters, and add the `Auto Framing` video filter.

The release zip is laid out so it can be extracted directly into an OBS install or OBS build runtime folder. The default package includes `obs-auto-framing.dll`, `onnxruntime.dll`, `crop.effect`, `en-US.ini`, and `yolox_tiny.onnx`, so the default ONNX Runtime CPU settings work without manually selecting a model path. See [docs/install.md](docs/install.md), [docs/troubleshooting.md](docs/troubleshooting.md), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for install help and bundled asset notices.

Recommended first settings:

- Detection Model: `Balanced - YOLOX-Tiny`
- Tracking Algorithm: `ByteTrack`
- Framing Preset: `Headroom` or the `Presenter Smooth` preset

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
.\scripts\build.ps1 -BuildConfig RelWithDebInfo
```

`scripts\build.ps1` runs CMake inside a clean child PowerShell with one canonical `Path` variable. This avoids the duplicate `PATH`/`Path` environment that some terminal hosts inherit and that MSBuild rejects. To build, test, and package in one clean environment, run:

```powershell
.\scripts\build.ps1 -BuildConfig RelWithDebInfo -RunTests -Package
```

`download_yolox_model.ps1` defaults to YOLOX-Tiny. Use `-Model nano`, `-Model tiny`, `-Model s`, or `-Model all` to populate `data\models` with additional choices. YOLOX-Tiny is the recommended bundled release model.

The plugin build intentionally fails early if ONNX Runtime, the default YOLOX-Tiny model, or OBS development files are missing. `onnxruntime.dll` is copied beside the plugin binary after build so OBS can load the module without a missing-DLL error.

`import_obs_dev_files.ps1` requires both an OBS source tree and its Windows build directory. It copies the `libobs` header tree to `third_party/obs/include/libobs`, its configuration headers (`obs-config.h` and generated `obsconfig.h`), and `obs.lib` to `third_party/obs/lib/obs.lib`. A regular OBS Studio installation normally does not include the import library required to build plugins.

`buildspec.json` is the shared project metadata source for the plugin name and version. CMake uses it for `PROJECT_VERSION`, and release packaging uses it for zip filenames.

Release packaging paths are documented in [docs/release_layout.md](docs/release_layout.md).

## Release Packaging

Build the plugin, then create a distribution zip:

```powershell
.\scripts\package_release.ps1
```

If the local PowerShell policy blocks unsigned scripts, run the same script with a per-process bypass:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_release.ps1
```

The script defaults to `RelWithDebInfo` when available, falls back to `Release` when no build config is supplied, validates required files, rejects unexpected files and accidental build artifacts such as `.pdb` or `.exe` files, verifies the zip layout, and writes a SHA256 checksum. Output is written to `out\release`, for example:

```text
out\release\obs-auto-framing-v0.1.1-windows-x64.zip
out\release\obs-auto-framing-v0.1.1-windows-x64.zip.sha256
```

YOLOX-Tiny is bundled by default. Add `-IncludeNano` for the lightweight fallback model or `-IncludeSmall` for YOLOX-S. YOLOX-S can be more accurate, but it is larger and slower on CPU.

Use [docs/release_checklist.md](docs/release_checklist.md) for the final v0.1.1 Preview manual QA pass.

## GitHub Release

Release zips are generated artifacts. They stay ignored by git and are attached to GitHub Releases with `scripts\publish_release.ps1` instead of being committed.

```powershell
.\scripts\publish_release.ps1
```

The default dry run validates the zip, checksum, release notes, GitHub CLI install, and GitHub authentication, then prints the `gh release create` command. To publish the prerelease after review:

```powershell
.\scripts\publish_release.ps1 -Publish
```

An installer may be added later after the preview zip install flow is validated.

To build only the core tracker and crop-controller tests without OBS development files:

```powershell
cmake --preset windows-core-tests
cmake --build --preset windows-core-tests
ctest --test-dir build_tests -C RelWithDebInfo
```

## Notes

- The ONNX backend expects YOLOX-style output with COCO class index `0` as person. It inspects every class score and accepts a detection only when the person class score is strong enough and is the best or near-best class.
- Detection Model choices are Fast - YOLOX-Nano, Balanced - YOLOX-Tiny, Accurate / Slower - YOLOX-S, and Custom ONNX. YOLOX-Tiny is the recommended default for CPU use. YOLOX-S can be more accurate, but it is slower on CPU; increase Detection Interval to around `300` ms or switch back to Tiny if OBS feels CPU-bound.
- Presets configure model, ByteTrack, tracking mode, tracking speed, detection interval, max zoom, framing preset, and tracking sensitivity. Presenter Smooth is the default. Low CPU keeps YOLOX-Tiny selected and raises Detection Interval so a Tiny-only release package remains usable.
- ByteTrack mode asks the ONNX detector for person detections down to Detector Score Floor (Advanced ByteTrack), then splits them internally into high-score and low-score associations. New Subject Confidence controls ByteTrack new track creation.
- Between detector results, ByteTrack predicts active track motion so boxes and crop movement remain responsive when inference is slower than the OBS frame rate. If detector age grows stale, prediction velocity decays and then holds the last predicted box instead of drifting.
- Subject Lock can stay off, auto-lock the first subject/group, or manually lock the current presenter/group. Clicking Lock Current Subject while lock mode is Off switches to Manual behavior and locks the current subject. Once locked, new tracks are not created and non-locked tracks are not sent to the crop controller until unlocked or reset.
- Supported capture formats include RGBA, BGRA/BGRX, BGR3, Y800, NV12, I420, YUY2, UYVY, and YVYU.
- If ONNX initialization or inference fails, detections become empty and the crop eases back to full frame.
- Use Simple IoU if you need the older tracker behavior for comparison or fallback testing.
