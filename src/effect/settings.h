// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class KConfigGroup;

namespace KWin
{
// User-facing configuration from the Effect-kwin_vfx group, validated once.
struct Settings
{
    float strength = 0.6f;
    float denoise = 0.5f;
    float sharpen = 0.35f;
    float compressionCleanup = 0;
    float casStrength = 0; // CAS-only strength, AI post-sharpen or TrueHDR pre-sharpen.
    float grain = 0;
    bool casOnly = false;
    bool trueHdrRequested = false; // HDR output when an HDR display is active.
    bool trueHdrSdr = false; // TrueHDR look tone-mapped for SDR displays.
    unsigned hdrPeakNits = 650;
    unsigned nvidiaMode = 8; // Same-size SDK modes: 8-11 denoise, 12-15 deblur.

    // Invalid or missing entries fall back to defaults; an invalid NvidiaMode
    // keeps previous.nvidiaMode. TrueHDR options require the NVIDIA runtime.
    static Settings read(const KConfigGroup &config, bool nvidiaAvailable, const Settings &previous);
    bool operator==(const Settings &) const = default;
};
}
