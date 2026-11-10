// SPDX-License-Identifier: GPL-2.0-or-later
#include "frame_limits.h"
#include <iostream>
#include <limits>

int main()
{
    for (const QSize size : {QSize(3840,2160), QSize(3904,2224), QSize(2160,3840), QSize(5120,2880)}) {
        try { VfxNvidia::validateFrameSize(size); }
        catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    }
    for (const QSize size : {QSize(), QSize(0,360), QSize(-1,360), QSize(8193,1), QSize(8192,8192),
                            QSize(std::numeric_limits<int>::max(),std::numeric_limits<int>::max())}) {
        bool rejected=false;
        try { VfxNvidia::validateFrameSize(size); }
        catch (const std::exception &) { rejected=true; }
        if (!rejected) { std::cerr << "Oversized or empty capture was accepted\n"; return 1; }
    }
}
