#version 450

// The layer's alpha, offset and tinted; blurred afterwards when needed.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Shadow {
    vec4 color;
    vec2 uv_min;
    vec2 uv_max;
} u;

void main() {
    vec2 inside = step(u.uv_min, in_uv) * step(in_uv, u.uv_max);
    out_color = texture(tex, in_uv).a * inside.x * inside.y * u.color;
}
