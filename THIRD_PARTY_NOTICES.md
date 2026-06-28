# Third-Party Notices

This release package bundles third-party runtime and model assets. Keep this notice with redistributed copies of `obs-auto-framing`.

## ONNX Runtime

- Bundled file: `obs-plugins/64bit/onnxruntime.dll`
- Upstream project: https://github.com/microsoft/onnxruntime
- License: MIT
- Copyright: Microsoft Corporation
- Notes: The local build dependency package may also contain `third_party/onnxruntime/ThirdPartyNotices.txt`; consult it when updating ONNX Runtime.

## YOLOX ONNX Models

- Bundled by default: `data/obs-plugins/obs-auto-framing/models/yolox_tiny.onnx`
- Optional package flags may include: `yolox_nano.onnx`, `yolox_s.onnx`
- Upstream project: https://github.com/Megvii-BaseDetection/YOLOX
- Model download source: https://github.com/Megvii-BaseDetection/YOLOX/releases/tag/0.1.1rc0
- License: Apache-2.0
- Copyright: Megvii Inc.

YOLOX-Tiny is the recommended default CPU model for this preview release. YOLOX-Nano and YOLOX-S are optional release-package additions.
