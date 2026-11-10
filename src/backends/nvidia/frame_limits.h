// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSize>
#include <QString>
#include <cstdint>
#include <stdexcept>

namespace VfxNvidia {
// Application allocation budget, independent of the SDK and GL device limits.
inline constexpr int MaximumFrameDimension = 8192;
inline constexpr uint64_t MaximumFramePixels = 16 * 1024 * 1024;

inline void validateFrameSize(QSize size)
{
    if (size.width() <= 0 || size.height() <= 0
        || size.width() > MaximumFrameDimension || size.height() > MaximumFrameDimension
        || uint64_t(size.width()) * uint64_t(size.height()) > MaximumFramePixels) {
        throw std::runtime_error(QStringLiteral("NVIDIA capture %1x%2 exceeds the allocation budget "
                                               "(%3 pixels per edge, %4 pixels total)")
                                     .arg(size.width()).arg(size.height()).arg(MaximumFrameDimension)
                                     .arg(qulonglong(MaximumFramePixels)).toStdString());
    }
}
}
