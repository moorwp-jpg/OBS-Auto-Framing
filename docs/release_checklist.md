# v0.1.1 Preview Release Checklist

Use this checklist against the exact zip intended for release.

## Package

- [ ] Build `RelWithDebInfo` or `Release`.
- [ ] Run `scripts\package_release.ps1`.
- [ ] Confirm the zip name is `obs-auto-framing-v0.1.1-windows-x64.zip`.
- [ ] Confirm the zip contains `obs-plugins\64bit\obs-auto-framing.dll`.
- [ ] Confirm the zip contains `obs-plugins\64bit\onnxruntime.dll`.
- [ ] Confirm the zip contains `data\obs-plugins\obs-auto-framing\effects\crop.effect`.
- [ ] Confirm the zip contains `data\obs-plugins\obs-auto-framing\locale\en-US.ini`.
- [ ] Confirm the zip contains `data\obs-plugins\obs-auto-framing\models\yolox_tiny.onnx`.
- [ ] Confirm `yolox_nano.onnx` and `yolox_s.onnx` are absent unless intentionally packaged with `-IncludeNano` or `-IncludeSmall`.
- [ ] Confirm the zip does not contain build directories, `third_party`, `.git`, `out`, PDBs, test executables, or generated staging files.
- [ ] Confirm `THIRD_PARTY_NOTICES.md` is included.
- [ ] Record the SHA256 checksum.

## GitHub Release

- [ ] Confirm the package zip was created in `out\release`.
- [ ] Confirm the `.sha256` checksum was created in `out\release`.
- [ ] Review `docs\release_notes\v0.1.1.md`.
- [ ] Run `scripts\publish_release.ps1` and inspect the dry-run `gh release create` command.
- [ ] Run `scripts\publish_release.ps1 -Publish`.
- [ ] Confirm the GitHub Release is marked as a prerelease.
- [ ] Confirm the zip and checksum are attached as release assets.
- [ ] Download the release zip from GitHub and confirm it installs successfully by extraction.
- [ ] Do not commit the generated zip or checksum; they are release assets only.

## Install Smoke Test

- [ ] Extract the zip into a clean OBS install or OBS runtime root.
- [ ] Start OBS from that same runtime root.
- [ ] Confirm the OBS log shows `obs-auto-framing` loaded with version `0.1.1 Preview`.
- [ ] Confirm `Auto Framing` appears as a video filter.
- [ ] Add the filter with default settings and leave Custom Model Path empty.
- [ ] Confirm runtime status shows plugin version, ONNX Runtime CPU, `Balanced - YOLOX-Tiny`, model loaded, and the bundled model path.
- [ ] Confirm the OBS log shows the resolved `yolox_tiny.onnx` path and ONNX Runtime initialization success.

## Manual QA

- [ ] Presenter Smooth follows one moving presenter smoothly.
- [ ] Group mode expands the crop to keep multiple people framed.
- [ ] Subject Lock keeps the selected presenter or group locked while new detections enter.
- [ ] Tight framing creates the expected head-and-shoulders crop without cutting off the face.
- [ ] Soft dead-zone behavior avoids jitter during small movements.
- [ ] Debug Overlay shows detections, track IDs/states, crop bounds, dead-zone, and status summaries.
- [ ] Debug Overlay can be disabled without changing tracking behavior.
- [ ] YOLOX-S, if intentionally bundled, loads and reports slower CPU performance guidance when inference is slow.
- [ ] Run a 10-minute stability test with ONNX Runtime CPU, `Balanced - YOLOX-Tiny`, ByteTrack, and Presenter Smooth.
- [ ] During the stability test, confirm OBS does not freeze, logs are not flooded, and the crop returns to full frame when no target is present.
