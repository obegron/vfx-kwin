// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "windoweffect.h"
#include "hdr.h"
#ifdef KWIN_VFX_NVIDIA_BACKEND
#include "nvidiaslot.h"
#endif
#include <opengl/glutils.h>
#include <scene/item.h>
#include <QSize>
#include <chrono>
#include <memory>
#include <vector>

namespace KWin
{
// Unlike OffscreenEffect, this small capture path owns its FBO and propagates
// allocation/capture/vertex-map failures so drawWindow can retry normal rendering.
struct WindowEnhancementEffect::WindowState
{
    // Reverse destruction order keeps the texture alive until its FBO is gone.
    std::unique_ptr<GLTexture> texture;
    std::unique_ptr<GLFramebuffer> framebuffer;
    std::unique_ptr<GLTexture> cleanupTexture;
    std::unique_ptr<GLFramebuffer> cleanupFramebuffer;
    // Cached filter output, redrawn only when its inputs change.
    std::unique_ptr<GLTexture> processed;
    std::unique_ptr<GLFramebuffer> processedFramebuffer;
    struct ProcessKey {
        uint64_t revision = 0, aiRevision = 0;
        int mode = -1;
        float denoise = 0, sharpen = 0, cas = 0, strength = 0;
        bool operator==(const ProcessKey &) const = default;
    } processedKey;
    ItemEffect itemEffect;
    std::unique_ptr<HdrHeadroomItem> hdrHeadroom;
    QMetaObject::Connection damageConnection;
    std::vector<QMetaObject::Connection> itemConnections;
    std::vector<QMetaObject::Connection> windowConnections;
    QSize size;
    qreal scale = 0;
    bool dirty = true;
    bool redirected = false;
    bool captured = false;
    uint64_t revision = 0;
    uint64_t rejectedRevision = 0;
    uint64_t lastRevisionGap = 0;
    bool itemTreeDirty = true;
    bool awaitingAi = false;
    // An NVIDIA failure keeps the selection on the shader path until settings change.
    bool nvidiaFailed = false;
    // Consecutive AI results discarded as stale; drives submission backoff.
    unsigned staleStreak = 0;
    std::chrono::steady_clock::time_point aiResumeAt;
    bool hdrEnabled = false;
    std::chrono::steady_clock::time_point aiStableAt;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    std::unique_ptr<NvidiaSlot> nvidia;
#endif

    ~WindowState() {
        QObject::disconnect(damageConnection);
        for (const auto &connection : itemConnections) QObject::disconnect(connection);
        for (const auto &connection : windowConnections) QObject::disconnect(connection);
    }
};
}
