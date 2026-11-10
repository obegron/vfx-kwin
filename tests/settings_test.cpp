// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"
#include <KConfig>
#include <KConfigGroup>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace KWin;

static void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

int main()
{
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "Cannot create temporary directory");
        KConfig config(dir.filePath("kwinrc"), KConfig::SimpleConfig);
        KConfigGroup group(&config, QStringLiteral("Effect-kwin_vfx"));

        const Settings defaults = Settings::read(group, true, Settings{});
        require(defaults == Settings{}, "Empty group differs from defaults");

        group.writeEntry("Strength", 2.5);
        group.writeEntry("Denoise", -1.0);
        group.writeEntry("Sharpen", std::numeric_limits<double>::quiet_NaN());
        group.writeEntry("HdrPeakNits", 9000);
        group.writeEntry("NvidiaMode", 13);
        group.writeEntry("TrueHdr", true);
        group.writeEntry("TrueHdrSdr", true);
        group.writeEntry("NvidiaSharpen", 0.4);
        Settings read = Settings::read(group, false, Settings{});
        require(read.strength == 1.0f, "Strength not clamped to 1");
        require(read.denoise == 0.0f, "Denoise not clamped to 0");
        require(read.sharpen == 0.35f, "NaN sharpen did not fall back");
        require(read.hdrPeakNits == 2000, "Peak not clamped to SDK range");
        require(read.nvidiaMode == 13, "Valid mode not read");
        require(!read.trueHdrRequested && !read.trueHdrSdr, "TrueHDR enabled without NVIDIA runtime");
        require(std::abs(read.casStrength - 0.4f) < 1e-6f, "Legacy NvidiaSharpen key not honored");
        read = Settings::read(group, true, Settings{});
        require(read.trueHdrRequested && read.trueHdrSdr, "TrueHDR options ignored with NVIDIA runtime");

        group.writeEntry("HdrContrast", 500);
        group.writeEntry("HdrSaturation", -20);
        group.writeEntry("HdrMiddleGray", 2);
        group.writeEntry("HdrDebanding", false);
        read = Settings::read(group, true, Settings{});
        require(read.hdrContrast == 200 && read.hdrSaturation == 0 && read.hdrMiddleGray == 10,
            "TrueHDR tunables not clamped to SDK ranges");
        require(!read.hdrDebanding, "HdrDebanding not read");

        group.writeEntry("CasStrength", 0.7);
        group.writeEntry("NvidiaMode", 3);
        Settings previous;
        previous.nvidiaMode = 10;
        read = Settings::read(group, true, previous);
        require(std::abs(read.casStrength - 0.7f) < 1e-6f, "CasStrength does not override legacy key");
        require(read.nvidiaMode == 10, "Invalid mode did not keep previous");
        std::cout << "{\"passed\":true}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
