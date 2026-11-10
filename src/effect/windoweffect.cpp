// SPDX-License-Identifier: GPL-2.0-or-later
// Effect lifecycle, configuration, window selection, shortcuts and D-Bus control.
#include "windowstate.h"
#include "logging.h"
#include "renderprofile.h"

#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <core/backendoutput.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <opengl/glutils.h>
#include <scene/item.h>
#include <scene/windowitem.h>

#include <KGlobalAccel>
#include <KConfigGroup>
#include <QAction>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QDBusConnection>
#include <QUuid>
#include <cmath>
#include <algorithm>

Q_LOGGING_CATEGORY(KWIN_VFX, "kwin.effect.vfx", QtWarningMsg)

namespace KWin
{
static constexpr auto controlPath = "/WindowEnhancement";
static bool selectableWindow(const EffectWindow *window)
{
    return window && !window->isDeleted() && !window->isLockScreen() && !window->isInputMethod()
        && (window->isNormalWindow() || window->isDialog() || window->isUtility());
}

WindowEnhancementEffect::WindowEnhancementEffect()
    : m_testFailure(qgetenv("KWIN_VFX_TEST_FAILURE"))
    , m_testTint(qEnvironmentVariableIsSet("KWIN_VFX_TEST_TINT"))
{
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (qgetenv("KWIN_VFX_NESTED") == "1" && !qEnvironmentVariableIsEmpty("KWIN_VFX_NVIDIA_ROOT")) {
        m_nvidiaRoot = qEnvironmentVariable("KWIN_VFX_NVIDIA_ROOT");
        if (!m_nvidiaRoot.isEmpty()) {
            bool ok = false;
            const int mode = qEnvironmentVariableIntValue("KWIN_VFX_NVIDIA_MODE", &ok);
            if (ok && mode >= 8 && mode <= 15) m_settings.nvidiaMode = unsigned(mode);
            else if (qEnvironmentVariableIsSet("KWIN_VFX_NVIDIA_MODE")) {
                m_nvidiaError = QStringLiteral("NVIDIA preview requires a same-size mode 8 through 15");
                m_nvidiaRoot.clear();
            }
        }
    } else {
        // Desktop activation is an explicit persisted opt-in; the nested
        // launcher's environment never enables CUDA in the desktop compositor.
        effects->config()->reparseConfiguration();
        const KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
        if (config.readEntry("NvidiaEnabled", false))
            m_nvidiaRoot = config.readPathEntry("NvidiaRuntime", QString());
    }
#endif
    // Fires once at the earliest resize-stability deadline of a waiting window.
    m_aiWakeTimer.setSingleShot(true);
    connect(&m_aiWakeTimer, &QTimer::timeout, this, [this] {
#ifdef KWIN_VFX_NVIDIA_BACKEND
        for (auto &[window, state] : m_windows)
            if (state->awaitingAi || state->nvidia) window->addRepaintFull();
#endif
    });
    if (qEnvironmentVariableIsSet("KWIN_VFX_PROFILE")) m_profile = std::make_unique<RenderProfile>();
    auto *action = new QAction(this);
    action->setObjectName(QStringLiteral("ToggleWindowEnhancement"));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setText(QStringLiteral("Toggle Window Enhancement"));
    action->setAutoRepeat(false);
    KGlobalAccel::self()->setGlobalShortcut(action, QKeySequence(Qt::META | Qt::CTRL | Qt::Key_E));
    connect(action, &QAction::triggered, this, &WindowEnhancementEffect::toggleFocusedWindow);
    auto strengthAction = [this](const QString &name, const QString &label, int key, float delta) {
        auto *action = new QAction(this);
        action->setObjectName(name);
        action->setProperty("componentName", QStringLiteral("kwin"));
        action->setText(label);
        KGlobalAccel::setGlobalShortcut(action, QKeySequence(Qt::META | Qt::CTRL | key));
        connect(action, &QAction::triggered, this, [this, delta] { changeStrength(delta); });
    };
    strengthAction(QStringLiteral("IncreaseWindowEnhancementStrength"), QStringLiteral("Increase Window Enhancement Strength"), Qt::Key_PageUp, 0.1f);
    strengthAction(QStringLiteral("DecreaseWindowEnhancementStrength"), QStringLiteral("Decrease Window Enhancement Strength"), Qt::Key_PageDown, -0.1f);
    connect(effects, &EffectsHandler::windowClosed, this, &WindowEnhancementEffect::disableWindow);
    connect(effects, &EffectsHandler::windowDeleted, this, &WindowEnhancementEffect::disableWindow);
    m_cleanupTimer.setInterval(16);
    connect(&m_cleanupTimer, &QTimer::timeout, this, &WindowEnhancementEffect::collectRetired);
    m_grainTimer.setInterval(42); // Approximately 24 distinct grain patterns/second.
    connect(&m_grainTimer, &QTimer::timeout, this, [this] {
        m_grainFrame = (m_grainFrame + 1) % 4096;
        for (auto &[window, state] : m_windows) window->addRepaintFull();
    });
    connect(effects, &EffectsHandler::screenLockingChanged, this, [this] { updateRedirects(); });
    for (auto *output : effects->screens()) watchOutput(output);
    connect(effects, &EffectsHandler::screenAdded, this, [this](LogicalOutput *output) {
        watchOutput(output); updateHdrAvailability();
    });
    connect(effects, &EffectsHandler::screenRemoved, this, [this] { updateHdrAvailability(); });
    reconfigure(ReconfigureAll);
    m_controlRegistered = QDBusConnection::sessionBus().registerObject(QString::fromLatin1(controlPath), this,
        QDBusConnection::ExportScriptableSlots);
    if (!m_controlRegistered) qCWarning(KWIN_VFX) << "Could not register window enhancement controls";
}

WindowEnhancementEffect::~WindowEnhancementEffect()
{
    if (m_controlRegistered) QDBusConnection::sessionBus().unregisterObject(QString::fromLatin1(controlPath));
    // KWin guarantees a current GL context on effect destruction.
    for (auto &[window, state] : m_windows) retireSlot(*state);
    m_cleanupTimer.stop();
    m_windows.clear();
    m_retired.clear();
#ifdef KWIN_VFX_NVIDIA_BACKEND
    // Module unload is an explicit one-time join: worker code must not outlive
    // its DSO. Normal disable/failure/resize only uses deferred retirement.
    m_retiredSlots.clear();
#endif
    m_shader.reset();
    m_cleanupShader.reset();
    m_processShader.reset();
    m_hdrShader.reset();
    m_profile.reset();
    effects->addRepaintFull();
}

bool WindowEnhancementEffect::supported()
{
    return effects->isOpenGLCompositing();
}

bool WindowEnhancementEffect::isActive() const
{
    return processingEnabled() && !m_windows.empty() && !effects->isScreenLocked();
}

bool WindowEnhancementEffect::processingEnabled() const
{
    return m_testTint || (m_settings.strength > 0 && (usingNvidia() || m_settings.grain > 0 || m_settings.compressionCleanup > 0
        || (m_settings.casOnly ? m_settings.casStrength > 0 : m_settings.denoise > 0 || m_settings.sharpen > 0)));
}

bool WindowEnhancementEffect::usingNvidia() const { return !m_nvidiaRoot.isEmpty() && (!m_settings.casOnly || m_trueHdr); }

void WindowEnhancementEffect::watchOutput(LogicalOutput *output)
{
    const auto changed = [this] { QTimer::singleShot(0, this, &WindowEnhancementEffect::updateHdrAvailability); };
    auto *backend = output->backendOutput();
    connect(backend, &BackendOutput::colorDescriptionChanged, this, changed);
    connect(backend, &BackendOutput::highDynamicRangeChanged, this, changed);
    connect(backend, &BackendOutput::enabledChanged, this, changed);
    connect(backend, &BackendOutput::capabilitiesChanged, this, changed);
}

void WindowEnhancementEffect::updateHdrAvailability()
{
    const auto screens = effects->screens();
    m_trueHdr = m_settings.trueHdrSdr || (m_settings.trueHdrRequested && std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    updateRedirects();
}

void WindowEnhancementEffect::updateRedirects()
{
    const bool enabled = processingEnabled();
    if (!enabled) m_aiWakeTimer.stop();
    if (enabled && m_settings.grain > 0 && !m_windows.empty() && !effects->isScreenLocked()) m_grainTimer.start();
    else m_grainTimer.stop();
    for (auto &[window, state] : m_windows) {
        const bool outputHdr = m_settings.trueHdrRequested && hdrOutputReady(window->screen());
        const bool hdrEnabled = m_settings.trueHdrSdr || outputHdr;
        if (state->hdrEnabled != hdrEnabled) {
            state->hdrEnabled = hdrEnabled;
            retireSlot(*state);
        }
        if (enabled && outputHdr && !effects->isScreenLocked()) {
            if (!state->hdrHeadroom) state->hdrHeadroom = std::make_unique<HdrHeadroomItem>(window->windowItem(), m_hdrColor);
            else state->hdrHeadroom->setColor(m_hdrColor);
        } else {
            state->hdrHeadroom.reset();
        }
        if (state->redirected != enabled) {
            state->itemEffect = enabled ? ItemEffect(window->windowItem()) : ItemEffect();
            state->redirected = enabled;
            state->dirty = true;
        }
        window->addRepaintFull();
    }
}

void WindowEnhancementEffect::reconfigure(ReconfigureFlags)
{
    effects->config()->reparseConfiguration();
    const KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
    const Settings old = m_settings;
    m_settings = Settings::read(config, !m_nvidiaRoot.isEmpty(), old);
    // Filter, grain and AI-blend settings are applied at draw time (the filter
    // cache keys on them). Only inputs baked into a capture need a recapture:
    // compression cleanup, and CAS when it pre-sharpens the TrueHDR input.
    // TrueHDR look changes apply without reloading the model, but static
    // content needs a new revision to be submitted again.
    const bool hdrLookChanged = old.hdrContrast != m_settings.hdrContrast || old.hdrSaturation != m_settings.hdrSaturation
        || old.hdrMiddleGray != m_settings.hdrMiddleGray || old.hdrDebanding != m_settings.hdrDebanding;
    const bool recapture = old.compressionCleanup * old.strength != m_settings.compressionCleanup * m_settings.strength
        || (m_trueHdr && (old.casStrength != m_settings.casStrength || hdrLookChanged));
    if (recapture)
        for (auto &[window, state] : m_windows) state->dirty = true;
    if ((m_settings.trueHdrRequested || m_settings.trueHdrSdr) && (!m_hdrColor || m_settings.hdrPeakNits != old.hdrPeakNits))
        m_hdrColor = ColorDescription::BT2020PQ->withHdrMetadata(m_settings.hdrPeakNits, m_settings.hdrPeakNits);
    // A completely disabled shader path doesn't enter paint(), so retire here
    // too when changing from an SDK pipeline to plain CAS/passthrough.
    if (!usingNvidia())
        for (auto &[window, state] : m_windows) retireSlot(*state);
    // Any settings change retries NVIDIA on windows that fell back to shaders.
    for (auto &[window, state] : m_windows) state->nvidiaFailed = false;
    updateHdrAvailability();
    qCInfo(KWIN_VFX) << "Settings: strength" << m_settings.strength << "denoise" << m_settings.denoise << "sharpen" << m_settings.sharpen;
}

void WindowEnhancementEffect::changeStrength(float delta)
{
    // Round to tenths so repeated decreases reach exact zero (true passthrough).
    m_settings.strength = std::clamp(std::round((m_settings.strength + delta) * 10.0f) / 10.0f, 0.0f, 1.0f);
    KConfigGroup config(effects->config(), QStringLiteral("Effect-kwin_vfx"));
    config.writeEntry("Strength", m_settings.strength);
    config.sync();
    if (m_settings.compressionCleanup > 0)
        for (auto &[window, state] : m_windows) state->dirty = true;
    updateRedirects();
    qCInfo(KWIN_VFX) << "Strength:" << m_settings.strength;
}

QString WindowEnhancementEffect::debug(const QString &parameter) const
{
    QJsonObject result{{"strength", m_settings.strength}, {"denoise", m_settings.denoise}, {"sharpen", m_settings.sharpen},
        {"selected", int(m_windows.size())}, {"processing", processingEnabled()}, {"test_tint", m_testTint}};
    result.insert("backend", usingNvidia() ? "nvidia-experimental" : m_settings.casOnly ? "cas" : "shader");
    result.insert("true_hdr", m_trueHdr);
    result.insert("true_hdr_requested", m_settings.trueHdrRequested);
    result.insert("true_hdr_sdr", m_settings.trueHdrSdr);
    const auto screens = effects->screens();
    result.insert("hdr_available", std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    result.insert("true_hdr_output", m_settings.trueHdrRequested && std::any_of(screens.begin(), screens.end(), hdrOutputReady));
    result.insert("hdr_capable", std::any_of(screens.begin(), screens.end(), [](const auto *screen) {
        return screen->backendOutput()->isEnabled() && (hdrOutputReady(screen)
            || screen->backendOutput()->capabilities().testFlag(BackendOutput::Capability::HighDynamicRange));
    }));
    result.insert("hdr_peak_nits", int(m_settings.hdrPeakNits));
    result.insert("hdr_contrast", int(m_settings.hdrContrast));
    result.insert("hdr_saturation", int(m_settings.hdrSaturation));
    result.insert("hdr_middle_gray", int(m_settings.hdrMiddleGray));
    result.insert("hdr_debanding", m_settings.hdrDebanding);
    result.insert("nvidia_available", !m_nvidiaRoot.isEmpty());
    result.insert("control_available", m_controlRegistered);
    result.insert("cas_only", m_settings.casOnly);
    result.insert("cas_strength", m_settings.casStrength);
    result.insert("hdr_output_seen", m_hdrOutputSeen);
    const auto colorInfo = [](const std::shared_ptr<ColorDescription> &color) {
        return QJsonObject{{"transfer", int(color->transferFunction().type)},
            {"reference_nits", color->referenceLuminance()},
            {"transfer_max_nits", color->transferFunction().maxLuminance},
            {"max_hdr_nits", color->maxHdrLuminance().value_or(color->referenceLuminance())}};
    };
    QJsonArray outputs;
    for (const auto *screen : effects->screens()) {
        outputs.append(QJsonObject{{"name", screen->name()}, {"blending", colorInfo(screen->blendingColor())},
            {"output", colorInfo(screen->backendOutput()->colorDescription())}});
    }
    result.insert("output_colors", outputs);
    if (m_lastTargetColor) result.insert("last_target_color", colorInfo(m_lastTargetColor));
    result.insert("nvidia_draws", double(m_nvidiaDraws));
    result.insert("nvidia_error", m_nvidiaError);
    result.insert("processing_error", m_processingError);
    result.insert("compression_cleanup", m_settings.compressionCleanup);
    result.insert("cleanup_draws", double(m_cleanupDraws));
    result.insert("process_draws", double(m_processDraws));
    if (parameter == QStringLiteral("windows")) {
        QJsonArray windows;
        for (const auto *window : effects->stackingOrder()) {
            if (!selectableWindow(window)) continue;
            QJsonArray items;
            const auto appendItems = [&](const auto &self, const Item *item) -> void {
                auto info = colorInfo(item->colorDescription());
                info.insert("type", QString::fromLatin1(item->metaObject()->className()));
                items.append(info);
                for (const Item *child : item->childItems()) self(self, child);
            };
            appendItems(appendItems, window->windowItem());
            windows.append(QJsonObject{{"id", window->internalId().toString(QUuid::WithoutBraces)},
                {"application", window->windowClass()}, {"items", items}});
        }
        result.insert("windows", windows);
    }
    result.insert("stale_ai_outputs", double(m_staleAiOutputs));
    result.insert("ai_backoffs", double(m_aiBackoffs));
    result.insert("nvidia_fallbacks", double(m_nvidiaFallbacks));
    result.insert("ai_slot_starts", double(m_aiSlotStarts));
#ifdef KWIN_VFX_NVIDIA_BACKEND
    result.insert("retiring_ai_slots", int(m_retiredSlots.size()));
#endif
    result.insert("nvidia_mode", int(m_settings.nvidiaMode));
    result.insert("nvidia_output_scale", 1);
    result.insert("nvidia_sharpen", m_settings.casStrength);
    result.insert("film_grain", m_settings.grain);
    result.insert("grain_frame", m_grainFrame);
    result.insert("grain_animating", m_grainTimer.isActive());
#ifdef KWIN_VFX_NVIDIA_BACKEND
    QJsonArray jobs;
    for (const auto &[window, state] : m_windows) {
        Q_UNUSED(window)
        if (state->nvidia) {
            auto job = state->nvidia->debug();
            job.insert("capture_revision", double(state->revision));
            job.insert("last_result_revision_gap", double(state->lastRevisionGap));
            job.insert("stale_streak", int(state->staleStreak));
            jobs.append(job);
        }
    }
    result.insert("nvidia_jobs", jobs);
#endif
    if (m_profile) {
        result.insert("profile", m_profile->results(parameter == QStringLiteral("reset")));
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

int WindowEnhancementEffect::requestedEffectChainPosition() const
{
    return 99;
}

bool WindowEnhancementEffect::blocksDirectScanout() const
{
    // WindowState's ItemEffect excludes only selected items from promotion.
    return false;
}

bool WindowEnhancementEffect::ensureShader()
{
    if (m_shaderFailed) {
        return false;
    }
    if (m_settings.compressionCleanup > 0 && !m_cleanupShader) {
        m_cleanupShader = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture, {}, QStringLiteral(":/kwin-vfx/compression.frag"));
        if (!m_cleanupShader) { m_shaderFailed = true; return false; }
    }
    if (!m_shader) {
        if (m_testFailure == "shader") {
            m_shader = ShaderManager::instance()->generateCustomShader(
                ShaderTrait::MapTexture, {}, QByteArrayLiteral("invalid shader for failure test"));
        } else {
            m_shader = ShaderManager::instance()->generateShaderFromFile(
                ShaderTrait::MapTexture, {}, m_testTint ? QStringLiteral(":/kwin-vfx/test.frag") : QStringLiteral(":/kwin-vfx/enhance.frag"));
        }
        if (!m_shader) {
            m_shaderFailed = true;
            qCWarning(KWIN_VFX) << "Shader failed; effect remains passthrough until reloaded";
        }
    }
    if (!m_testTint && !m_processShader) {
        m_processShader = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture, {}, QStringLiteral(":/kwin-vfx/process.frag"));
        if (!m_processShader) { m_shaderFailed = true; return false; }
    }
    if (m_trueHdr && !m_hdrShader) {
        m_hdrShader = ShaderManager::instance()->generateShaderFromFile(
            ShaderTrait::MapTexture, {}, QStringLiteral(":/kwin-vfx/enhance-hdr.frag"));
        if (!m_hdrShader) { m_shaderFailed = true; return false; }
    }
    return bool(m_shader);
}

void WindowEnhancementEffect::toggleFocusedWindow()
{
    EffectWindow *window = effects->activeWindow();
    qCInfo(KWIN_VFX) << "Toggle requested for" << window;
    if (!selectableWindow(window) || effects->isScreenLocked()) return;
    if (m_windows.contains(window)) {
        disableWindow(window);
        return;
    }
    selectWindow(window);
}

QString WindowEnhancementEffect::windowList() const
{
    QJsonArray windows;
    if (!effects->isScreenLocked()) for (auto *window : effects->stackingOrder()) {
        if (!selectableWindow(window)) continue;
        windows.append(QJsonObject{{"id", window->internalId().toString(QUuid::WithoutBraces)},
            {"title", window->caption()}, {"application", window->windowClass()},
            {"enabled", m_windows.contains(window)}, {"active", window == effects->activeWindow()},
            {"hdr_available", hdrOutputReady(window->screen())}});
    }
    return QString::fromUtf8(QJsonDocument(windows).toJson(QJsonDocument::Compact));
}

bool WindowEnhancementEffect::setWindowEnabled(const QString &id, bool enabled)
{
    const QUuid uuid(id);
    if (uuid.isNull() || effects->isScreenLocked()) return false;
    for (auto *window : effects->stackingOrder()) {
        if (window->internalId() != uuid || !selectableWindow(window)) continue;
        if (!enabled) { disableWindow(window); return true; }
        return m_windows.contains(window) || selectWindow(window);
    }
    return false; // The window may have closed since the tray was opened.
}

void WindowEnhancementEffect::disableAllWindows()
{
    while (!m_windows.empty()) disableWindow(m_windows.begin()->first);
}

bool WindowEnhancementEffect::selectWindow(EffectWindow *window)
{
    if (!selectableWindow(window) || effects->isScreenLocked()) return false;
    if (m_windows.contains(window)) return true;
    if (!effects->makeOpenGLContextCurrent() || (processingEnabled() && !ensureShader())) return false;
    collectRetired();
    auto state = std::make_unique<WindowState>();
    state->damageConnection = connect(window, &EffectWindow::windowDamaged, this, [this, window] {
        if (auto it = m_windows.find(window); it != m_windows.end()) {
            it->second->dirty = true;
        }
    });
    const auto invalidate = [this, window] {
        if (const auto it = m_windows.find(window); it != m_windows.end()) {
            it->second->dirty = true;
            it->second->itemTreeDirty = true;
        }
    };
    state->windowConnections.push_back(connect(window, &EffectWindow::windowExpandedGeometryChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowDecorationChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowFullScreenChanged, this, invalidate));
    state->windowConnections.push_back(connect(window, &EffectWindow::windowFrameGeometryChanged, this, [this] {
        QTimer::singleShot(0, this, &WindowEnhancementEffect::updateRedirects);
    }));
    m_windows.emplace(window, std::move(state));
    updateRedirects();
    qCInfo(KWIN_VFX) << "Selected window" << window;
    return true;
}

void WindowEnhancementEffect::disableWindow(EffectWindow *window)
{
    if (!m_windows.contains(window)) {
        return;
    }
    // Closed/deleted signals and shortcuts may arrive outside a paint callback.
    auto state = std::move(m_windows.at(window));
    m_windows.erase(window);
    retireSlot(*state);
    if (m_windows.empty()) {
        m_aiWakeTimer.stop();
        m_grainTimer.stop();
    }
    if (!effects->makeOpenGLContextCurrent()) {
        // Drop selection and scanout restriction even when the context is lost.
        // Retain only GPU allocations until a current context or destruction.
        QObject::disconnect(state->damageConnection);
        state->itemEffect = ItemEffect();
        m_retired.push_back(std::move(state));
        m_cleanupTimer.start();
    } else {
        state.reset();
        collectRetired();
    }
    effects->addRepaintFull();
    qCInfo(KWIN_VFX) << "Deselected window" << window;
}

bool WindowEnhancementEffect::drawWindow(const RenderTarget &target, const RenderViewport &viewport,
    EffectWindow *window, int mask, const Region &region, WindowPaintData &data)
{
    collectRetired();
    const auto it = m_windows.find(window);
    if (it == m_windows.end() || !processingEnabled()) {
        return effects->drawWindow(target, viewport, window, mask, region, data);
    }
    // Both final shaders convert into the destination encoding. The SDR
    // capture path still bypasses windows that already contain HDR pixels.
    const auto &color = target.colorDescription();
    m_lastTargetColor = color;
    const bool hdr = color->transferFunction().type == TransferFunction::PerceptualQuantizer
        || color->transferFunction().type == TransferFunction::HLG
        || color->maxHdrLuminance().value_or(color->referenceLuminance()) > color->referenceLuminance() * 1.01;
    m_hdrOutputSeen |= hdr;
    if (m_profile) m_profile->begin();
    bool success = false;
    QString failure;
    try {
        if (!ensureShader()) failure = QStringLiteral("Enhancement shader unavailable");
        else if (hasHdrContent(window->windowItem(), it->second->hdrHeadroom.get()))
            failure = QStringLiteral("Window contains native HDR content; SDR enhancement cannot process it");
        else if (!capture(*it->second, window, viewport)) failure = QStringLiteral("Window capture failed");
        else if (!paint(*it->second, target, viewport, window, region, data)) failure = QStringLiteral("Enhancement rendering failed");
        else { success = true; m_processingError.clear(); }
    } catch (const std::exception &error) {
        m_nvidiaError = QString::fromUtf8(error.what());
        failure = m_nvidiaError;
        qCWarning(KWIN_VFX) << "Enhancement exception:" << error.what();
    }
    if (m_profile) m_profile->end(it->second->size, it->second->captured, success);
    if (!success) {
        m_processingError = failure;
        qCWarning(KWIN_VFX) << failure << "; disabling" << window;
        disableWindow(window);
        return effects->drawWindow(target, viewport, window, mask, region, data);
    }
    return true;
}
}
