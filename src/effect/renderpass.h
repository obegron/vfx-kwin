// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <opengl/glutils.h>
#include <QMatrix4x4>
#include <QScopeGuard>
#include <QSize>
#include <memory>

namespace KWin
{
// Draws source (and optional secondary on texture unit 1) through shader into
// target, a full-size offscreen pass with blending, scissor and dithering off.
// Restores every piece of GL state KWin's renderer relies on.
template<typename Setup>
void renderPass(GLShader *shader, GLTexture *source, GLTexture *secondary, GLFramebuffer *target, QSize size, Setup &&setup)
{
    const bool blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST), dither = glIsEnabled(GL_DITHER);
    GLint activeTexture = 0, binding = 0, secondaryBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &secondaryBinding);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    GLFramebuffer::pushFramebuffer(target);
    const auto restore = qScopeGuard([&] {
        GLFramebuffer::popFramebuffer();
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, secondaryBinding);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, binding);
        glActiveTexture(activeTexture);
        if (blend) glEnable(GL_BLEND);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (dither) glEnable(GL_DITHER);
    });
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER);
    ShaderBinder binder(shader);
    QMatrix4x4 projection;
    projection.ortho(0, size.width(), size.height(), 0, -1, 1);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projection);
    shader->setUniform(GLShader::IntUniform::Sampler, 0);
    setup(shader);
    if (secondary) {
        glActiveTexture(GL_TEXTURE1);
        secondary->bind();
        glActiveTexture(GL_TEXTURE0);
    }
    source->bind();
    source->render(size);
}

// (Re)allocates an RGBA8 render target of the given size; true when usable.
inline bool allocateTarget(std::unique_ptr<GLTexture> &texture, std::unique_ptr<GLFramebuffer> &framebuffer, QSize size)
{
    if (texture && framebuffer && texture->size() == size) return true;
    framebuffer.reset();
    texture = GLTexture::allocate(GL_RGBA8, size);
    if (!texture) return false;
    texture->setFilter(GL_LINEAR);
    texture->setWrapMode(GL_CLAMP_TO_EDGE);
    framebuffer = std::make_unique<GLFramebuffer>(texture.get());
    return framebuffer->valid();
}
}
