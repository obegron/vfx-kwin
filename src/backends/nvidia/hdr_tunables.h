// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace VfxNvidia {
// TrueHDR look parameters. The SDK re-reads them on every run, so they can
// change without reloading the model. Ranges follow the SDK documentation.
struct HdrTunables
{
    unsigned contrast = 100; // [0, 200]
    unsigned saturation = 100; // [0, 200]
    unsigned middleGray = 50; // [10, 100]
    bool debanding = true; // Learned debander; off is faster.

    // Single word so the worker can read a consistent set atomically.
    uint32_t pack() const { return contrast | saturation << 8 | middleGray << 16 | (debanding ? 1u : 0u) << 24; }
    static HdrTunables unpack(uint32_t value)
    {
        return {value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, bool((value >> 24) & 1)};
    }
    bool operator==(const HdrTunables &) const = default;
};
}
