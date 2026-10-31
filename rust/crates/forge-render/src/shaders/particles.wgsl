// Particles that live on the video card, as engine/render/shaders/particles_*.comp: spawned and moved here, drawn by
// sprites.wgsl (particle_vs).

struct Emitter {
    pos: vec2<f32>,
    radius: f32,
    speed_min: f32,
    speed_max: f32,
    angle: f32,
    spread: f32,
    life_min: f32,
    life_max: f32,
    size_start: f32,
    size_end: f32,
    spin: f32,
    color: u32,
    frame: u32,
    first: u32, // index of its first particle among this frame's new ones
    count: u32,
};

struct Particle {
    pos: vec2<f32>,
    vel: vec2<f32>,
    age: f32,
    life: f32,
    size_start: f32,
    size_end: f32,
    color: u32,
    frame: u32,
    angle: f32,
    spin: f32,
};

struct EmitParams {
    total: u32,         // new particles this frame
    emitter_count: u32,
    first_slot: u32,    // where they go in the ring
    capacity: u32,
    seed: u32,
};

struct UpdateParams {
    gravity: vec2<f32>, // tiles per second²
    drag: f32,          // share of speed lost per second
    dt: f32,
    count: u32,         // slots in use
};

@group(0) @binding(0) var<storage, read_write> particles: array<Particle>;
@group(1) @binding(0) var<storage, read> emitters: array<Emitter>;
@group(1) @binding(1) var<uniform> emit_params: EmitParams;
@group(1) @binding(2) var<uniform> update_params: UpdateParams;

fn hash(v: u32) -> u32 {
    var x = v;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

fn rand(state: ptr<function, u32>) -> f32 {
    *state = hash(*state);
    return f32(*state & 0xffffffu) / 16777216.0;
}

// Spawns this frame's particles: one thread per new particle, every emitter in one dispatch.
@compute @workgroup_size(256)
fn emit(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if i >= emit_params.total {
        return;
    }
    // Which emitter: the last one whose first particle is at or before i.
    var lo = 0u;
    var hi = emit_params.emitter_count - 1u;
    while lo < hi {
        let mid = (lo + hi + 1u) / 2u;
        if emitters[mid].first <= i {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    let e = emitters[lo];
    var state = hash(i * 1664525u + emit_params.seed);
    let r = e.radius * sqrt(rand(&state));
    let a = rand(&state) * 6.2831853;
    let dir = e.angle + (rand(&state) - 0.5) * e.spread;
    let speed = mix(e.speed_min, e.speed_max, rand(&state));
    var p: Particle;
    p.pos = e.pos + vec2<f32>(cos(a), sin(a)) * r;
    p.vel = vec2<f32>(cos(dir), sin(dir)) * speed;
    p.age = 0.0;
    p.life = mix(e.life_min, e.life_max, rand(&state));
    p.size_start = e.size_start;
    p.size_end = e.size_end;
    p.color = e.color;
    p.frame = e.frame;
    p.angle = rand(&state) * 6.2831853;
    p.spin = (rand(&state) * 2.0 - 1.0) * e.spin;
    particles[(emit_params.first_slot + i) % emit_params.capacity] = p;
}

// Moves every particle one step: gravity, drag, ageing, spin.
@compute @workgroup_size(256)
fn update(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x;
    if i >= update_params.count {
        return;
    }
    var p = particles[i];
    if p.age >= p.life {
        return;
    }
    let dt = update_params.dt;
    p.vel += update_params.gravity * dt;
    p.vel *= exp(-update_params.drag * dt);
    p.pos += p.vel * dt;
    p.age += dt;
    p.angle += p.spin * dt;
    particles[i] = p;
}
