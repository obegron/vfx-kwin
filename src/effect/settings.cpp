// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"
#include <KConfigGroup>
#include <algorithm>
#include <cmath>

namespace KWin
{
Settings Settings::read(const KConfigGroup &config, bool nvidiaAvailable, const Settings &previous)
{
    const auto unit = [&config](const char *key, float fallback) {
        const float value = config.readEntry(key, fallback);
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
    };
    Settings result;
    result.strength = unit("Strength", 0.6f);
    result.compressionCleanup = unit("CompressionCleanup", 0.0f);
    result.denoise = unit("Denoise", 0.5f);
    result.sharpen = unit("Sharpen", 0.35f);
    result.casStrength = unit("CasStrength", unit("NvidiaSharpen", 0.0f));
    result.grain = unit("FilmGrain", 0.0f);
    result.casOnly = config.readEntry("CasOnly", false);
    result.trueHdrRequested = nvidiaAvailable && config.readEntry("TrueHdr", false);
    result.trueHdrSdr = nvidiaAvailable && config.readEntry("TrueHdrSdr", false);
    result.hdrPeakNits = std::clamp(config.readEntry("HdrPeakNits", 650u), 400u, 2000u);
    const int mode = config.readEntry("NvidiaMode", int(previous.nvidiaMode));
    result.nvidiaMode = mode >= 8 && mode <= 15 ? unsigned(mode) : previous.nvidiaMode;
    return result;
}
}
