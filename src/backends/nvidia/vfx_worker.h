// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSize>
#include <QString>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include "hdr_tunables.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace VfxNvidia {
// One fixed-size slot. Main thread owns textures and publishes a GL fence after
// its final use. The worker owns CUDA registration and all VFX calls. Neither
// texture may be touched by the main thread between submit() and Ready/Idle.
class Worker
{
public:
    enum class State { Starting, Idle, Queued, Running, Ready, Failed, Stopping, Finished };
    struct Timing { double sdkCpuMs = 0; double totalMs = 0; double sdkGpuMs = -1; };
    // notify runs on the worker thread after each transition the main thread
    // acts on (Idle, Ready, Failed, finished). It must be thread-safe and cheap.
    Worker(QString root, QSize size, GLuint input, GLuint output, unsigned mode = 8, float strength = 1,
        GLuint secondOutput = 0, bool trueHdr = false, bool restoration = true, unsigned peakNits = 650,
        std::function<void()> notify = {});
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    State state() const { return m_state.load(std::memory_order_acquire); }
    bool accessible() const { const auto s = state(); return s == State::Idle || s == State::Ready; }
    // Main-thread only, after accessible(). Transfers ownership of fence.
    void submit(GLsync fence, uint64_t sequence, unsigned outputIndex = 0);
    void requestStop();
    // Any thread. Applied before the next TrueHDR run; no reload.
    void setHdrTunables(const HdrTunables &tunables) { m_hdrTunables.store(tunables.pack(), std::memory_order_relaxed); }
    bool finished() const { return m_finished.load(std::memory_order_acquire); }
    // Read only after acquiring Ready/Failed respectively, before submit().
    Timing timing() const { return m_timing; }
    uint64_t sequence() const { return m_sequence; }
    unsigned outputIndex() const { return m_outputIndex; }
    QString error() const { return m_error; }
private:
    void run();
    void notify() const { if (m_notify) m_notify(); }
    std::function<void()> m_notify;
    QString m_root;
    QSize m_size;
    GLuint m_input, m_output;
    GLuint m_secondOutput = 0;
    unsigned m_outputIndex = 0;
    unsigned m_mode;
    float m_strength;
    bool m_gpuTiming = false;
    bool m_trueHdr = false, m_restoration = true;
    unsigned m_peakNits = 650;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_share = EGL_NO_CONTEXT;
    EGLConfig m_config = nullptr;
    EGLenum m_api = EGL_OPENGL_ES_API;
    EGLint m_major = 3, m_minor = 2;
    bool m_robust = false, m_loseOnReset = false, m_resetOnPurge = false;
    std::atomic<State> m_state{State::Starting};
    std::atomic<uint32_t> m_hdrTunables{HdrTunables{}.pack()};
    std::atomic_bool m_stop{false}, m_finished{false};
    GLsync m_fence = nullptr;
    uint64_t m_sequence = 0;
    Timing m_timing;
    QString m_error;
    std::thread m_thread;
};
} // namespace VfxNvidia
