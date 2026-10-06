#version 450

// mix-blend-mode: a layer composited onto its backdrop with one of the CSS / W3C
// compositing blend modes, then source-over. Both inputs are premultiplied.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 1) uniform sampler2D backdrop;
layout(set = 3, binding = 0) uniform Mode {
    ivec4 mode; // x: 0 multiply, 1 screen, 2 overlay, 3 darken, 4 lighten, 5 color-dodge, 6 color-burn,
                // 7 hard-light, 8 soft-light, 9 difference, 10 exclusion, 11 hue, 12 saturation, 13 color,
                // 14 luminosity, 15 plus-lighter
} u;

vec3 multiply(vec3 b, vec3 s) { return b * s; }
vec3 screen(vec3 b, vec3 s) { return b + s - b * s; }
vec3 hard_light(vec3 b, vec3 s) {
    return mix(multiply(b, 2.0 * s), screen(b, 2.0 * s - 1.0), step(0.5, s));
}
float color_dodge(float b, float s) {
    if (b <= 0.0) return 0.0;
    if (s >= 1.0) return 1.0;
    return min(1.0, b / (1.0 - s));
}
float color_burn(float b, float s) {
    if (b >= 1.0) return 1.0;
    if (s <= 0.0) return 0.0;
    return 1.0 - min(1.0, (1.0 - b) / s);
}
float soft_light(float b, float s) {
    if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    float d = (b <= 0.25) ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}

float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clip_color(vec3 c) {
    float l = lum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / (l - n);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);
    return c;
}
vec3 set_lum(vec3 c, float l) { return clip_color(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }
vec3 set_sat(vec3 c, float s) {
    float mx = max(max(c.r, c.g), c.b);
    float mn = min(min(c.r, c.g), c.b);
    if (mx <= mn) return vec3(0.0);
    return (c - mn) * s / (mx - mn);
}

vec3 blend(vec3 b, vec3 s, int mode) {
    switch (mode) {
    case 0: return multiply(b, s);
    case 1: return screen(b, s);
    case 2: return hard_light(s, b); // overlay is hard-light with the layers swapped
    case 3: return min(b, s);
    case 4: return max(b, s);
    case 5: return vec3(color_dodge(b.r, s.r), color_dodge(b.g, s.g), color_dodge(b.b, s.b));
    case 6: return vec3(color_burn(b.r, s.r), color_burn(b.g, s.g), color_burn(b.b, s.b));
    case 7: return hard_light(b, s);
    case 8: return vec3(soft_light(b.r, s.r), soft_light(b.g, s.g), soft_light(b.b, s.b));
    case 9: return abs(b - s);
    case 10: return b + s - 2.0 * b * s;
    case 11: return set_lum(set_sat(s, sat(b)), lum(b));
    case 12: return set_lum(set_sat(b, sat(s)), lum(b));
    case 13: return set_lum(s, lum(b));
    case 14: return set_lum(b, lum(s));
    }
    return s;
}

void main() {
    vec4 s = texture(source, in_uv);
    vec4 b = texture(backdrop, in_uv);
    if (u.mode.x == 15) { // plus-lighter: the premultiplied colours added up
        out_color = min(s + b, vec4(1.0));
        return;
    }
    vec3 cs = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    vec3 cb = b.a > 0.0 ? b.rgb / b.a : vec3(0.0);
    vec3 mixed = (1.0 - b.a) * cs + b.a * clamp(blend(cb, cs, u.mode.x), 0.0, 1.0);
    out_color = vec4(s.a * mixed + (1.0 - s.a) * b.rgb, s.a + b.a * (1.0 - s.a));
}
