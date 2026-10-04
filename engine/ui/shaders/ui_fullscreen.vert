#version 450

// One triangle over the viewport, for layer compositing and filters.

layout(set = 1, binding = 0) uniform Quad {
    vec2 uv_offset;
    vec2 uv_scale;
} q;

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    out_uv = vec2(p.x, 1.0 - p.y) * q.uv_scale + q.uv_offset;
}
