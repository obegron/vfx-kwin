# Compression restoration investigation

Investigated 2026-10-26. This is research and runtime inspection, not a new
restoration backend. The working effect and user settings were retained.

## What the installed NVIDIA wheel actually contains

Inspected the installed `nvidia-vfx 0.2.0.0` wheel with `readelf`, demangled
dynamic symbols from `nm -D -C`, and bounded `strings` searches. Then ran an
independent CUDA process using the existing runtime loader, `NvVFX_CreateEffect`
and `NvVFX_GetString(effect, "Info", ...)`. No effect was loaded into KWin by
this probe, and no new restoration model was run.

| Selector | Create status | Result |
| --- | --- | --- |
| `ArtifactReduction` | -2 | The requested feature is not yet implemented |
| `VideoSuperRes` | 0 | Info reports VideoSuperRes 1.3.0.0 |
| `TrueHDR` | 0 | Info reports TrueHDR 1.3.0.0 |
| `SuperRes`, `Upscale`, `Denoising` | -2 | These separate selectors are not implemented in this wheel |

The `VideoSuperRes` Info string advertises VSR 0–4, denoise 8–11, deblur 12–15,
high-bitrate 16–19 and streaming 21/23. The existing compositor path uses
same-size denoise/deblur. The presence of a mode in Info is not proof of its
inference speed or quality; upscaling modes have not been benchmarked here.

The TrueHDR library contains `Flatten_DLDebander`, architecture-specific
`Flatten_DLDebander_86` and related model names, plus `TrueHDR.DebandingOff`.
Its public Info states that `DebandingOff=1` skips the deep-learning debander;
the default is zero. `vfx_worker.cpp` already sets zero whenever TrueHDR runs.
Thus there is already learned debanding in the TrueHDR path. It is not exposed
as an independent deblocking effect.

NVIDIA's [TrueHDR documentation](https://docs.nvidia.com/maxine/vfx/latest/Filters/TrueHDR.html)
describes this stage as addressing contouring in 8-bit-to-HDR conversion.
It does not establish that it repairs severe codec macroblocks. The historical
[Artifact Reduction filter](https://docs.nvidia.com/maxine/vfx/1.0.0/Filters/ArtifactReduction.html)
does target blocking and ringing, but its documented name is rejected by our
installed runtime.

## Most relevant NVIDIA direction

The current [VSR documentation](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoSuperResolution.html)
specifically describes modes 1–4 as reducing compression artifacts while
upscaling. It warns that deblur can amplify artifacts on heavily compressed
inputs and that denoise is not intended for structured blocking/banding.
The Python wrapper's descriptions are less restrictive, so visual tests should
decide rather than treating the wrapper text as a quality guarantee.

Our input is the composed window, after the browser may already have enlarged
a 720p/1080p source to a 4K display. An inference from this architecture is that
codec blocks may arrive larger than the model expects. Testing source-scale
video or a known video crop is more informative than blindly stacking another
model over the entire desktop-sized window. Downsampling an entire window and
upscaling it again can also sacrifice real detail and application text; it is
not a default recommendation.

## Hugging Face candidates

| Candidate | Why it is relevant | Limits for this project |
| --- | --- | --- |
| [Remaster DRUNet student](https://huggingface.co/seantempesta/remaster-drunet) | Same-resolution compression restoration; about 1.06M parameters, 4 MB weights, MIT model card, ONNX/TensorRT path | Promising first external candidate, not locally tested. Author reports roughly 53–57 fps at 1080p on an RTX 3060 laptop; project README reports 13 fps at 4K. SDR pipeline, dark-scene brightness shifts and motion consistency require checking. |
| [userDevice RTUS](https://huggingface.co/ltomes/userdevice-rtus) | Trained on actual H.264-degraded inputs; publishes parity and temporal checks | A 2x upscaler rather than a same-size drop-in. Author reports 28.742 ms compute-only for 1080p to 4K on Jetson Thor, not this RTX 3080 laptop. Fixed exported shapes; CC BY-SA weights. |
| [SCUNet ONNX](https://huggingface.co/Heliosoph/scunet-onnx) | Available restoration/denoising baseline and relevant training lineage for Remaster | General image denoising, not demonstrated here as a real-time video deblocker. GAN variant can invent texture; frame-to-frame behavior needs testing. |
| [LTX decompression LoRA](https://huggingface.co/Lightricks/LTX-2.3-22b-IC-LoRA-Decompression) | Explicitly targets severe compression damage | A 22B video diffusion base plus conditioning pipeline. No demonstrated suitability for this compositor's real-time budget; not a first candidate. |

These are publisher measurements, not local benchmarks. No weights or new
inference dependencies were installed. The [Remaster project](https://github.com/seantempesta/remaster)
also notes SDR-only processing, possible dark-scene brightness changes and
limits imposed by information lost during compression. Its training targets
include denoising and sharpening, so it should be compared both as a replacement
for deblur and before deblur, rather than assuming that stacking is beneficial.

## Decision

Keep the existing pipeline. There is no verified real-time, full-window 4K
replacement established by this research. The next useful isolated experiment
would compare NVIDIA VSR at a known source scale, then the small Remaster student,
on the same severely compressed and reasonably clean video clips.

Judge visible block reduction, retained hair/text detail, brightness, temporal
flicker and end-to-end frame time. A still-frame improvement alone is not enough
to justify adding a model to KWin. Preserve an original/result blend and separate
enable control if a candidate earns integration.
