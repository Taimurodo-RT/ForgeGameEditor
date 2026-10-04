#version 450

// RmlUi geometry: positions in pixels, premultiplied vertex colours.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(set = 1, binding = 0) uniform Transform {
    mat4 transform; // projection * element transform
    vec2 translate;
} u;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    out_uv = in_uv;
    out_color = in_color;
    gl_Position = u.transform * vec4(in_position + u.translate, 0.0, 1.0);
}
