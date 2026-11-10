// SPDX-License-Identifier: GPL-2.0-or-later
#version 140
#include "kwin-vfx/compression.glsl"
uniform sampler2D sampler;
uniform vec2 texelSize;
uniform float cleanupStrength;
uniform float pixelScale;
in vec2 texcoord0;
out vec4 fragColor;
void main()
{
    fragColor = cleanupCompression(sampler, texcoord0, texelSize, cleanupStrength, pixelScale);
}
