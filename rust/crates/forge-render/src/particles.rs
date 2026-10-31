//! Particles that live entirely on the video card, as `ParticleSystem` in engine/render: spawned, moved and drawn
//! there, so a million of them cost the processor almost nothing. The processor only says where and how many to
//! spawn each frame. Positions are kept relative to the system's origin, so a system belongs to a place in the world.

use crate::camera::Camera2D;
use crate::gpu;
use crate::sprites::{SheetBinding, SpriteSheet, ViewUniforms, sheet_binding};
use bytemuck::{Pod, Zeroable};

const GROUP: u32 = 256;
const PARTICLE_BYTES: u64 = 48;
const MAX_EMITTERS: usize = 4096;

/// One burst or one frame's worth of a stream. Angles in radians, clockwise on screen (0 = right, π/2 = down).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ParticleEmit {
    /// World tiles.
    pub x: f64,
    pub y: f64,
    pub count: u32,
    /// Spawn anywhere inside this disc.
    pub radius: f32,
    /// Tiles per second.
    pub speed_min: f32,
    pub speed_max: f32,
    /// Direction of flight...
    pub angle: f32,
    /// ...and the width of the cone around it (2π: all around).
    pub spread: f32,
    /// Seconds.
    pub life_min: f32,
    pub life_max: f32,
    /// Tiles.
    pub size_start: f32,
    pub size_end: f32,
    /// Up to this many radians per second, either way.
    pub spin: f32,
    /// RGBA8; fades out over the second half of life.
    pub color: u32,
    /// Picture in the sprite sheet.
    pub frame: u32,
}

impl Default for ParticleEmit {
    fn default() -> Self {
        ParticleEmit {
            x: 0.0,
            y: 0.0,
            count: 0,
            radius: 0.0,
            speed_min: 0.0,
            speed_max: 1.0,
            angle: 0.0,
            spread: std::f32::consts::TAU,
            life_min: 1.0,
            life_max: 2.0,
            size_start: 0.25,
            size_end: 0.25,
            spin: 0.0,
            color: 0xffff_ffff,
            frame: 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ParticleLook {
    /// Tiles per second².
    pub gravity_x: f32,
    pub gravity_y: f32,
    /// Share of speed lost per second.
    pub drag: f32,
    /// Glows add up (fire, sparks); off: normal blending (smoke, rain).
    pub additive: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ParticleStats {
    pub capacity: u32,
    /// Slots ever filled (dead ones are reused in turn).
    pub slots_used: u32,
    /// Last frame.
    pub spawned_last: u32,
    pub spawned_total: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct GpuEmitter {
    x: f32,
    y: f32,
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
    first: u32,
    count: u32,
}

const _: () = assert!(std::mem::size_of::<GpuEmitter>() == 64);

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct EmitParams {
    total: u32,
    emitter_count: u32,
    first_slot: u32,
    capacity: u32,
    seed: u32,
    pad: [u32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct UpdateParams {
    gravity: [f32; 2],
    drag: f32,
    dt: f32,
    count: u32,
    pad: [u32; 3],
}

pub struct ParticleSystem {
    emit_pipeline: wgpu::ComputePipeline,
    update_pipeline: wgpu::ComputePipeline,
    draw_pipeline: wgpu::RenderPipeline,
    particles_group: wgpu::BindGroup,
    emit_group: wgpu::BindGroup,
    update_group: wgpu::BindGroup,
    draw_group: wgpu::BindGroup,
    sheet: SheetBinding,
    emitters: wgpu::Buffer,
    emit_params: wgpu::Buffer,
    update_params: wgpu::Buffer,
    capacity: u32,
    cursor: u32,
    seed: u32,
    origin: (f64, f64),
    look: ParticleLook,
    pending: Vec<GpuEmitter>,
    pending_count: u32,
    stats: ParticleStats,
}

impl ParticleSystem {
    /// `capacity`: most particles alive at once (48 bytes each on the video card). When full, new particles replace
    /// the oldest.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        format: wgpu::TextureFormat,
        sheet: &SpriteSheet,
        capacity: u32,
        origin_x: f64,
        origin_y: f64,
        look: ParticleLook,
    ) -> ParticleSystem {
        use wgpu::BufferUsages as U;
        use wgpu::ShaderStages as S;
        let capacity = capacity.max(GROUP);
        let particles = gpu::buffer(device, "particles", capacity as u64 * PARTICLE_BYTES, U::STORAGE | U::COPY_DST);
        // Every slot starts dead (age >= life).
        let mut dead = vec![0.0f32; capacity as usize * 12];
        for p in dead.chunks_exact_mut(12) {
            p[4] = 1.0; // age 1, life 0
        }
        queue.write_buffer(&particles, 0, bytemuck::cast_slice(&dead));
        let emitters = gpu::buffer(device, "emitters", (MAX_EMITTERS * 64) as u64, U::STORAGE | U::COPY_DST);
        let emit_params = gpu::buffer(device, "emit params", 32, U::UNIFORM | U::COPY_DST);
        let update_params = gpu::buffer(device, "update params", 32, U::UNIFORM | U::COPY_DST);

        let particles_layout = gpu::layout(device, "particles", &[(0, S::COMPUTE, gpu::storage(false))]);
        let emit_layout = gpu::layout(
            device,
            "particle emit",
            &[(0, S::COMPUTE, gpu::storage(true)), (1, S::COMPUTE, gpu::uniform(false))],
        );
        let update_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("particle update"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 2,
                visibility: S::COMPUTE,
                ty: gpu::uniform(false),
                count: None,
            }],
        });
        let particles_group = gpu::bind_group(device, "particles", &particles_layout, &[particles.as_entire_binding()]);
        let emit_group = gpu::bind_group(
            device,
            "particle emit",
            &emit_layout,
            &[emitters.as_entire_binding(), emit_params.as_entire_binding()],
        );
        let update_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("particle update"),
            layout: &update_layout,
            entries: &[wgpu::BindGroupEntry { binding: 2, resource: update_params.as_entire_binding() }],
        });
        let compute = gpu::shader(device, "particles", include_str!("shaders/particles.wgsl"));
        let emit_pipeline = gpu::compute_pipeline(
            device,
            "particle emit",
            &gpu::pipeline_layout(device, "particle emit", &[&particles_layout, &emit_layout]),
            &compute,
            "emit",
        );
        let update_pipeline = gpu::compute_pipeline(
            device,
            "particle update",
            &gpu::pipeline_layout(device, "particle update", &[&particles_layout, &update_layout]),
            &compute,
            "update",
        );

        // Frame rectangles half a texel inside, so smooth filtering does not pick up the neighbouring frame.
        let sheet = sheet_binding(device, queue, "particle sheet", sheet, 0.5, true);
        let draw_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("particle draw"),
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 2,
                visibility: S::VERTEX,
                ty: gpu::storage(true),
                count: None,
            }],
        });
        let draw_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("particle draw"),
            layout: &draw_layout,
            entries: &[wgpu::BindGroupEntry { binding: 2, resource: particles.as_entire_binding() }],
        });
        let module = gpu::shader(device, "sprites", include_str!("shaders/sprites.wgsl"));
        let draw_pipeline = gpu::render_pipeline(
            device,
            "particles",
            &gpu::pipeline_layout(device, "particles", &[&sheet.layout, &draw_layout]),
            &module,
            "particle_vs",
            "fs",
            &[],
            format,
            gpu::premultiplied(look.additive),
        );
        ParticleSystem {
            emit_pipeline,
            update_pipeline,
            draw_pipeline,
            particles_group,
            emit_group,
            update_group,
            draw_group,
            sheet,
            emitters,
            emit_params,
            update_params,
            capacity,
            cursor: 0,
            seed: 1,
            origin: (origin_x, origin_y),
            look,
            pending: Vec::new(),
            pending_count: 0,
            stats: ParticleStats { capacity, ..Default::default() },
        }
    }

    /// Gravity and drag; whether glows add up is fixed when the system is made.
    pub fn set_look(&mut self, look: ParticleLook) {
        self.look = ParticleLook { additive: self.look.additive, ..look };
    }

    pub fn stats(&self) -> ParticleStats {
        self.stats
    }

    pub fn emit(&mut self, e: &ParticleEmit) {
        if e.count == 0 || self.pending.len() >= MAX_EMITTERS {
            return;
        }
        // More than the whole ring in one frame would only overwrite itself.
        let count = e.count.min(self.capacity - self.pending_count.min(self.capacity));
        if count == 0 {
            return;
        }
        self.pending.push(GpuEmitter {
            x: (e.x - self.origin.0) as f32,
            y: (e.y - self.origin.1) as f32,
            radius: e.radius,
            speed_min: e.speed_min,
            speed_max: e.speed_max,
            angle: e.angle,
            spread: e.spread,
            life_min: e.life_min,
            life_max: e.life_max,
            size_start: e.size_start,
            size_end: e.size_end,
            spin: e.spin,
            color: e.color,
            frame: e.frame,
            first: self.pending_count,
            count,
        });
        self.pending_count += count;
    }

    /// Each frame, before the render pass: spawns what was emitted and moves everything by `dt` seconds (both on the
    /// video card).
    pub fn simulate(&mut self, queue: &wgpu::Queue, encoder: &mut wgpu::CommandEncoder, dt: f32) {
        let spawned = self.pending_count;
        self.stats.spawned_last = spawned;
        self.stats.spawned_total += spawned as u64;
        self.stats.slots_used = self.stats.spawned_total.min(self.capacity as u64) as u32;
        let used = self.stats.slots_used;
        let mut pass = encoder
            .begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("particles"), timestamp_writes: None });
        pass.set_bind_group(0, &self.particles_group, &[]);
        if spawned > 0 {
            queue.write_buffer(&self.emitters, 0, bytemuck::cast_slice(&self.pending));
            let p = EmitParams {
                total: spawned,
                emitter_count: self.pending.len() as u32,
                first_slot: self.cursor,
                capacity: self.capacity,
                seed: self.seed,
                pad: [0; 3],
            };
            queue.write_buffer(&self.emit_params, 0, bytemuck::bytes_of(&p));
            pass.set_pipeline(&self.emit_pipeline);
            pass.set_bind_group(1, &self.emit_group, &[]);
            pass.dispatch_workgroups(spawned.div_ceil(GROUP), 1, 1);
            self.cursor = (self.cursor + spawned) % self.capacity;
            self.seed = self.seed.wrapping_mul(1664525).wrapping_add(1013904223);
        }
        if used > 0 && dt > 0.0 {
            let p = UpdateParams {
                gravity: [self.look.gravity_x, self.look.gravity_y],
                drag: self.look.drag,
                dt,
                count: used,
                pad: [0; 3],
            };
            queue.write_buffer(&self.update_params, 0, bytemuck::bytes_of(&p));
            pass.set_pipeline(&self.update_pipeline);
            pass.set_bind_group(1, &self.update_group, &[]);
            pass.dispatch_workgroups(used.div_ceil(GROUP), 1, 1);
        }
        drop(pass);
        self.pending.clear();
        self.pending_count = 0;
    }

    /// Writes where the camera is; before the render pass, once a frame.
    pub fn prepare(&self, queue: &wgpu::Queue, camera: &Camera2D, width: u32, height: u32) {
        let zoom = camera.zoom as f64;
        let u = ViewUniforms {
            scale: [(2.0 * zoom / width as f64) as f32, (-2.0 * zoom / height as f64) as f32],
            offset: [(self.origin.0 - camera.snapped_x()) as f32, (self.origin.1 - camera.snapped_y()) as f32],
            sorted: 0,
            pad: [0; 3],
        };
        queue.write_buffer(&self.sheet.view, 0, bytemuck::bytes_of(&u));
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass) {
        if self.stats.slots_used == 0 {
            return;
        }
        pass.set_pipeline(&self.draw_pipeline);
        pass.set_bind_group(0, &self.sheet.group, &[]);
        pass.set_bind_group(1, &self.draw_group, &[]);
        pass.draw(0..6, 0..self.stats.slots_used);
    }
}
