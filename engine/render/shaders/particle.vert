#version 450

// Draws particles straight from the simulation buffer: instance N is
// particle N. Dead ones become an empty triangle.

struct Particle {
    vec2 pos;
    vec2 vel;
    float age;
    float life;
    float size_start;
    float size_end;
    uint color;
    uint frame;
    float angle;
    float spin;
};

layout(std430, set = 0, binding = 0) readonly buffer Particles {
    Particle particles[];
};

layout(std430, set = 0, binding = 1) readonly buffer Frames {
    vec4 frames[];
};

layout(set = 1, binding = 0) uniform View {
    vec2 scale;  // tiles -> clip space
    vec2 offset; // system origin minus camera centre, tiles
} view;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    Particle p = particles[gl_InstanceIndex];
    out_uv = vec2(0.0);
    out_color = vec4(0.0);
    if (p.age >= p.life) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the screen
        return;
    }
    float t = p.age / p.life;
    uint v = uint(gl_VertexIndex);
    vec2 corner = vec2((v == 1u || v == 2u || v == 4u) ? 1.0 : 0.0, (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0);
    vec2 local = (corner - 0.5) * mix(p.size_start, p.size_end, t);
    float c = cos(p.angle);
    float n = sin(p.angle);
    vec2 pos = vec2(local.x * c - local.y * n, local.x * n + local.y * c) + p.pos + view.offset;
    gl_Position = vec4(pos * view.scale, 0.0, 1.0);

    vec4 f = frames[p.frame];
    out_uv = mix(f.xy, f.zw, corner);
    vec4 tint = unpackUnorm4x8(p.color);
    tint.a *= clamp((1.0 - t) * 2.0, 0.0, 1.0); // fade out over the second half of life
    out_color = vec4(tint.rgb * tint.a, tint.a);
}
