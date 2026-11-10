// SPDX-License-Identifier: GPL-2.0-or-later
// SDR encoded, premultiplied input. No codec-grid alignment is assumed: browser
// video may have been scaled or translated before it reaches the compositor.
vec4 cleanupCompression(sampler2D image, vec2 uv, vec2 texel, float strength, float pixelScale)
{
    vec4 center = texture(image, uv);
    if (strength <= 0.0 || center.a <= 0.00001) return center;
    vec3 rgb = center.rgb / center.a;
    float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    float shadow = 1.0 - smoothstep(0.12, 0.45, luma);
    if (shadow <= 0.0) return center;

    vec3 sum = vec3(0.0);
    float total = 0.0;
    float edge = 0.0;
    float range = mix(0.035, 0.075, strength);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            // Nearby samples catch small steps; the wider ring crosses larger
            // block boundaries without needing the original video resolution.
            vec2 offset = vec2(abs(x) == 2 ? float(x) * 2.0 : float(x),
                               abs(y) == 2 ? float(y) * 2.0 : float(y));
            vec4 tap = (x == 0 && y == 0) ? center
                : texture(image, uv + offset * texel * pixelScale);
            vec3 sampleRgb = tap.rgb / max(tap.a, 0.00001);
            vec3 delta = abs(sampleRgb - rgb);
            float difference = max(delta.r, max(delta.g, delta.b));
            if (abs(x) <= 1 && abs(y) <= 1) edge = max(edge, difference);
            float weight = float((x == 0 ? 4 : abs(x) == 1 ? 2 : 1)
                               * (y == 0 ? 4 : abs(y) == 1 ? 2 : 1));
            float compatible = 1.0 - smoothstep(range * 0.25, range, difference);
            weight *= compatible * compatible;
            weight *= tap.a > 0.00001 ? max(0.0, 1.0 - abs(tap.a - center.a) * 16.0) : 0.0;
            sum += sampleRgb * weight;
            total += weight;
        }
    }
    // Keep text, silhouettes and other strong edges out of the smoothing pass.
    float preserveEdge = 1.0 - smoothstep(0.08, 0.18, edge);
    vec3 cleaned = mix(rgb, sum / max(total, 0.00001), strength * shadow * preserveEdge);
    return vec4(cleaned * center.a, center.a);
}

