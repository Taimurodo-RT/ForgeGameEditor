#version 450

// Sprites are pulled from a storage buffer and drawn as two triangles each.
// Instance N is sprite N, or sprite order[N] when the list was sorted. No
// vertex buffers, so a million sprites cost one upload and one draw call.

struct Sprite {
    vec2 pos;   // centre, tiles from the batch origin
    vec2 size;  // tiles; negative x mirrors
    float angle;
    uint frame;
    uint color; // RGBA8
    uint order;
};

layout(std430, set = 0, binding = 0) readonly buffer Sprites {
    Sprite sprites[];
};

// Texture rectangle of every frame: u0, v0, u1, v1.
layout(std430, set = 0, binding = 1) readonly buffer Frames {
    vec4 frames[];
};

// Draw order after sorting: indices into sprites.
layout(std430, set = 0, binding = 2) readonly buffer Order {
    uint order[];
};

layout(set = 1, binding = 0) uniform View {
    vec2 scale;  // tiles -> clip space
    vec2 offset; // batch origin minus camera centre, tiles
    uint sorted; // instances go through order[]
} view;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    uint id = view.sorted != 0u ? order[gl_InstanceIndex] : uint(gl_InstanceIndex);
    Sprite s = sprites[id];
    uint v = uint(gl_VertexIndex);
    vec2 corner = vec2((v == 1u || v == 2u || v == 4u) ? 1.0 : 0.0, (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0);
    vec2 local = (corner - 0.5) * s.size;
    float c = cos(s.angle);
    float n = sin(s.angle);
    vec2 p = vec2(local.x * c - local.y * n, local.x * n + local.y * c) + s.pos + view.offset;
    gl_Position = vec4(p * view.scale, 0.0, 1.0);

    vec4 f = frames[s.frame];
    out_uv = mix(f.xy, f.zw, corner);
    vec4 tint = unpackUnorm4x8(s.color);
    out_color = vec4(tint.rgb * tint.a, tint.a); // premultiplied
}
