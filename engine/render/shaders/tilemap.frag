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
    uint liquid;       // 1: this layer holds liquids (kind << 12 | amount), not tile ids
    uint full;         // amount of a full liquid tile
    uint down;         // where liquids settle: 0 +y, 1 -x, 2 -y, 3 +x
    uint plain_from;   // ids from here on keep their own colours, untinted (0: none do)
    vec4 liquid_colors[16];
} layer;

const int kChunkSize = 64;

uint cell_at(ivec2 t) {
    uint index = uint(t.y * kChunkSize + t.x);
    uint word = tiles.words[in_slot * layer.slot_words + layer.layer_words + (index >> 1u)];
    return (index & 1u) != 0u ? (word >> 16u) : (word & 0xffffu);
}

void draw_liquid(ivec2 t, uint cell) {
    uint kind = cell >> 12u;
    if (kind == 0u) discard;
    float level = min(float(cell & 0xfffu) / float(layer.full), 1.0);
    // Distance from the cell's floor, 0..1, and the cell above it.
    vec2 f = fract(in_local);
    float height;
    ivec2 up;
    if (layer.down == 0u) { height = 1.0 - f.y; up = ivec2(0, -1); }
    else if (layer.down == 1u) { height = f.x; up = ivec2(1, 0); }
    else if (layer.down == 2u) { height = f.y; up = ivec2(0, 1); }
    else { height = 1.0 - f.x; up = ivec2(-1, 0); }
    // Under more liquid the cell shows full, so falling streams have no gaps.
    ivec2 above = t + up;
    bool covered = all(greaterThanEqual(above, ivec2(0))) && all(lessThan(above, ivec2(kChunkSize))) &&
                   (cell_at(above) >> 12u) != 0u;
    if (covered) level = 1.0;
    if (height > level) discard;
    vec4 c = layer.liquid_colors[kind];
    // A lighter line along the surface.
    if (!covered && level - height < 0.12) c.rgb = mix(c.rgb, vec3(1.0), 0.35);
    out_color = c * layer.tint;
}

void main() {
    ivec2 t = clamp(ivec2(floor(in_local)), ivec2(0), ivec2(kChunkSize - 1));
    uint id = cell_at(t);
    if (layer.liquid != 0u) {
        draw_liquid(t, id);
        return;
    }
    // An id the atlas has no cell for shows nothing.
    if (id == 0u || id >= layer.atlas_cells * layer.atlas_cells) discard;

    // Stay half a texel inside the cell so neighbours never bleed in.
    float cells = float(layer.atlas_cells);
    float texels = float(textureSize(atlas, 0).x) / cells;
    float inset = 0.5 / texels;
    vec2 inside = clamp(fract(in_local), vec2(inset), vec2(1.0 - inset));
    vec2 cell = vec2(float(id % layer.atlas_cells), float(id / layer.atlas_cells));
    vec4 tint = layer.plain_from != 0u && id >= layer.plain_from ? vec4(1.0) : layer.tint;
    out_color = textureLod(atlas, (cell + inside) / cells, layer.atlas_lod) * tint;
}
