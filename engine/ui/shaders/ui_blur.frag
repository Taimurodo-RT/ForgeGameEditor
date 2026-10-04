#version 450

#define BLUR_SIZE 7
#define BLUR_NUM_WEIGHTS 4

layout(location = 0) in vec2 in_uv[BLUR_SIZE];
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Weights {
    vec4 weights; // centre tap first
    vec2 uv_min;  // samples outside the blurred region count as transparent
    vec2 uv_max;
} u;

void main() {
    vec4 color = vec4(0.0);
    for (int i = 0; i < BLUR_SIZE; i++) {
        vec2 inside = step(u.uv_min, in_uv[i]) * step(in_uv[i], u.uv_max);
        color += texture(tex, in_uv[i]) * inside.x * inside.y * u.weights[abs(i - BLUR_NUM_WEIGHTS + 1)];
    }
    out_color = color;
}
