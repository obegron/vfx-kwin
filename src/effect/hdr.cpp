// SPDX-License-Identifier: GPL-2.0-or-later
#include "hdr.h"
#include "colorpolicy.h"
#include <core/backendoutput.h>
#include <core/output.h>

namespace KWin
{
bool hdrOutputReady(const LogicalOutput *output)
{
    if (!output || !output->backendOutput()->isEnabled()) return false;
    const auto *backend = output->backendOutput();
    if (backend->highDynamicRange()) return true;
    const auto &color = backend->colorDescription();
    const bool extended = color->maxHdrLuminance().value_or(color->referenceLuminance()) > color->referenceLuminance() * 1.01;
    // A nested Wayland output receives extended gamma-2.2 metadata from its
    // parent instead of exposing the DRM HDR flag. EDR uses extended range too.
    return extended && (qgetenv("KWIN_VFX_NESTED") == "1"
        || backend->capabilities().testFlag(BackendOutput::Capability::Edr));
}
bool hasHdrContent(const Item *item, const Item *effectOutput)
{
    if (item == effectOutput) return false;
    if (sourceHasHdrHeadroom(*item->colorDescription())) {
        return true;
    }
    for (Item *child : item->childItems()) {
        if (hasHdrContent(child, effectOutput)) {
            return true;
        }
    }
    return false;
}
}
