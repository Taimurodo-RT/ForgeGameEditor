// Sprites and particles, as engine/render/shaders/sprite.*, particle.vert: pulled from a storage buffer and drawn as
// two triangles each, no vertex buffers, so a million cost one upload and one draw call.

struct Sprite {
    pos: vec2<f32>,  // centre, tiles from the batch origin
    size: vec2<f32>, // tiles; negative x mirrors
    angle: f32,
    frame: u32,
    color: u32, // RGBA8
    order: u32,
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

struct View {
    scale: vec2<f32>,  // tiles -> clip space
    offset: vec2<f32>, // batch origin minus camera centre, tiles
    sorted: u32,       // instances go through order[]
};

@group(0) @binding(0) var<uniform> view: View;
// Texture rectangle of every frame: u0, v0, u1, v1.
@group(0) @binding(1) var<storage, read> frames: array<vec4<f32>>;
@group(0) @binding(2) var sheet: texture_2d<f32>;
@group(0) @binding(3) var samp: sampler;
@group(1) @binding(0) var<storage, read> sprites: array<Sprite>;
// Draw order after sorting: indices into sprites.
@group(1) @binding(1) var<storage, read> order: array<u32>;
@group(1) @binding(2) var<storage, read> particles: array<Particle>;

struct Out {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
};

fn corner(v: u32) -> vec2<f32> {
    return vec2<f32>(select(0.0, 1.0, v == 1u || v == 2u || v == 4u), select(0.0, 1.0, v == 2u || v == 4u || v == 5u));
}

fn place(c: vec2<f32>, size: vec2<f32>, angle: f32, pos: vec2<f32>) -> vec4<f32> {
    let local = (c - 0.5) * size;
    let co = cos(angle);
    let si = sin(angle);
    let p = vec2<f32>(local.x * co - local.y * si, local.x * si + local.y * co) + pos + view.offset;
    return vec4<f32>(p * view.scale, 0.0, 1.0);
}

@vertex
fn sprite_vs(@builtin(vertex_index) v: u32, @builtin(instance_index) n: u32) -> Out {
    let id = select(n, order[n], view.sorted != 0u);
    let s = sprites[id];
    let c = corner(v);
    var o: Out;
    o.pos = place(c, s.size, s.angle, s.pos);
    let f = frames[s.frame];
    o.uv = mix(f.xy, f.zw, c);
    let tint = unpack4x8unorm(s.color);
    o.color = vec4<f32>(tint.rgb * tint.a, tint.a); // premultiplied
    return o;
}

// Instance N is particle N; dead ones become an empty triangle.
@vertex
fn particle_vs(@builtin(vertex_index) v: u32, @builtin(instance_index) n: u32) -> Out {
    let p = particles[n];
    var o: Out;
    o.uv = vec2<f32>(0.0);
    o.color = vec4<f32>(0.0);
    if p.age >= p.life {
        o.pos = vec4<f32>(2.0, 2.0, 2.0, 1.0); // outside the screen
        return o;
    }
    let t = p.age / p.life;
    let c = corner(v);
    o.pos = place(c, vec2<f32>(mix(p.size_start, p.size_end, t)), p.angle, p.pos);
    let f = frames[p.frame];
    o.uv = mix(f.xy, f.zw, c);
    var tint = unpack4x8unorm(p.color);
    tint.a *= clamp((1.0 - t) * 2.0, 0.0, 1.0); // fade out over the second half of life
    o.color = vec4<f32>(tint.rgb * tint.a, tint.a);
    return o;
}

@fragment
fn fs(o: Out) -> @location(0) vec4<f32> {
    let t = textureSample(sheet, samp, o.uv);
    let c = vec4<f32>(t.rgb * t.a, t.a) * o.color;
    if c.a < 0.004 {
        discard;
    }
    return c;
}
