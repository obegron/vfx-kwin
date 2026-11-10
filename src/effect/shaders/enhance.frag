// SPDX-License-Identifier: GPL-2.0-or-later
// Per-repaint draw of the cached SDR result (see process.frag): animated grain,
// destination color management, saturation and modulation only.
#version 140
#include "colormanagement.glsl"
#include "saturation.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;
uniform vec2 texelSize;
uniform float grainStrength;
uniform int grainFrame;
in vec2 texcoord0;
out vec4 fragColor;

float grainRandom(uvec2 pixel, uint seed)
{
    uint value = pixel.x * 1973u + pixel.y * 9277u + seed * 26699u;
    value = (value ^ 61u) ^ (value >> 16u);
    value *= 9u;
    value ^= value >> 4u;
    value *= 668265261u;
    value ^= value >> 15u;
    return float(value & 65535u) / 65535.0;
}

vec4 addFilmGrain(vec4 color)
{
    if (grainStrength <= 0.0 || color.a <= 0.0) return color;
    vec3 straight = clamp(color.rgb / color.a, 0.0, 1.0);
    float luma = dot(straight, vec3(0.2126, 0.7152, 0.0722));
    // Monochrome triangular noise, strongest in midtones. Pure black/white
    // stay unchanged, preserving letterbox bars; alpha is never modified.
    uvec2 pixel = uvec2(floor(texcoord0 / texelSize));
    uint frame = uint(grainFrame);
    float noise = grainRandom(pixel, frame) + grainRandom(pixel, frame + 7919u) - 1.0;
    float amplitude = 0.08 * grainStrength * (4.0 * luma * (1.0 - luma));
    color.rgb = clamp(straight + vec3(noise * amplitude), 0.0, 1.0) * color.a;
    return color;
}

void main()
{
    vec4 tex = addFilmGrain(texture(sampler, texcoord0));
    if (tex.a <= 0.0) { fragColor = vec4(0.0); return; }
    // Color transforms can include a black-point offset, and tone mapping is
    // nonlinear. Apply them to straight RGB, then restore premultiplied alpha.
    float alpha = tex.a;
    tex = sourceEncodingToNitsInDestinationColorspace(vec4(tex.rgb / alpha, 1.0));
    tex.rgb *= alpha;
    tex.a = alpha;
    tex = adjustSaturation(tex);
    fragColor = nitsToDestinationEncoding(tex * modulation);
}
