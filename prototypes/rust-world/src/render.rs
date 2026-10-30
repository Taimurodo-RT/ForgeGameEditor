//! wgpu renderer: tile chunks (storage buffer of chunk slots, one instance per chunk, as TilemapRenderer in C++),
//! creature sprites and GPU particles.

use crate::worldgen::CHUNK;
use crate::world::{Event, SLOT_TILES, World};
use bytemuck::{Pod, Zeroable};
use std::collections::HashMap;

const UPLOAD_BUDGET: usize = 192; // chunks sent to the GPU per frame at most, as kUploadBudget
const MAX_TILE_BYTES: u64 = 128 << 20;
const ATLAS_CELL: u32 = 16;
const ATLAS_CELLS: u32 = 16;

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct ViewU {
    pub scale: [f32; 2],
    pub cam: [f32; 2],
    pub px: f32,
    pub dt: f32,
    pub count: u32,
    pub _pad: u32,
    pub area: [f32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct LayerU {
    tint: [f32; 4],
    slot_words: u32,
    layer_words: u32,
    atlas_cells: u32,
    atlas_lod: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Instance {
    origin: [f32; 2],
    slot: u32,
}

struct Slot {
    index: u32,
    revision: u32,
    last_seen: u64,
}

pub struct Camera {
    pub x: f64,
    pub y: f64,
    pub zoom: f32, // pixels per tile
}

pub struct Renderer {
    tile_pipe: wgpu::RenderPipeline,
    sprite_pipe: wgpu::RenderPipeline,
    particle_pipe: wgpu::RenderPipeline,
    particle_cs: wgpu::ComputePipeline,
    view_buf: wgpu::Buffer,
    layer_bufs: [wgpu::Buffer; 2],
    layer_groups: [wgpu::BindGroup; 2],
    tiles_buf: wgpu::Buffer,
    main_group: wgpu::BindGroup,
    particle_rw_group: wgpu::BindGroup,
    inst_buf: wgpu::Buffer,
    sprite_buf: wgpu::Buffer,
    sprite_cap: usize,
    sprites: u32,
    particles: u32,
    slots: HashMap<(i32, i32), Slot>,
    free: Vec<u32>,
    max_slots: u32,
    frame: u64,
    instances: Vec<Instance>,
    pub drawn_chunks: u32,
    pub uploads: usize,
    pub clear: wgpu::Color,
    pub tints: [[f32; 4]; 2],
}

fn atlas_pixels() -> Vec<Vec<u8>> {
    // Flat colours per tile id with a light speckle, like a real tile set at a glance.
    let colors: [[u8; 3]; 17] = [
        [0, 0, 0], [86, 170, 60], [134, 96, 62], [120, 120, 128], [220, 200, 140], [190, 110, 60], [170, 160, 150],
        [230, 190, 60], [90, 64, 44], [70, 70, 78], [60, 120, 220], [30, 70, 160], [110, 180, 80], [40, 110, 50],
        [20, 80, 30], [130, 130, 130], [235, 240, 250],
    ];
    let size = ATLAS_CELL * ATLAS_CELLS;
    let mut base = vec![0u8; (size * size * 4) as usize];
    for id in 1..17u32 {
        let (cx, cy) = (id % ATLAS_CELLS, id / ATLAS_CELLS);
        for y in 0..ATLAS_CELL {
            for x in 0..ATLAS_CELL {
                let speck = (crate::worldgen::hash_tile(id as u64, x as i32, y as i32) % 7) as i32 - 3;
                let c = colors[id as usize];
                let i = (((cy * ATLAS_CELL + y) * size + cx * ATLAS_CELL + x) * 4) as usize;
                for k in 0..3 {
                    base[i + k] = (c[k] as i32 + speck * 6).clamp(0, 255) as u8;
                }
                base[i + 3] = 255;
            }
        }
    }
    let mut levels = vec![base];
    let mut s = size;
    while s > 1 {
        let prev = levels.last().unwrap();
        let n = s / 2;
        let mut next = vec![0u8; (n * n * 4) as usize];
        for y in 0..n {
            for x in 0..n {
                for k in 0..4 {
                    let p = |xx: u32, yy: u32| prev[((yy * s + xx) * 4 + k) as usize] as u32;
                    next[((y * n + x) * 4 + k) as usize] = ((p(2 * x, 2 * y) + p(2 * x + 1, 2 * y) + p(2 * x, 2 * y + 1) + p(2 * x + 1, 2 * y + 1)) / 4) as u8;
                }
            }
        }
        levels.push(next);
        s = n;
    }
    levels
}

impl Renderer {
    pub fn new(device: &wgpu::Device, queue: &wgpu::Queue, format: wgpu::TextureFormat, sprite_cap: usize, particles: u32, area: [f32; 4]) -> Self {
        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("shaders"),
            source: wgpu::ShaderSource::Wgsl(include_str!("shaders.wgsl").into()),
        });
        let slot_bytes = (SLOT_TILES * 2) as u64;
        let max_slots = (MAX_TILE_BYTES / slot_bytes).min(device.limits().max_storage_buffer_binding_size as u64 / slot_bytes) as u32;
        let tiles_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("tiles"),
            size: max_slots as u64 * slot_bytes,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let inst_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("chunk instances"),
            size: max_slots as u64 * std::mem::size_of::<Instance>() as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let sprite_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("sprites"),
            size: (sprite_cap.max(1) * 12) as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let view_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("view"),
            size: std::mem::size_of::<ViewU>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Particles: random positions and speeds inside the area.
        let mut seed = 7u32;
        let mut rnd = || {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed as f32 / u32::MAX as f32
        };
        let mut pdata = Vec::with_capacity(particles.max(1) as usize * 4);
        for _ in 0..particles.max(1) {
            pdata.extend_from_slice(&[
                area[0] + rnd() * (area[2] - area[0]),
                area[1] + rnd() * (area[3] - area[1]),
                (rnd() - 0.5) * 40.0,
                (rnd() - 0.5) * 40.0,
            ]);
        }
        let particle_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("particles"),
            size: (pdata.len() * 4) as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&particle_buf, 0, bytemuck::cast_slice(&pdata));

        let levels = atlas_pixels();
        let size = ATLAS_CELL * ATLAS_CELLS;
        let atlas = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("atlas"),
            size: wgpu::Extent3d { width: size, height: size, depth_or_array_layers: 1 },
            mip_level_count: levels.len() as u32,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });
        for (level, data) in levels.iter().enumerate() {
            let s = (size >> level).max(1);
            queue.write_texture(
                wgpu::TexelCopyTextureInfo { texture: &atlas, mip_level: level as u32, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
                data,
                wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(4 * s), rows_per_image: Some(s) },
                wgpu::Extent3d { width: s, height: s, depth_or_array_layers: 1 },
            );
        }
        let atlas_view = atlas.create_view(&wgpu::TextureViewDescriptor::default());
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });

        let vis = wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT | wgpu::ShaderStages::COMPUTE;
        let entry = |binding, ty| wgpu::BindGroupLayoutEntry { binding, visibility: vis, ty, count: None };
        let uniform = wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None };
        let storage = |read_only| wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Storage { read_only }, has_dynamic_offset: false, min_binding_size: None };
        let draw_vis = wgpu::ShaderStages::VERTEX | wgpu::ShaderStages::FRAGMENT;
        let main_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("main"),
            entries: &[
                entry(0, uniform),
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: draw_vis,
                    ty: wgpu::BindingType::Texture { sample_type: wgpu::TextureSampleType::Float { filterable: true }, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry { binding: 2, visibility: draw_vis, ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering), count: None },
                wgpu::BindGroupLayoutEntry { binding: 3, visibility: draw_vis, ty: storage(true), count: None },
                wgpu::BindGroupLayoutEntry { binding: 4, visibility: wgpu::ShaderStages::VERTEX, ty: storage(true), count: None },
            ],
        });
        let compute_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("particles rw"),
            entries: &[entry(0, uniform), wgpu::BindGroupLayoutEntry { binding: 4, visibility: wgpu::ShaderStages::COMPUTE, ty: storage(false), count: None }],
        });
        let layer_layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("layer"),
            entries: &[wgpu::BindGroupLayoutEntry { binding: 0, visibility: draw_vis, ty: uniform, count: None }],
        });
        let main_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("main"),
            layout: &main_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: view_buf.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&atlas_view) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(&sampler) },
                wgpu::BindGroupEntry { binding: 3, resource: tiles_buf.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: particle_buf.as_entire_binding() },
            ],
        });
        let particle_rw_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("particles rw"),
            layout: &compute_layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: view_buf.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 4, resource: particle_buf.as_entire_binding() },
            ],
        });
        let make_layer = || {
            device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("layer"),
                size: std::mem::size_of::<LayerU>() as u64,
                usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            })
        };
        let layer_bufs = [make_layer(), make_layer()];
        let layer_group = |b: &wgpu::Buffer| {
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("layer"),
                layout: &layer_layout,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: b.as_entire_binding() }],
            })
        };
        let layer_groups = [layer_group(&layer_bufs[0]), layer_group(&layer_bufs[1])];

        let tile_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("tiles"),
            bind_group_layouts: &[Some(&main_layout), Some(&layer_layout)],
            immediate_size: 0,
        });
        let flat_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("flat"),
            bind_group_layouts: &[Some(&main_layout)],
            immediate_size: 0,
        });
        let blend = Some(wgpu::ColorTargetState { format, blend: Some(wgpu::BlendState::ALPHA_BLENDING), write_mask: wgpu::ColorWrites::ALL });
        let pipe = |layout: &wgpu::PipelineLayout, vs: &str, fs: &str, buffers: &[Option<wgpu::VertexBufferLayout>]| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(vs),
                layout: Some(layout),
                vertex: wgpu::VertexState { module: &shader, entry_point: Some(vs), buffers, compilation_options: Default::default() },
                fragment: Some(wgpu::FragmentState { module: &shader, entry_point: Some(fs), targets: &[blend.clone()], compilation_options: Default::default() }),
                primitive: wgpu::PrimitiveState::default(),
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview_mask: None,
                cache: None,
            })
        };
        let tile_pipe = pipe(
            &tile_layout,
            "tile_vs",
            "tile_fs",
            &[Some(wgpu::VertexBufferLayout {
                array_stride: std::mem::size_of::<Instance>() as u64,
                step_mode: wgpu::VertexStepMode::Instance,
                attributes: &wgpu::vertex_attr_array![0 => Float32x2, 1 => Uint32],
            })],
        );
        let sprite_pipe = pipe(
            &flat_layout,
            "sprite_vs",
            "flat_fs",
            &[Some(wgpu::VertexBufferLayout {
                array_stride: 12,
                step_mode: wgpu::VertexStepMode::Instance,
                attributes: &wgpu::vertex_attr_array![0 => Float32x2, 1 => Uint32],
            })],
        );
        let particle_pipe = pipe(&flat_layout, "particle_vs", "flat_fs", &[]);
        let cs_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("particles cs"),
            bind_group_layouts: &[Some(&compute_layout)],
            immediate_size: 0,
        });
        let particle_cs = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("particles"),
            layout: Some(&cs_layout),
            module: &shader,
            entry_point: Some("particles_cs"),
            compilation_options: Default::default(),
            cache: None,
        });

        Self {
            tile_pipe,
            sprite_pipe,
            particle_pipe,
            particle_cs,
            view_buf,
            layer_bufs,
            layer_groups,
            tiles_buf,
            main_group,
            particle_rw_group,
            inst_buf,
            sprite_buf,
            sprite_cap,
            sprites: 0,
            particles,
            slots: HashMap::new(),
            free: (0..max_slots).rev().collect(),
            max_slots,
            frame: 0,
            instances: Vec::new(),
            drawn_chunks: 0,
            uploads: 0,
            clear: wgpu::Color { r: 0.53, g: 0.74, b: 0.92, a: 1.0 },
            tints: [[0.55, 0.55, 0.6, 1.0], [1.0; 4]],
        }
    }

    pub fn max_slots(&self) -> u32 {
        self.max_slots
    }

    /// Frees GPU slots of chunks that left memory. Call before the world's events are cleared.
    pub fn on_world_events(&mut self, world: &World) {
        for e in &world.events {
            if let Event::Unloading(c) = e {
                if let Some(s) = self.slots.remove(c) {
                    self.free.push(s.index);
                }
            }
        }
    }

    fn acquire_slot(&mut self, coord: (i32, i32)) -> Option<u32> {
        if let Some(s) = self.slots.get(&coord) {
            return Some(s.index);
        }
        let index = if let Some(i) = self.free.pop() {
            i
        } else {
            let frame = self.frame;
            let oldest = self.slots.iter().filter(|(_, s)| s.last_seen < frame).min_by_key(|(_, s)| s.last_seen).map(|(c, _)| *c)?;
            self.slots.remove(&oldest).unwrap().index
        };
        self.slots.insert(coord, Slot { index, revision: u32::MAX, last_seen: 0 });
        Some(index)
    }

    #[allow(clippy::too_many_arguments)]
    pub fn prepare(&mut self, queue: &wgpu::Queue, world: &World, cam: &Camera, width: u32, height: u32, sprites: &[[u32; 3]], dt: f32, area: [f32; 4]) {
        self.frame += 1;
        self.uploads = 0;
        self.instances.clear();
        let (hw, hh) = (width as f64 / 2.0 / cam.zoom as f64, height as f64 / 2.0 / cam.zoom as f64);
        let (x0, y0) = (((cam.x - hw) / CHUNK as f64).floor() as i32, ((cam.y - hh) / CHUNK as f64).floor() as i32);
        let (x1, y1) = (((cam.x + hw) / CHUNK as f64).ceil() as i32, ((cam.y + hh) / CHUNK as f64).ceil() as i32);
        let slot_bytes = (SLOT_TILES * 2) as u64;
        for cy in y0..y1 {
            for cx in x0..x1 {
                let Some(chunk) = world.chunks.get(&(cx, cy)) else { continue };
                if !chunk.ready {
                    continue;
                }
                let Some(index) = self.acquire_slot((cx, cy)) else { continue };
                let slot = self.slots.get_mut(&(cx, cy)).unwrap();
                slot.last_seen = self.frame;
                if slot.revision != chunk.revision {
                    if self.uploads >= UPLOAD_BUDGET {
                        if slot.revision == u32::MAX {
                            continue; // never sent: nothing to draw yet
                        }
                    } else {
                        queue.write_buffer(&self.tiles_buf, index as u64 * slot_bytes, bytemuck::cast_slice(&chunk.tiles));
                        slot.revision = chunk.revision;
                        self.uploads += 1;
                    }
                }
                self.instances.push(Instance {
                    origin: [((cx * CHUNK) as f64 - cam.x) as f32, ((cy * CHUNK) as f64 - cam.y) as f32],
                    slot: index,
                });
            }
        }
        self.drawn_chunks = self.instances.len() as u32;
        if !self.instances.is_empty() {
            queue.write_buffer(&self.inst_buf, 0, bytemuck::cast_slice(&self.instances));
        }
        let n = sprites.len().min(self.sprite_cap);
        if n > 0 {
            queue.write_buffer(&self.sprite_buf, 0, bytemuck::cast_slice(&sprites[..n]));
        }
        self.sprites = n as u32;
        let view = ViewU {
            scale: [2.0 * cam.zoom / width as f32, -2.0 * cam.zoom / height as f32],
            cam: [cam.x as f32, cam.y as f32],
            px: 1.0 / cam.zoom,
            dt,
            count: self.particles,
            _pad: 0,
            area,
        };
        queue.write_buffer(&self.view_buf, 0, bytemuck::bytes_of(&view));
        let lod = (ATLAS_CELL as f32 / cam.zoom).log2().max(0.0);
        for l in 0..2 {
            let u = LayerU {
                tint: self.tints[l],
                slot_words: (SLOT_TILES / 2) as u32,
                layer_words: (l * SLOT_TILES / 4) as u32,
                atlas_cells: ATLAS_CELLS,
                atlas_lod: lod,
            };
            queue.write_buffer(&self.layer_bufs[l], 0, bytemuck::bytes_of(&u));
        }
    }

    pub fn draw(&self, encoder: &mut wgpu::CommandEncoder, target: &wgpu::TextureView) {
        if self.particles > 0 {
            let mut cp = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("particles"), timestamp_writes: None });
            cp.set_pipeline(&self.particle_cs);
            cp.set_bind_group(0, &self.particle_rw_group, &[]);
            cp.dispatch_workgroups(self.particles.div_ceil(256), 1, 1);
        }
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("world"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(self.clear), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_bind_group(0, &self.main_group, &[]);
        if self.drawn_chunks > 0 {
            pass.set_pipeline(&self.tile_pipe);
            pass.set_vertex_buffer(0, self.inst_buf.slice(..));
            for l in 0..2 {
                pass.set_bind_group(1, &self.layer_groups[l], &[]);
                pass.draw(0..6, 0..self.drawn_chunks);
            }
        }
        if self.sprites > 0 {
            pass.set_pipeline(&self.sprite_pipe);
            pass.set_vertex_buffer(0, self.sprite_buf.slice(..));
            pass.draw(0..6, 0..self.sprites);
        }
        if self.particles > 0 {
            pass.set_pipeline(&self.particle_pipe);
            pass.draw(0..self.particles * 6, 0..1);
        }
    }
}
