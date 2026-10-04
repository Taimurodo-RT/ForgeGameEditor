#version 450

// Copies a layer, scaled by a factor (opacity filter, clears).

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Tint {
    vec4 tint;
} u;

void main() {
    out_color = texture(tex, in_uv) * u.tint;
}
