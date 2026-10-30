// Tiles: one instance per visible chunk, tiles looked up per pixel (as engine/render/shaders/tilemap.*).
// Sprites: one instance per creature. Particles: a compute pass moves them, a draw reads them straight from the buffer.

struct View {
    scale: vec2<f32>,  // tiles -> clip space
    cam: vec2<f32>,    // camera centre in tiles (sprites, particles)
    px: f32,           // one screen pixel in tiles
    dt: f32,
    count: u32,
    _pad: u32,
    area: vec4<f32>,   // x0, y0, x1, y1 the particles bounce inside
};

struct Layer {
    tint: vec4<f32>,
    slot_words: u32,
    layer_words: u32,
    atlas_cells: u32,
    atlas_lod: f32,
};

@group(0) @binding(0) var<uniform> view: View;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;
@group(0) @binding(3) var<storage, read> tiles: array<u32>;
@group(1) @binding(0) var<uniform> layer: Layer;

struct TileOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) local: vec2<f32>,
    @location(1) @interpolate(flat) slot: u32,
};

fn corner(v: u32) -> vec2<f32> {
    let x = select(0.0, 1.0, v == 1u || v == 2u || v == 4u);
    let y = select(0.0, 1.0, v == 2u || v == 4u || v == 5u);
    return vec2<f32>(x, y);
}

@vertex
fn tile_vs(@builtin(vertex_index) v: u32, @location(0) origin: vec2<f32>, @location(1) slot: u32) -> TileOut {
    let local = corner(v) * 64.0;
    var o: TileOut;
    o.pos = vec4<f32>((origin + local) * view.scale, 0.0, 1.0);
    o.local = local;
    o.slot = slot;
    return o;
}

@fragment
fn tile_fs(i: TileOut) -> @location(0) vec4<f32> {
    let t = clamp(vec2<i32>(floor(i.local)), vec2<i32>(0), vec2<i32>(63));
    let index = u32(t.y * 64 + t.x);
    let word = tiles[i.slot * layer.slot_words + layer.layer_words + (index >> 1u)];
    let id = select(word & 0xffffu, word >> 16u, (index & 1u) != 0u);
    if (id == 0u || id >= layer.atlas_cells * layer.atlas_cells) {
        discard;
    }
    let cells = f32(layer.atlas_cells);
    let texels = f32(textureDimensions(atlas, 0).x) / cells;
    let inset = 0.5 / texels;
    let inside = clamp(fract(i.local), vec2<f32>(inset), vec2<f32>(1.0 - inset));
    let cell = vec2<f32>(f32(id % layer.atlas_cells), f32(id / layer.atlas_cells));
    return textureSampleLevel(atlas, samp, (cell + inside) / cells, layer.atlas_lod) * layer.tint;
}

struct FlatOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn sprite_vs(@builtin(vertex_index) v: u32, @location(0) at: vec2<f32>, @location(1) color: u32) -> FlatOut {
    let size = max(1.0, view.px * 2.0);
    let p = at - view.cam + (corner(v) - vec2<f32>(0.5)) * size;
    var o: FlatOut;
    o.pos = vec4<f32>(p * view.scale, 0.0, 1.0);
    o.color = unpack4x8unorm(color);
    return o;
}

@fragment
fn flat_fs(i: FlatOut) -> @location(0) vec4<f32> {
    return i.color;
}

@group(0) @binding(4) var<storage, read_write> particles: array<vec4<f32>>;

@compute @workgroup_size(256)
fn particles_cs(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= view.count) {
        return;
    }
    var p = particles[id.x];
    var at = p.xy + p.zw * view.dt;
    var vel = p.zw;
    if (at.x < view.area.x || at.x > view.area.z) { vel.x = -vel.x; at.x = clamp(at.x, view.area.x, view.area.z); }
    if (at.y < view.area.y || at.y > view.area.w) { vel.y = -vel.y; at.y = clamp(at.y, view.area.y, view.area.w); }
    particles[id.x] = vec4<f32>(at, vel);
}

@group(0) @binding(4) var<storage, read> particles_ro: array<vec4<f32>>;

@vertex
fn particle_vs(@builtin(vertex_index) v: u32) -> FlatOut {
    let p = particles_ro[v / 6u];
    let size = max(0.25, view.px);
    let at = p.xy - view.cam + (corner(v % 6u) - vec2<f32>(0.5)) * size;
    var o: FlatOut;
    o.pos = vec4<f32>(at * view.scale, 0.0, 1.0);
    let s = clamp(length(p.zw) / 20.0, 0.0, 1.0);
    o.color = vec4<f32>(1.0, 0.5 + 0.5 * s, 0.2, 0.6);
    return o;
}
