// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QSize>
#include <array>
#include <functional>
#include <memory>
#include <opengl/gltexture.h>
#include "../backends/nvidia/hdr_tunables.h"

namespace VfxNvidia { class Worker; }
namespace KWin {
class GLFramebuffer;
class GLShader;
// Main-thread staging slot for the optional NVIDIA worker backend.
class NvidiaSlot {
public:
    // notify is invoked from the worker thread on each state change (see Worker).
    NvidiaSlot(const QString &runtime, QSize size, unsigned mode, bool trueHdr, bool restoration, unsigned peakNits,
        std::function<void()> notify);
    ~NvidiaSlot();
    QSize size() const { return m_size; }
    unsigned mode() const { return m_mode; }
    bool hdr() const { return m_trueHdr; }
    bool matches(QSize size, unsigned mode, bool hdr, bool restoration, unsigned peak) const;
    GLTexture *result() const;
    uint64_t completedRevision() const;
    QString error() const;
    bool pending() const;
    bool needsSubmission(uint64_t revision) const;
    bool needsPublish() const;
    void stop();
    void setHdrTunables(const VfxNvidia::HdrTunables &tunables);
    bool finished() const;
    void submit(GLTexture *source, uint64_t revision, float preCasStrength = 0);
    QJsonObject debug() const;
    void observe(uint64_t captureRevision);
private:
    QSize m_size;
    unsigned m_mode;
    bool m_trueHdr = false, m_restoration = true;
    unsigned m_peakNits = 650;
    std::array<GLuint, 3> m_names{};
    std::unique_ptr<GLTexture> m_input;
    std::array<std::unique_ptr<GLTexture>, 2> m_outputs;
    std::unique_ptr<GLFramebuffer> m_framebuffer;
    std::unique_ptr<GLShader> m_copyShader;
    std::unique_ptr<VfxNvidia::Worker> m_worker;
    uint64_t m_submitted = 0;
    bool m_stopping = false;
    uint64_t m_recorded = 0;
    unsigned m_publishedIndex = 0;
    uint64_t m_publishedRevision = 0;
    QJsonArray m_timings;
};
}
