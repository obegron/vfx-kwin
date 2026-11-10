// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <core/colorspace.h>

namespace KWin
{
inline bool sourceHasHdrHeadroom(const ColorDescription &color)
{
    // Chromium can encode SDR surfaces as PQ on HDR outputs. The transfer
    // function describes the encoding, not the brightness of the content.
    if (const auto peak = color.maxHdrLuminance()) {
        return *peak > color.referenceLuminance() * 1.01;
    }
    // Without content metadata, keep unknown PQ/HLG sources out of the
    // SDR capture path rather than assuming their highlights are SDR.
    const auto transfer = color.transferFunction().type;
    return transfer == TransferFunction::PerceptualQuantizer || transfer == TransferFunction::HLG;
}
}
