// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen filter pass. Runs once per capture/AI revision or settings change;
// its premultiplied SDR result is cached and drawn by enhance.frag.
#version 140
#include "kwin-vfx/filter.glsl"
#include "kwin-vfx/cas.glsl"

uniform sampler2D sampler;
uniform sampler2D aiSampler;
uniform vec2 texelSize;
uniform vec2 aiTexelSize;
uniform int filterMode; // 0: spatial shader, 2: AI+CAS, 3: CAS only
uniform float denoiseStrength;
uniform float sharpenStrength;
uniform float casStrength;
uniform float aiSharpenStrength;
uniform float enhancementStrength;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    if (filterMode == 0) {
        fragColor = enhanceWindow(sampler, texcoord0, texelSize, denoiseStrength, sharpenStrength);
    } else if (filterMode == 3) {
        fragColor = casSharpen(sampler, texcoord0, texelSize, casStrength);
    } else {
        vec4 tex = texture(sampler, texcoord0);
        vec4 ai = casSharpen(aiSampler, texcoord0, aiTexelSize, aiSharpenStrength);
        // The worker sees straight RGB with opaque alpha; keep original alpha.
        if (ai.a > 0.99) tex.rgb = mix(tex.rgb, ai.rgb * tex.a, enhancementStrength);
        fragColor = tex;
    }
}
