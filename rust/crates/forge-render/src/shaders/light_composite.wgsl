// The picture multiplied by the light, as engine/render/shaders/light_composite.*.

struct Composite {
    ambient: vec4<f32>,
    screen: vec2<f32>, // pixels
    camera: vec2<f32>, // screen centre, tiles from the grid's corner
    zoom: f32,         // pixels per tile
    step: f32,         // tiles per cell
    width: u32,        // cells
    height: u32,
    has_lamps: u32,    // 1: the lamps buffer has light this frame
};

@group(0) @binding(0) var<storage, read> light: array<vec4<f32>>;
// Per cell: the lamps with a radius, R, G, B (10 bits each).
@group(0) @binding(1) var<storage, read> lamps: array<u32>;
@group(0) @binding(2) var<uniform> composite: Composite;

fn unpack10(cell: u32) -> vec3<f32> {
    return vec3<f32>(f32(cell & 1023u), f32((cell >> 10u) & 1023u), f32((cell >> 20u) & 1023u)) / 256.0;
}

// A triangle that covers the whole screen.
@vertex
fn composite_vs(@builtin(vertex_index) v: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((v << 1u) & 2u), f32(v & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

fn grid_cell(c0: vec2<i32>) -> vec3<f32> {
    let c = clamp(c0, vec2<i32>(0), vec2<i32>(i32(composite.width) - 1, i32(composite.height) - 1));
    let i = c.y * i32(composite.width) + c.x;
    var l = light[i].rgb;
    if composite.has_lamps != 0u {
        l = max(l, unpack10(lamps[i]));
    }
    return l;
}

// The light at each pixel, read smoothly (bilinear) from the grid, plus the ambient light; the brighter of the
// spread light and the lamps counts, channel by channel. The blend multiplies the picture by it.
@fragment
fn composite_fs(@builtin(position) frag: vec4<f32>) -> @location(0) vec4<f32> {
    let tile = composite.camera + (frag.xy - composite.screen * 0.5) / composite.zoom;
    let g = tile / composite.step - 0.5;
    let g0 = vec2<i32>(floor(g));
    let f = g - vec2<f32>(g0);
    let top = mix(grid_cell(g0), grid_cell(g0 + vec2<i32>(1, 0)), f.x);
    let bottom = mix(grid_cell(g0 + vec2<i32>(0, 1)), grid_cell(g0 + vec2<i32>(1, 1)), f.x);
    return vec4<f32>(composite.ambient.rgb + mix(top, bottom, f.y), 1.0);
}
