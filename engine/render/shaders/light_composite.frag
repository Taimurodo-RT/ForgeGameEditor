#version 450

// Multiplies the picture by the light at each pixel, read smoothly
// (bilinear) from the light grid, plus the ambient light. Lamps with a
// radius come in their own buffer (not spread); the brighter of the two
// counts, channel by channel.

layout(location = 0) out vec4 out_light;

layout(std430, set = 2, binding = 0) readonly buffer Light {
    vec4 light[];
};

// Per cell: R, G, B (10 bits each, 0..4 in steps of 1/256).
layout(std430, set = 2, binding = 1) readonly buffer Lamps {
    uint lamps[];
};

layout(set = 3, binding = 0) uniform Params {
    vec4 ambient;
    vec2 screen;   // pixels
    vec2 camera;   // screen centre, tiles from the grid's corner
    float zoom;    // pixels per tile
    float step;    // tiles per cell
    uint width;    // cells
    uint height;
    uint has_lamps; // 1: the lamps buffer has light this frame
} params;

vec3 cell(ivec2 c) {
    c = clamp(c, ivec2(0), ivec2(int(params.width) - 1, int(params.height) - 1));
    int i = c.y * int(params.width) + c.x;
    vec3 l = light[i].rgb;
    if (params.has_lamps != 0u) {
        uint p = lamps[i];
        l = max(l, vec3(float(p & 1023u), float((p >> 10) & 1023u), float((p >> 20) & 1023u)) / 256.0);
    }
    return l;
}

void main() {
    vec2 tile = params.camera + (gl_FragCoord.xy - params.screen * 0.5) / params.zoom;
    vec2 g = tile / params.step - 0.5;
    ivec2 g0 = ivec2(floor(g));
    vec2 f = g - vec2(g0);
    vec3 top = mix(cell(g0), cell(g0 + ivec2(1, 0)), f.x);
    vec3 bottom = mix(cell(g0 + ivec2(0, 1)), cell(g0 + ivec2(1, 1)), f.x);
    out_light = vec4(params.ambient.rgb + mix(top, bottom, f.y), 1.0);
}
