// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
// Per-window capture, cached filter passes and the final color-managed draw.
// Capture/drawing patterns adapted from KWin 6.8.1 OffscreenEffect.
#include "windowstate.h"
#include "renderpass.h"
#include <core/output.h>
#include <core/pixelgrid.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <scene/itemgeometry.h>
#include <scene/surfaceitem.h>
#include <scene/windowitem.h>
#include <kdecoration3/decoration.h>
#include <QScopeGuard>
#include <cmath>

namespace KWin
{
void WindowEnhancementEffect::watchItems(EffectWindow *window, WindowState &state, Item *item, bool root)
{
    const auto invalidate = [this, window] {
        if (const auto it = m_windows.find(window); it != m_windows.end()) it->second->dirty = true;
    };
    const auto structure = [this, window, invalidate] {
        invalidate();
        if (const auto it = m_windows.find(window); it != m_windows.end()) it->second->itemTreeDirty = true;
    };
    state.itemConnections.push_back(connect(item, &Item::childAdded, this, structure));
    state.itemConnections.push_back(connect(item, &Item::childRemoved, this, structure));
    state.itemConnections.push_back(connect(item, &Item::visibleChanged, this, invalidate));
    state.itemConnections.push_back(connect(item, &Item::sizeChanged, this, invalidate));
    state.itemConnections.push_back(connect(item, &Item::boundingRectChanged, this, invalidate));
    if (!root) state.itemConnections.push_back(connect(item, &Item::positionChanged, this, invalidate));
    if (auto *surface = qobject_cast<SurfaceItem *>(item))
        state.itemConnections.push_back(connect(surface, &SurfaceItem::damaged, this, invalidate));
    if (root && window->decoration()) {
        state.itemConnections.push_back(connect(window->decoration(), &KDecoration3::Decoration::damaged, this, invalidate));
        state.itemConnections.push_back(connect(window->decoration(), &KDecoration3::Decoration::shadowChanged, this, structure));
    }
    for (Item *child : item->childItems()) watchItems(window, state, child, false);
}

// Capture once at the densest output the window touches. Using each draw's
// viewport scale would reallocate and recapture on every frame for a window
// spanning outputs with different scales (and keep the AI slot from starting).
static qreal captureScale(const EffectWindow *window, qreal viewportScale)
{
    const RectF geometry = window->expandedGeometry();
    qreal scale = 0;
    for (const auto *output : effects->screens()) {
        if (output->geometryF().intersects(geometry)) scale = std::max(scale, output->scale());
    }
    // Offscreen renders (e.g. screenshots) may request a denser viewport.
    return std::max(scale, viewportScale);
}

bool WindowEnhancementEffect::capture(WindowState &state, EffectWindow *window, const RenderViewport &viewport)
{
    state.captured = false;
    if (state.itemTreeDirty) {
        for (const auto &connection : state.itemConnections) QObject::disconnect(connection);
        state.itemConnections.clear();
        watchItems(window, state, window->windowItem());
        state.itemTreeDirty = false;
    }
    const qreal scale = captureScale(window, viewport.scale());
    const RectF logicalGeometry = snapToPixels(window->expandedGeometry(), scale);
    const QSize size = (logicalGeometry.size() * scale).toSize();
    if (size.isEmpty() || m_testFailure == "allocation") {
        return false;
    }
    if (!state.texture || state.size != size || state.scale != scale) {
        state.processedFramebuffer.reset();
        state.processed.reset();
        state.cleanupFramebuffer.reset();
        state.cleanupTexture.reset();
        state.framebuffer.reset();
        state.texture = GLTexture::allocate(GL_RGBA8, size);
        if (!state.texture) {
            return false;
        }
        state.texture->setFilter(GL_LINEAR);
        state.texture->setWrapMode(GL_CLAMP_TO_EDGE);
        state.framebuffer = std::make_unique<GLFramebuffer>(state.texture.get());
        if (!state.framebuffer->valid()) {
            return false;
        }
        state.size = size;
        state.scale = scale;
        state.aiStableAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        state.dirty = true;
    }
    if (!state.dirty) {
        return true;
    }

    {
        const RenderTarget captureTarget(state.framebuffer.get());
        const RenderViewport captureViewport(logicalGeometry, scale, captureTarget, QPoint());
        GLFramebuffer::pushFramebuffer(state.framebuffer.get());
        const auto restoreFramebuffer = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
        GLfloat clearColor[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
        const auto restoreClearState = qScopeGuard([&] {
            glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            if (scissor) {
                glEnable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
        });
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        WindowPaintData captureData;
        captureData.setOpacity(1);
        if (!effects->drawWindow(captureTarget, captureViewport, window,
                PAINT_WINDOW_TRANSFORMED | PAINT_WINDOW_TRANSLUCENT, Region::infinite(), captureData)
            || m_testFailure == "capture") {
            return false;
        }
    }
    if (m_settings.compressionCleanup > 0 && !m_testTint) {
        if (!cleanupCapture(state)) return false;
    } else {
        state.cleanupFramebuffer.reset();
        state.cleanupTexture.reset();
    }
    state.dirty = false;
    state.captured = true;
    ++state.revision;
    return true;
}

bool WindowEnhancementEffect::cleanupCapture(WindowState &state)
{
    if (!allocateTarget(state.cleanupTexture, state.cleanupFramebuffer, state.size)) return false;
    renderPass(m_cleanupShader.get(), state.texture.get(), nullptr, state.cleanupFramebuffer.get(), state.size, [&](GLShader *shader) {
        shader->setUniform("texelSize", QVector2D(1.0f / state.size.width(), 1.0f / state.size.height()));
        shader->setUniform("cleanupStrength", m_settings.compressionCleanup * m_settings.strength);
        shader->setUniform("pixelScale", float(state.scale));
    });
    // The rest of the pipeline consumes the cleaned capture, including CAS,
    // NVIDIA input, pending-frame fallback and the final original/AI blend.
    std::swap(state.texture, state.cleanupTexture);
    std::swap(state.framebuffer, state.cleanupFramebuffer);
    ++m_cleanupDraws;
    return true;
}

// Modes: 0 spatial shader, 1 original while AI is pending, 2 AI+CAS, 3 CAS only.
bool WindowEnhancementEffect::filterActive(int mode) const
{
    switch (mode) {
    case 0: return m_settings.strength > 0 && (m_settings.denoise > 0 || m_settings.sharpen > 0);
    case 2: return true;
    case 3: return m_settings.strength > 0 && m_settings.casStrength > 0;
    default: return false;
    }
}

bool WindowEnhancementEffect::process(WindowState &state, int mode, GLTexture *aiOutput)
{
    WindowState::ProcessKey key{state.revision, 0, mode, m_settings.denoise * m_settings.strength, m_settings.sharpen * m_settings.strength,
        m_settings.casStrength, m_settings.strength};
#ifdef KWIN_VFX_NVIDIA_BACKEND
    if (aiOutput) key.aiRevision = state.nvidia->completedRevision();
#endif
    if (state.processed && state.processedKey == key) return true;
    if (!allocateTarget(state.processed, state.processedFramebuffer, state.size)) return false;
    renderPass(m_processShader.get(), state.texture.get(), aiOutput, state.processedFramebuffer.get(), state.size, [&](GLShader *shader) {
        shader->setUniform("aiSampler", 1);
        shader->setUniform("filterMode", mode);
        shader->setUniform("texelSize", QVector2D(1.0f / state.size.width(), 1.0f / state.size.height()));
        shader->setUniform("aiTexelSize", aiOutput
            ? QVector2D(1.0f / aiOutput->width(), 1.0f / aiOutput->height()) : QVector2D());
        shader->setUniform("denoiseStrength", key.denoise);
        shader->setUniform("sharpenStrength", key.sharpen);
        shader->setUniform("casStrength", m_settings.casStrength * m_settings.strength);
        shader->setUniform("aiSharpenStrength", m_settings.casStrength);
        shader->setUniform("enhancementStrength", m_settings.strength);
    });
    state.processedKey = key;
    ++m_processDraws;
    return true;
}

bool WindowEnhancementEffect::paint(WindowState &state, const RenderTarget &target,
    const RenderViewport &viewport, EffectWindow *window, const Region &region, const WindowPaintData &data)
{
    const auto now = std::chrono::steady_clock::now();
    GLTexture *aiOutput = updateNvidia(state, now);
    const bool nvidia = usesNvidia(state);
    const bool hdrEnabled = state.hdrEnabled;
    const bool hdrResult = aiOutput && hdrEnabled;
    // The SDR path filters into a cached texture only when its inputs change;
    // every repaint below is one texture fetch plus grain and color conversion.
    GLTexture *source = state.texture.get();
    if (!hdrResult && !m_testTint) {
        const int mode = m_settings.casOnly ? 3 : !nvidia ? 0 : aiOutput ? 2 : m_settings.casStrength > 0 ? 3 : 1;
        if (filterActive(mode)) {
            if (!process(state, mode, aiOutput)) return false;
            source = state.processed.get();
        }
    }

    const double scale = viewport.scale();
    RectF rect = snapToPixels(window->expandedGeometry(), scale);
    rect.moveTopLeft(rect.topLeft() - snapToPixels(window->frameGeometry(), scale).topLeft());
    RenderGeometry geometry;
    geometry.appendWindowQuad(WindowQuad::fromRect(rect), scale);
    geometry.postProcessTextureCoordinates(source->matrix(NormalizedCoordinates));

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
    const auto mapped = vbo->map<GLVertex2D>(geometry.size());
    if (!mapped) {
        return false;
    }
    geometry.copy(*mapped);
    vbo->unmap();
    if (m_testFailure == "vertex") {
        return false;
    }

    auto *shader = hdrResult ? m_hdrShader.get() : m_shader.get();
    ShaderBinder binder(shader);
    shader->setUniform(GLShader::IntUniform::Sampler, 0);
    if (!m_testTint) {
        shader->setUniform("texelSize", QVector2D(1.0f / state.texture->width(), 1.0f / state.texture->height()));
        shader->setUniform("grainStrength", m_settings.grain * m_settings.strength);
        shader->setUniform("grainFrame", m_grainFrame);
        if (hdrResult) {
            shader->setUniform("aiSampler", 1);
            shader->setUniform("enhancementStrength", m_settings.strength);
        }
    }
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(std::round(window->x() * scale), std::round(window->y() * scale));
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
    const float rgb = data.brightness() * data.opacity();
    shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, data.opacity()));
    shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
    const auto xyz = target.colorDescription()->containerColorimetry().toXYZ();
    shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(xyz(1, 0), xyz(1, 1), xyz(1, 2)));
    shader->setColorspaceUniforms(hdrResult ? m_hdrColor : ColorDescription::sRGB, target.colorDescription(), RenderingIntent::Perceptual);
    if (hdrResult) {
        shader->setUniform("originalToDestination", ColorDescription::sRGB->toOther(*target.colorDescription(), RenderingIntent::Perceptual));
        const auto tf = ColorDescription::sRGB->transferFunction();
        shader->setUniform("originalTransferParams", QVector2D(tf.minLuminance, tf.maxLuminance - tf.minLuminance));
    }

    const bool clipping = region != Region::infinite();
    const Region clipRegion = clipping ? viewport.transform().map(region, target.transformedSize()) : Region::infinite();
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    const bool blend = glIsEnabled(GL_BLEND);
    GLint scissorBox[4], srcRgb, dstRgb, srcAlpha, dstAlpha, activeTexture, textureBinding, aiBinding;
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &aiBinding);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    const auto restoreDrawState = qScopeGuard([&] {
        glBindTexture(GL_TEXTURE_2D, textureBinding);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, aiBinding);
        glActiveTexture(activeTexture);
        glBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
        if (!blend) {
            glDisable(GL_BLEND);
        }
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    });
    if (clipping) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    source->bind();
    if (hdrResult) { glActiveTexture(GL_TEXTURE1); aiOutput->bind(); glActiveTexture(GL_TEXTURE0); }
    vbo->bindArrays();
    vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
    vbo->unbindArrays();
    source->unbind();
    if (hdrResult) { glActiveTexture(GL_TEXTURE1); aiOutput->unbind(); glActiveTexture(GL_TEXTURE0); }
    if (aiOutput) ++m_nvidiaDraws;
    submitNvidia(state, now);
    return true;
}
}
