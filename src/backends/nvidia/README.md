# NVIDIA backend

`vfx_worker.cpp` implements asynchronous same-size denoise/deblur and optional
TrueHDR. `vfx_runtime.h` binds the native APIs from the supported
**nvidia-vfx 0.2.0.0** wheel using opaque descriptors. The effect requires neither
Python nor NVIDIA SDK headers. The runtime and models are installed separately;
see [packaging](../../../docs/packaging.md).

Each selected window has a worker with a shared EGL context, one staged input
and two alternating output textures. GL samples the published output while
CUDA owns the other. Producer/consumer fences protect reuse. CUDA map/unmap,
SDK inference and completion waits run on the worker. No frame readback occurs
in the compositor backend. Resize and disable retire workers asynchronously;
effect unloading joins them before their code can be unmapped.

The TrueHDR path converts packed RGB10A2/PQ into an RGBA16F GL texture on the GPU.
KWin performs destination color conversion and SDR tone mapping. Original
window alpha is preserved separately. SDK libraries use local symbol visibility
and remain pinned for process lifetime because CUDA/NGX can retain references.

`interop_probe.cpp` checks single-context CUDA/GL transfer, channel ordering,
alpha and orientation. `native_vfx_probe.cpp` checks inference independently of
KWin. `tests/nvidia_worker_probe.cpp` exercises the shared-context worker and
fence topology. These standalone probes may wait or read back pixels for
validation; they are not part of the compositor rendering path. Build them with
`KWIN_VFX_BUILD_NVIDIA_PROBES=ON`.

Without `KWIN_VFX_BUILD_NVIDIA_BACKEND=ON`, the effect links neither CUDA nor VFX.

The application allocation budget permits up to 16,777,216 pixels per window,
with at most 8192 pixels on either edge; actual GPU texture limits also apply.
This accommodates decorations/shadows around UHD windows and portrait layouts.
It is a memory guard, not a guarantee that every size is supported by every SDK
mode or runs within a frame deadline. SDK/allocation errors still restore normal
rendering. Larger captures increase GPU memory use and inference time.
