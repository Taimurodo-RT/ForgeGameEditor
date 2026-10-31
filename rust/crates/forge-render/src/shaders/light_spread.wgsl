// Light spreading over the tile grid, as engine/render/shaders/light_spread.comp.

struct SpreadParams {
    transmit: vec4<f32>, // per kind: light kept when passing one cell
    width: u32,
    height: u32,
    first: u32, // 1: only sources, no neighbours yet
    pad: u32,
};

// Per cell: source light R, G, B (10 bits each, 0..4 in steps of 1/256) and the cell's kind (2 bits).
@group(0) @binding(0) var<storage, read> cells: array<u32>;
@group(0) @binding(1) var<uniform> spread_params: SpreadParams;
@group(1) @binding(0) var<storage, read> prev: array<vec4<f32>>;
@group(1) @binding(1) var<storage, read_write> next: array<vec4<f32>>;

fn unpack10(cell: u32) -> vec3<f32> {
    return vec3<f32>(f32(cell & 1023u), f32((cell >> 10u) & 1023u), f32((cell >> 20u) & 1023u)) / 256.0;
}

// One step: every cell takes the brightest of its own source and its eight neighbours' light, dimmed by how much this
// cell lets through. Repeated N times, light reaches N cells and flows around corners into caves.
@compute @workgroup_size(16, 16)
fn spread(@builtin(global_invocation_id) gid: vec3<u32>) {
    let c = gid.xy;
    if c.x >= spread_params.width || c.y >= spread_params.height {
        return;
    }
    let i = c.y * spread_params.width + c.x;
    let cell = cells[i];
    var best = unpack10(cell);
    if spread_params.first == 0u {
        let t = spread_params.transmit[cell >> 30u];
        let td = pow(t, 1.41421356);
        let w = i32(spread_params.width);
        let h = i32(spread_params.height);
        let x = i32(c.x);
        let y = i32(c.y);
        for (var dy = -1; dy <= 1; dy++) {
            let ny = y + dy;
            if ny < 0 || ny >= h {
                continue;
            }
            for (var dx = -1; dx <= 1; dx++) {
                let nx = x + dx;
                if (dx == 0 && dy == 0) || nx < 0 || nx >= w {
                    continue;
                }
                let l = prev[ny * w + nx].rgb * select(t, td, dx != 0 && dy != 0);
                best = max(best, l);
            }
        }
    }
    next[i] = vec4<f32>(best, 1.0);
}
