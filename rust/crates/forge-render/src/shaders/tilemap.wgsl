// Tiles, as engine/render/shaders/tilemap.*: one instance per visible chunk, a 64 × 64 tile quad. Tiles are looked
// up per pixel, so a whole screen of the world costs one draw call per layer no matter how many tiles it shows.

struct View {
    scale: vec2<f32>, // tiles -> clip space
    unused: vec2<f32>,
};

struct Layer {
    tint: vec4<f32>,
    slot_words: u32,  // words per chunk slot (all layers)
    layer_words: u32, // offset of this layer inside a slot
    atlas_cells: u32, // cells per atlas row
    atlas_lod: f32,   // mip level: far zoom-out reads each tile's average colour
    liquid: u32,      // 1: this layer holds liquids (kind << 12 | amount), not tile ids
    full: u32,        // amount of a full liquid tile
    down: u32,        // where liquids settle: 0 +y, 1 -x, 2 -y, 3 +x
    plain_from: u32,  // ids from here on keep their own colours, untinted (0: none do)
    liquid_colors: array<vec4<f32>, 16>,
};

@group(0) @binding(0) var<uniform> view: View;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
// Tiles of every resident chunk, two 16-bit ids per word.
@group(0) @binding(3) var<storage, read> tiles: array<u32>;
@group(1) @binding(0) var<uniform> layer: Layer;

const CHUNK: f32 = 64.0;

struct Out {
    @builtin(position) pos: vec4<f32>,
    @location(0) local: vec2<f32>, // tile position inside the chunk, 0..64
    @location(1) @interpolate(flat) slot: u32,
};

fn corner(v: u32) -> vec2<f32> {
    // Two triangles: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1).
    return vec2<f32>(select(0.0, 1.0, v == 1u || v == 2u || v == 4u), select(0.0, 1.0, v == 2u || v == 4u || v == 5u));
}

@vertex
fn vs(@builtin(vertex_index) v: u32, @location(0) origin: vec2<f32>, @location(1) slot: u32) -> Out {
    let local = corner(v) * CHUNK;
    var o: Out;
    o.pos = vec4<f32>((origin + local) * view.scale, 0.0, 1.0);
    o.local = local;
    o.slot = slot;
    return o;
}

fn cell_at(slot: u32, t: vec2<i32>) -> u32 {
    let index = u32(t.y * 64 + t.x);
    let word = tiles[slot * layer.slot_words + layer.layer_words + (index >> 1u)];
    return select(word & 0xffffu, word >> 16u, (index & 1u) != 0u);
}

fn liquid(o: Out, t: vec2<i32>, cell: u32) -> vec4<f32> {
    let kind = cell >> 12u;
    if kind == 0u {
        discard;
    }
    var level = min(f32(cell & 0xfffu) / f32(layer.full), 1.0);
    // Distance from the cell's floor, 0..1, and the cell above it.
    let f = fract(o.local);
    var height: f32;
    var up: vec2<i32>;
    if layer.down == 0u {
        height = 1.0 - f.y;
        up = vec2<i32>(0, -1);
    } else if layer.down == 1u {
        height = f.x;
        up = vec2<i32>(1, 0);
    } else if layer.down == 2u {
        height = f.y;
        up = vec2<i32>(0, 1);
    } else {
        height = 1.0 - f.x;
        up = vec2<i32>(-1, 0);
    }
    // Under more liquid the cell shows full, so falling streams have no gaps.
    let above = t + up;
    var covered = false;
    if all(above >= vec2<i32>(0)) && all(above < vec2<i32>(64)) {
        covered = (cell_at(o.slot, above) >> 12u) != 0u;
    }
    if covered {
        level = 1.0;
    }
    if height > level {
        discard;
    }
    var c = layer.liquid_colors[kind];
    // A lighter line along the surface.
    if !covered && level - height < 0.12 {
        c = vec4<f32>(mix(c.rgb, vec3<f32>(1.0), 0.35), c.a);
    }
    return c * layer.tint;
}

@fragment
fn fs(o: Out) -> @location(0) vec4<f32> {
    let t = clamp(vec2<i32>(floor(o.local)), vec2<i32>(0), vec2<i32>(63));
    let id = cell_at(o.slot, t);
    if layer.liquid != 0u {
        return liquid(o, t, id);
    }
    // An id the atlas has no cell for shows nothing.
    if id == 0u || id >= layer.atlas_cells * layer.atlas_cells {
        discard;
    }
    // Stay half a texel inside the cell so neighbours never bleed in.
    let cells = f32(layer.atlas_cells);
    let texels = f32(textureDimensions(atlas, 0).x) / cells;
    let inset = 0.5 / texels;
    let inside = clamp(fract(o.local), vec2<f32>(inset), vec2<f32>(1.0 - inset));
    let cell = vec2<f32>(f32(id % layer.atlas_cells), f32(id / layer.atlas_cells));
    let tint = select(layer.tint, vec4<f32>(1.0), layer.plain_from != 0u && id >= layer.plain_from);
    return textureSampleLevel(atlas, samp, (cell + inside) / cells, layer.atlas_lod) * tint;
}
