#version 450

layout(location = 0) in vec2 in_local;
layout(location = 1) flat in uint in_slot;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D atlas;

// Tiles of every resident chunk, two 16-bit ids per word.
layout(std430, set = 2, binding = 1) readonly buffer Tiles {
    uint words[];
} tiles;

layout(set = 3, binding = 0) uniform Layer {
    vec4 tint;
    uint slot_words;   // words per chunk slot (all layers)
    uint layer_words;  // offset of this layer inside a slot
    uint atlas_cells;  // cells per atlas row
    float atlas_lod;   // mip level: far zoom-out reads each tile's average colour
} layer;

const int kChunkSize = 64;

void main() {
    ivec2 t = clamp(ivec2(floor(in_local)), ivec2(0), ivec2(kChunkSize - 1));
    uint index = uint(t.y * kChunkSize + t.x);
    uint word = tiles.words[in_slot * layer.slot_words + layer.layer_words + (index >> 1u)];
    uint id = (index & 1u) != 0u ? (word >> 16u) : (word & 0xffffu);
    if (id == 0u) discard;

    // Stay half a texel inside the cell so neighbours never bleed in.
    float cells = float(layer.atlas_cells);
    float texels = float(textureSize(atlas, 0).x) / cells;
    float inset = 0.5 / texels;
    vec2 inside = clamp(fract(in_local), vec2(inset), vec2(1.0 - inset));
    vec2 cell = vec2(float(id % layer.atlas_cells), float(id / layer.atlas_cells));
    out_color = textureLod(atlas, (cell + inside) / cells, layer.atlas_lod) * layer.tint;
}
