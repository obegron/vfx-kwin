// SPDX-License-Identifier: GPL-2.0-or-later
#include "colorpolicy.h"
#include <iostream>
#include <stdexcept>

using namespace KWin;

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

int main()
{
    try {
        require(!sourceHasHdrHeadroom(*ColorDescription::sRGB), "Ordinary SDR was rejected");
        // Metadata observed on Brave's SDR surface with the display in HDR.
        const ColorDescription braveSdr(Colorimetry::BT709,
            TransferFunction(TransferFunction::PerceptualQuantizer), 73, 0, 73, 73);
        require(!sourceHasHdrHeadroom(braveSdr), "SDR content encoded as PQ was rejected");
        require(sourceHasHdrHeadroom(*braveSdr.withHdrMetadata(73, 497)), "Actual PQ highlights were accepted");
        require(sourceHasHdrHeadroom(*ColorDescription::BT2020PQ), "Native PQ HDR was accepted");
        const ColorDescription unknownPq(Colorimetry::BT709,
            TransferFunction(TransferFunction::PerceptualQuantizer), 73, 0, std::nullopt, std::nullopt);
        require(sourceHasHdrHeadroom(unknownPq), "Unknown PQ content was assumed to be SDR");
        const ColorDescription unknownHlg(Colorimetry::BT2020,
            TransferFunction(TransferFunction::HLG), 203, 0, std::nullopt, std::nullopt);
        require(sourceHasHdrHeadroom(unknownHlg), "Unknown HLG content was assumed to be SDR");
        require(sourceHasHdrHeadroom(*ColorDescription::sRGB->withHdrMetadata(80, 650)),
            "Extended-range content was accepted into SDR capture");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
