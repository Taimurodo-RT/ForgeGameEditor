//! Light and shadow on the video card, as `LightRenderer` in engine/render: the grid (`LightGrid`, built on the
//! processor) is spread step by step by a compute shader, then multiplies the picture drawn so far.

use crate::camera::Camera2D;
use crate::gpu;
use crate::light::{LightGrid, LightRules, MAX_CELLS_H, MAX_CELLS_W, PointLight, STEPS};
use bytemuck::{Pod, Zeroable};
use forge_world::World;
use std::time::Instant;

const GROUP: u32 = 16;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LightStats {
    pub cells_w: u32,
    pub cells_h: u32,
    /// The grid's first cell, in tiles.
    pub x0: i32,
    pub y0: i32,
    /// Tiles per cell (grows when zoomed far out).
    pub step: u32,
    pub lights: u32,
    /// Of them, with a radius.
    pub lamps: u32,
    /// Building the grid (the lamps with a radius too).
    pub cpu_ms: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct SpreadParams {
    transmit: [f32; 4],
    width: u32,
    height: u32,
    first: u32,
    pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Composite {
    ambient: [f32; 4],
    screen: [f32; 2],
    camera: [f32; 2],
    zoom: f32,
    step: f32,
    width: u32,
    height: u32,
    lamps: u32,
    pad: [u32; 3],
}

pub struct LightRenderer {
    spread: wgpu::ComputePipeline,
    composite: wgpu::RenderPipeline,
    cells: wgpu::Buffer,
    lamps: wgpu::Buffer,
    /// The first step (sources only) and the others.
    params: [wgpu::Buffer; 2],
    params_groups: [wgpu::BindGroup; 2],
    /// Light from buffer 0 into 1, and back.
    ping_pong: [wgpu::BindGroup; 2],
    composite_params: wgpu::Buffer,
    /// Reads the answer from buffer 0 or 1.
    composite_groups: [wgpu::BindGroup; 2],
    rules: LightRules,
    lights: Vec<PointLight>,
    grid: LightGrid,
    result: usize,
    ready: bool,
    stats: LightStats,
}

impl LightRenderer {
    pub fn new(device: &wgpu::Device, format: wgpu::TextureFormat) -> LightRenderer {
        use wgpu::BufferUsages as U;
        use wgpu::ShaderStages as S;
        let cells = (MAX_CELLS_W * MAX_CELLS_H) as u64;
        let cells_buf = gpu::buffer(device, "light cells", cells * 4, U::STORAGE | U::COPY_DST);
        let lamps = gpu::buffer(device, "light lamps", cells * 4, U::STORAGE | U::COPY_DST);
        let light = [
            gpu::buffer(device, "light 0", cells * 16, U::STORAGE),
            gpu::buffer(device, "light 1", cells * 16, U::STORAGE),
        ];
        let params = [
            gpu::buffer(device, "light spread first", 32, U::UNIFORM | U::COPY_DST),
            gpu::buffer(device, "light spread", 32, U::UNIFORM | U::COPY_DST),
        ];
        let composite_params = gpu::buffer(device, "light composite", 64, U::UNIFORM | U::COPY_DST);

        let cells_layout = gpu::layout(
            device,
            "light cells",
            &[(0, S::COMPUTE, gpu::storage(true)), (1, S::COMPUTE, gpu::uniform(false))],
        );
        let ping_layout = gpu::layout(
            device,
            "light ping-pong",
            &[(0, S::COMPUTE, gpu::storage(true)), (1, S::COMPUTE, gpu::storage(false))],
        );
        let params_groups = [0, 1].map(|i| {
            gpu::bind_group(
                device,
                "light cells",
                &cells_layout,
                &[cells_buf.as_entire_binding(), params[i].as_entire_binding()],
            )
        });
        let ping_pong = [0, 1].map(|from| {
            gpu::bind_group(
                device,
                "light ping-pong",
                &ping_layout,
                &[light[from].as_entire_binding(), light[from ^ 1].as_entire_binding()],
            )
        });
        let spread_module = gpu::shader(device, "light spread", include_str!("shaders/light_spread.wgsl"));
        let spread = gpu::compute_pipeline(
            device,
            "light spread",
            &gpu::pipeline_layout(device, "light spread", &[&cells_layout, &ping_layout]),
            &spread_module,
            "spread",
        );

        let composite_layout = gpu::layout(
            device,
            "light composite",
            &[
                (0, S::FRAGMENT, gpu::storage(true)),
                (1, S::FRAGMENT, gpu::storage(true)),
                (2, S::FRAGMENT, gpu::uniform(false)),
            ],
        );
        let composite_groups = [0, 1].map(|i| {
            gpu::bind_group(
                device,
                "light composite",
                &composite_layout,
                &[light[i].as_entire_binding(), lamps.as_entire_binding(), composite_params.as_entire_binding()],
            )
        });
        let composite_module = gpu::shader(device, "light composite", include_str!("shaders/light_composite.wgsl"));
        // picture = picture × light
        let multiply = wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::Zero,
                dst_factor: wgpu::BlendFactor::Src,
                operation: wgpu::BlendOperation::Add,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::Zero,
                dst_factor: wgpu::BlendFactor::One,
                operation: wgpu::BlendOperation::Add,
            },
        };
        let composite = gpu::render_pipeline(
            device,
            "light composite",
            &gpu::pipeline_layout(device, "light composite", &[&composite_layout]),
            &composite_module,
            "composite_vs",
            "composite_fs",
            &[],
            format,
            multiply,
        );
        LightRenderer {
            spread,
            composite,
            cells: cells_buf,
            lamps,
            params,
            params_groups,
            ping_pong,
            composite_params,
            composite_groups,
            rules: LightRules::default(),
            lights: Vec::new(),
            grid: LightGrid::default(),
            result: 0,
            ready: false,
            stats: LightStats::default(),
        }
    }

    pub fn set_rules(&mut self, rules: LightRules) {
        self.rules = rules;
    }

    pub fn rules_mut(&mut self) -> &mut LightRules {
        &mut self.rules
    }

    /// A lamp for this frame (cleared by `prepare`).
    pub fn add(&mut self, light: PointLight) {
        self.lights.push(light);
    }

    pub fn stats(&self) -> LightStats {
        self.stats
    }

    /// The grid of the last `prepare`.
    pub fn grid(&self) -> &LightGrid {
        &self.grid
    }

    /// Each frame, before the render pass: builds the grid and spreads light on the video card.
    pub fn prepare(
        &mut self,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        world: &World,
        camera: &Camera2D,
        width: u32,
        height: u32,
    ) {
        let t0 = Instant::now();
        self.ready = false;
        self.grid = LightGrid::build(world, &self.rules, camera, width, height, &self.lights);
        self.lights.clear();
        let g = &self.grid;
        self.stats = LightStats {
            cells_w: g.w,
            cells_h: g.h,
            x0: g.x0,
            y0: g.y0,
            step: g.step,
            lights: g.lights,
            lamps: g.lamps,
            cpu_ms: 0.0,
        };
        queue.write_buffer(&self.cells, 0, bytemuck::cast_slice(&g.cells));
        if !g.lamp_packed.is_empty() {
            queue.write_buffer(&self.lamps, 0, bytemuck::cast_slice(&g.lamp_packed));
        }
        for (first, buffer) in self.params.iter().enumerate().map(|(i, b)| (i == 0, b)) {
            let p =
                SpreadParams { transmit: self.rules.transmit, width: g.w, height: g.h, first: first as u32, pad: 0 };
            queue.write_buffer(buffer, 0, bytemuck::bytes_of(&p));
        }
        self.stats.cpu_ms = t0.elapsed().as_secs_f64() * 1000.0;

        // Spread: ping-pong between the two light buffers.
        let mut pass = encoder
            .begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("light spread"), timestamp_writes: None });
        pass.set_pipeline(&self.spread);
        let mut from = 0;
        for i in 0..=STEPS {
            pass.set_bind_group(0, &self.params_groups[if i == 0 { 0 } else { 1 }], &[]);
            pass.set_bind_group(1, &self.ping_pong[from], &[]);
            pass.dispatch_workgroups(g.w.div_ceil(GROUP), g.h.div_ceil(GROUP), 1);
            from ^= 1;
        }
        drop(pass);
        self.result = from;

        let c = Composite {
            ambient: [self.rules.ambient.r, self.rules.ambient.g, self.rules.ambient.b, 1.0],
            screen: [width as f32, height as f32],
            camera: [(camera.snapped_x() - g.x0 as f64) as f32, (camera.snapped_y() - g.y0 as f64) as f32],
            zoom: camera.zoom,
            step: g.step as f32,
            width: g.w,
            height: g.h,
            lamps: !g.lamp_packed.is_empty() as u32,
            pad: [0; 3],
        };
        queue.write_buffer(&self.composite_params, 0, bytemuck::bytes_of(&c));
        self.ready = true;
    }

    /// Inside a render pass, after everything that light should touch.
    pub fn draw(&self, pass: &mut wgpu::RenderPass) {
        if !self.ready {
            return;
        }
        pass.set_pipeline(&self.composite);
        pass.set_bind_group(0, &self.composite_groups[self.result], &[]);
        pass.draw(0..3, 0..1);
    }
}
