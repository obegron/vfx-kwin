# Source layout

- `effect/`: the KWin effect.
  - `windoweffect.cpp`: lifecycle, configuration, selection, shortcuts and
    D-Bus controls.
  - `windowrender.cpp`: per-window capture, cached filter passes and the final
    color-managed draw. `renderpass.h` holds the shared offscreen-pass helper.
  - `windownvidia.cpp`: AI slot scheduling, stale-result backoff, shader
    fallback and deferred worker retirement. `nvidiaslot.cpp` stages GL input
    for one worker.
  - `settings.cpp`: validated configuration (`Settings`); `hdr.cpp` and
    `colorpolicy.h`: HDR output detection and native-HDR source policy.
  - `windowstate.h`: per-window state shared by the files above.
- `backends/shader/`: spatial denoise/sharpen and AMD CAS GLSL, shared with the
  standalone shader correctness and benchmark harness.
- `backends/nvidia/`: optional native CUDA/EGL worker, runtime bindings and
  standalone interop/inference probes. No proprietary libraries are vendored.
- `preview/`: Qt system tray controls for the desktop or a private test session.

The effect owns its captured textures. Shader filtering runs once per content
or settings change into a cached texture; each repaint only adds grain and color
conversion. NVIDIA inference runs on a separate worker with explicit GL/CUDA
ownership transfers. See [architecture](../docs/architecture.md) and
[validation](../docs/validation.md).
