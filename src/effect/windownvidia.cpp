// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA slot scheduling: creation after resize stability, stale-result
// backoff, shader fallback on failure and deferred retirement.
#include "windowstate.h"
#include "logging.h"
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>

namespace KWin
{
bool WindowEnhancementEffect::usesNvidia(const WindowState &state) const
{
    return !m_nvidiaRoot.isEmpty() && !m_testTint && (!m_settings.casOnly || state.hdrEnabled) && !state.nvidiaFailed;
}

void WindowEnhancementEffect::retireSlot(WindowState &state)
{
    state.awaitingAi = false;
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (state.nvidia) {
        state.nvidia->stop();
        m_retiredSlots.push_back(std::move(state.nvidia));
        m_cleanupTimer.start();
    }
#endif
}

void WindowEnhancementEffect::collectRetired()
{
    if (m_retired.empty()
#ifdef KWIN_VFX_NVIDIA_BACKEND
        && m_retiredSlots.empty()
#endif
    ) {
        m_cleanupTimer.stop();
        return;
    }
    if (!effects->makeOpenGLContextCurrent()) return;
    m_retired.clear();
#ifdef KWIN_VFX_NVIDIA_BACKEND
    std::erase_if(m_retiredSlots, [](const auto &slot) { return slot->finished(); });
    if (!m_retiredSlots.empty()) return;
#endif
    m_cleanupTimer.stop();
}

void WindowEnhancementEffect::nvidiaProgress()
{
    m_nvidiaEventQueued.store(false);
    collectRetired();
#ifdef KWIN_VFX_NVIDIA_BACKEND
    for (auto &[window, state] : m_windows)
        if (state->nvidia || state->awaitingAi) window->addRepaintFull();
#endif
}

void WindowEnhancementEffect::scheduleAiWake(std::chrono::steady_clock::duration delay)
{
    const int ms = int(std::chrono::ceil<std::chrono::milliseconds>(delay).count()) + 1;
    if (!m_aiWakeTimer.isActive() || m_aiWakeTimer.remainingTime() > ms) m_aiWakeTimer.start(ms);
}

// Losing the AI stage keeps the window selected on the shader path; the next
// settings change clears nvidiaFailed and retries.
void WindowEnhancementEffect::nvidiaFallback(WindowState &state, const QString &error)
{
    m_nvidiaError = error;
    qCWarning(KWIN_VFX) << "NVIDIA processing failed, using shader fallback:" << error;
    state.nvidiaFailed = true;
    retireSlot(state);
    ++m_nvidiaFallbacks;
}

GLTexture *WindowEnhancementEffect::updateNvidia(WindowState &state, std::chrono::steady_clock::time_point now)
{
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (!usesNvidia(state)) {
        if (state.nvidia) retireSlot(state);
        return nullptr;
    }
    if (state.nvidia && !state.nvidia->matches(state.size, m_settings.nvidiaMode, state.hdrEnabled, !m_settings.casOnly, m_settings.hdrPeakNits)) {
        retireSlot(state);
    }
    state.awaitingAi = false;
    if (!state.nvidia) {
        if (now < state.aiStableAt || !m_retiredSlots.empty()) {
            // Retired slots notify when finished; the resize delay needs a timer.
            state.awaitingAi = true;
            if (now < state.aiStableAt) scheduleAiWake(state.aiStableAt - now);
            return nullptr;
        }
        // Worker transitions wake the compositor instead of polling.
        const auto notify = [this] {
            if (!m_nvidiaEventQueued.exchange(true))
                QMetaObject::invokeMethod(this, &WindowEnhancementEffect::nvidiaProgress, Qt::QueuedConnection);
        };
        try {
            state.nvidia = std::make_unique<NvidiaSlot>(m_nvidiaRoot, state.size, m_settings.nvidiaMode, state.hdrEnabled,
                !m_settings.casOnly, m_settings.hdrPeakNits, notify);
        } catch (const std::exception &error) {
            nvidiaFallback(state, QString::fromUtf8(error.what()));
            return nullptr;
        }
        state.staleStreak = 0;
        state.aiResumeAt = {};
        ++m_aiSlotStarts;
    }
    if (const auto error = state.nvidia->error(); !error.isEmpty()) {
        nvidiaFallback(state, error);
        return nullptr;
    }
    // Cheap atomic store; the worker applies it before its next TrueHDR run.
    if (state.hdrEnabled)
        state.nvidia->setHdrTunables({m_settings.hdrContrast, m_settings.hdrSaturation, m_settings.hdrMiddleGray, m_settings.hdrDebanding});
    state.nvidia->observe(state.revision);
    GLTexture *output = state.nvidia->result();
    if (!output) return nullptr;
    const uint64_t completed = state.nvidia->completedRevision();
    state.lastRevisionGap = state.revision - completed;
    if (state.lastRevisionGap <= 1) {
        state.staleStreak = 0;
        return output;
    }
    // A result more than one capture behind would blend separated motion frames.
    if (state.rejectedRevision != completed) {
        ++m_staleAiOutputs;
        state.rejectedRevision = completed;
        // Inference can't keep up with content updates: every result would be
        // discarded, so stop spending GPU time on it for a while. Static
        // content gets a fresh submission when the backoff expires.
        if (++state.staleStreak >= 3) {
            const auto backoff = std::chrono::milliseconds(250 << std::min(state.staleStreak - 3, 4u));
            state.aiResumeAt = now + backoff;
            scheduleAiWake(backoff);
            ++m_aiBackoffs;
        }
    }
    return nullptr;
#else
    Q_UNUSED(state)
    Q_UNUSED(now)
    return nullptr;
#endif
}

// Called after the frame's draw: the slot's GL fence then covers every use of
// the textures the worker will overwrite.
void WindowEnhancementEffect::submitNvidia(WindowState &state, std::chrono::steady_clock::time_point now)
{
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (!usesNvidia(state) || !state.nvidia || state.nvidia->size() != state.size || now < state.aiResumeAt) return;
    try {
        state.nvidia->submit(state.texture.get(), state.revision, state.hdrEnabled ? m_settings.casStrength : 0);
    } catch (const std::exception &error) {
        nvidiaFallback(state, QString::fromUtf8(error.what()));
    }
#else
    Q_UNUSED(state)
    Q_UNUSED(now)
#endif
}
}
