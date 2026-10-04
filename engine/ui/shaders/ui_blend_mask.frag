#version 450

// mask-image: the layer times the alpha of a saved mask layer.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D mask;

void main() {
    out_color = texture(tex, in_uv) * texture(mask, in_uv).a;
}
