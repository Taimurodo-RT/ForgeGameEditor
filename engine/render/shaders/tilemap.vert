#version 450

// One instance per visible chunk: a 64 × 64 tile quad. Tiles are looked up
// per pixel in the fragment shader, so a whole screen of the world costs one
// draw call per layer no matter how many tiles it shows.

layout(location = 0) in vec2 in_origin; // chunk corner relative to the camera, in tiles
layout(location = 1) in uint in_slot;   // where the chunk's tiles live on the GPU

layout(set = 1, binding = 0) uniform View {
    vec2 scale; // tiles -> clip space
    vec2 unused;
} view;

layout(location = 0) out vec2 out_local; // tile position inside the chunk, 0..64
layout(location = 1) flat out uint out_slot;

const float kChunkSize = 64.0;

void main() {
    // Two triangles: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1).
    uint v = uint(gl_VertexIndex);
    vec2 corner = vec2((v == 1u || v == 2u || v == 4u) ? 1.0 : 0.0, (v == 2u || v == 4u || v == 5u) ? 1.0 : 0.0);
    vec2 local = corner * kChunkSize;
    gl_Position = vec4((in_origin + local) * view.scale, 0.0, 1.0);
    out_local = local;
    out_slot = in_slot;
}
