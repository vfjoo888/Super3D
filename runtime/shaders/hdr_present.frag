#version 450
// The last step of HDR output (nvn_hdr.cpp, docs/HDR.md). In: the frame as the game, the overlay and the blit left it,
// LINEAR light (the game's sRGB views encode on write, a float image has nothing to encode: measured). Values up to 1.0
// are the game's SDR picture and its UI at paper white, through the SDR look (sRGB encode + display gamma). Values above
// 1.0 come only from the composite replacement (tools/renodx_wonder.py): 1 + kEncodeScale * (x / x1 - 1), how far the
// scene was past the point x1 where the game's curve reaches white. Out: scRGB (linear, 1.0 = 80 nits) for a
// VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT swapchain.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    // x: paper white / 80 nits   y: display peak / paper white   z: highlight gain (0: SDR clip)
    // w: highlight start, in multiples of the white point x1 (1.0: the boost starts where the game's picture is white)
    vec4 a;
    // x: unused   y: 1 / kEncodeScale   z: SDR look display gamma (0: off, 2.2, 2.4)   w: unused
    vec4 b;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec3 e = max(texture(src, uv).rgb, vec3(0.0));
    vec3 c = min(e, vec3(1.0));  // the SDR picture
    if (p.b.z > 0.0) {
        // SDR EOTF emulation: the game's sRGB encode, then the display's gamma (p.b.z), as an SDR display shows it.
        // 1.0 maps to 1.0 and 0 to 0, so paper white and the highlights above it are not touched.
        vec3 enc = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThanEqual(c, vec3(0.0031308)));
        c = pow(enc, vec3(p.b.z));
    }
    // Highlights: past the white point, quadratic in scene brightness from the start on. The boost is 0 at the white
    // point itself whatever the start, so the step from the SDR picture into the highlights is continuous; a start below
    // 1 makes them rise faster from there, above 1 keeps the first part white, as in SDR.
    // The boost comes from the brightest channel only and scales the whole pixel, so the colour keeps the exact ratios
    // of the SDR picture (a vivid red whose red channel alone is past white gets brighter, not more saturated).
    vec3 over3 = max(e - 1.0, vec3(0.0)) * p.b.y;      // scene past the white point, in multiples of it
    float over = max(over3.r, max(over3.g, over3.b));
    float start = p.a.w;
    float t = max(over + (1.0 - start), 0.0);          // past the start
    float t0 = max(1.0 - start, 0.0);                   // the white point's own distance past the start
    c *= 1.0 + p.a.z * (t * t - t0 * t0);
    // Exponential rolloff of the brightest channel above paper white into the display's peak: slope 1 at 1.0, so
    // nothing up to SDR white moves; the colour is scaled as a whole, so hue and saturation hold.
    float peak = max(p.a.y, 1.01);
    float m = max(c.r, max(c.g, c.b));
    if (m > 1.0) {
        float range = peak - 1.0;
        c *= (1.0 + range * (1.0 - exp(-(m - 1.0) / range))) / m;
    }
    color = vec4(c * p.a.x, 1.0);
}
