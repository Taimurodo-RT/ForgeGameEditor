#version 450

// Untextured geometry binds a 1x1 white texture, so one shader covers both.

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;

void main() {
    out_color = in_color * texture(tex, in_uv);
}
