#version 450

// Separable Gaussian blur, 7 taps along one axis.

#define BLUR_SIZE 7
#define BLUR_NUM_WEIGHTS 4

layout(set = 1, binding = 0) uniform Blur {
    vec2 texel_offset;
    vec2 pad;
} b;

layout(location = 0) out vec2 out_uv[BLUR_SIZE];

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vec2 uv = vec2(p.x, 1.0 - p.y);
    for (int i = 0; i < BLUR_SIZE; i++)
        out_uv[i] = uv - float(i - BLUR_NUM_WEIGHTS + 1) * b.texel_offset;
}
