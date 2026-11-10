// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <scene/item.h>
#include <memory>

namespace KWin
{
class LogicalOutput;
class ColorDescription;

// True when the output currently presents HDR (DRM HDR, EDR or nested HDR).
bool hdrOutputReady(const LogicalOutput *output);

// True when the item tree contains sources with HDR headroom, ignoring the
// effect's own headroom item.
bool hasHdrContent(const Item *item, const Item *effectOutput = nullptr);

// KWin 6.8.1 WorkspaceScene::desiredHdrHeadroom recursively inspects item
// color descriptions. Our drawWindow replacement generates HDR although the
// application's surface stays SDR. This effect-owned item declares that output
// headroom. It has no geometry/quads and never relabels the original pixels.
class HdrHeadroomItem final : public Item
{
public:
    HdrHeadroomItem(Item *parent, const std::shared_ptr<ColorDescription> &color)
        : Item(parent) { setColorDescription(color); }
    void setColor(const std::shared_ptr<ColorDescription> &color) { setColorDescription(color); }
};
}
