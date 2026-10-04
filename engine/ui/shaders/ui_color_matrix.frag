#version 450

// brightness, contrast, grayscale, sepia, hue-rotate, saturate, invert.
// Applied in premultiplied space: only rgb is transformed, so the constant
// column is scaled by alpha automatically.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Matrix {
    mat4 color_matrix;
} u;

void main() {
    vec4 c = texture(tex, in_uv);
    out_color = vec4(vec3(u.color_matrix * c), c.a);
}
