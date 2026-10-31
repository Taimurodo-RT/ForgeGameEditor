//! Draws a streamed tile world, as `TilemapRenderer` in engine/render. Each visible chunk's tiles live in one big GPU
//! buffer ("slots"); every frame the renderer uploads only chunks that are new or changed and draws all visible
//! chunks of a layer with a single instanced draw call. The fragment shader looks each tile up in the atlas, so the
//! cost depends on screen pixels, not on how many tiles are visible.

use crate::camera::Camera2D;
use crate::gpu;
use crate::light::Color;
use bytemuck::{Pod, Zeroable};
use forge_world::world::{Event, LAYERS, SLOT_TILES};
use forge_world::{CHUNK, CHUNK_TILES, World};
use std::collections::HashMap;

/// Chunks sent to the GPU per frame at most; the rest wait for the next frame.
const UPLOAD_BUDGET: u32 = 192;
/// Vulkan only guarantees 128 MiB per storage buffer binding.
const MAX_TILE_BUFFER_BYTES: u64 = 128 << 20;
/// Bytes between the layers' uniforms (the dynamic offset's alignment).
const LAYER_STRIDE: u64 = 512;

/// A square texture of cells_per_row × cells_per_row tiles, each cell_px pixels, RGBA8. Tile id N is cell N, row by
/// row; ids past the last cell show nothing.
pub struct TileAtlas<'a> {
    pub rgba: &'a [u8],
    pub cell_px: u32,
    pub cells_per_row: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TilemapStats {
    /// Ready chunks on screen.
    pub visible_chunks: u32,
    /// Of those, already on the GPU.
    pub drawn_chunks: u32,
    /// Chunks sent to the GPU this frame.
    pub uploads: u32,
    pub slots_used: u32,
    pub slots_total: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct ViewUniforms {
    scale: [f32; 2],
    unused: [f32; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct LayerUniforms {
    tint: [f32; 4],
    slot_words: u32,
    layer_words: u32,
    atlas_cells: u32,
    atlas_lod: f32,
    liquid: u32,
    full: u32,
    down: u32,
    plain_from: u32,
    liquid_colors: [[f32; 4]; 16],
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct Instance {
    x: f32,
    y: f32,
    slot: u32,
}

struct Slot {
    index: u32,
    revision: u32,
    /// Frame it was last on screen.
    last_seen: u64,
    uploaded: bool,
}

fn rgba(c: Color) -> [f32; 4] {
    [c.r, c.g, c.b, c.a]
}

pub struct TilemapRenderer {
    pipeline: wgpu::RenderPipeline,
    tiles: wgpu::Buffer,
    instances: wgpu::Buffer,
    view: wgpu::Buffer,
    layers: wgpu::Buffer,
    main_group: wgpu::BindGroup,
    layer_group: wgpu::BindGroup,
    max_slots: u32,
    atlas_cells: u32,
    atlas_cell_px: u32,
    atlas_levels: u32,
    slots: HashMap<(i32, i32), Slot>,
    free_slots: Vec<u32>,
    tints: [Color; LAYERS],
    liquid_layers: [bool; LAYERS],
    liquid_colors: [Color; 16],
    liquid_full: u32,
    liquid_down: u32,
    plain_from: u32,
    frame_instances: Vec<Instance>,
    draw_count: u32,
    frame: u64,
    stats: TilemapStats,
}

impl TilemapRenderer {
    /// `max_slots` bounds how many chunks can be on screen at once (and the GPU memory: max_slots × layers × 8 KiB).
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        format: wgpu::TextureFormat,
        atlas: &TileAtlas,
        max_slots: u32,
    ) -> TilemapRenderer {
        let slot_bytes = (SLOT_TILES * 2) as u64;
        let max_slots = (max_slots as u64)
            .min(MAX_TILE_BUFFER_BYTES / slot_bytes)
            .min(device.limits().max_storage_buffer_binding_size / slot_bytes) as u32;
        use wgpu::BufferUsages as U;
        let tiles = gpu::buffer(device, "tiles", max_slots as u64 * slot_bytes, U::STORAGE | U::COPY_DST);
        let instances = gpu::buffer(
            device,
            "chunk instances",
            max_slots as u64 * std::mem::size_of::<Instance>() as u64,
            U::VERTEX | U::COPY_DST,
        );
        let view =
            gpu::buffer(device, "tile view", std::mem::size_of::<ViewUniforms>() as u64, U::UNIFORM | U::COPY_DST);
        let layers = gpu::buffer(device, "tile layers", LAYER_STRIDE * LAYERS as u64, U::UNIFORM | U::COPY_DST);

        // Mips down to one texel per cell: far away, a tile becomes its average colour.
        let size = atlas.cell_px * atlas.cells_per_row;
        let mut atlas_levels = 1;
        while (atlas.cell_px >> (atlas_levels - 1)) > 1 {
            atlas_levels += 1;
        }
        let levels = gpu::mip_levels(atlas.rgba, size, size, atlas_levels);
        let texture = gpu::texture_rgba8(device, queue, "tile atlas", &levels, size, size);
        let atlas_view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("tile atlas"),
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            ..Default::default()
        });

        use wgpu::ShaderStages as S;
        let main_layout = gpu::layout(
            device,
            "tiles",
            &[
                (0, S::VERTEX, gpu::uniform(false)),
                (1, S::FRAGMENT, gpu::texture()),
                (2, S::FRAGMENT, gpu::sampler()),
                (3, S::FRAGMENT, gpu::storage(true)),
            ],
        );
        let layer_layout = gpu::layout(device, "tile layer", &[(0, S::FRAGMENT, gpu::uniform(true))]);
        let main_group = gpu::bind_group(
            device,
            "tiles",
            &main_layout,
            &[
                view.as_entire_binding(),
                wgpu::BindingResource::TextureView(&atlas_view),
                wgpu::BindingResource::Sampler(&sampler),
                tiles.as_entire_binding(),
            ],
        );
        let layer_group = gpu::bind_group(
            device,
            "tile layer",
            &layer_layout,
            &[wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                buffer: &layers,
                offset: 0,
                size: wgpu::BufferSize::new(std::mem::size_of::<LayerUniforms>() as u64),
            })],
        );
        let module = gpu::shader(device, "tilemap", include_str!("shaders/tilemap.wgsl"));
        let pipeline = gpu::render_pipeline(
            device,
            "tilemap",
            &gpu::pipeline_layout(device, "tilemap", &[&main_layout, &layer_layout]),
            &module,
            "vs",
            "fs",
            &[Some(wgpu::VertexBufferLayout {
                array_stride: std::mem::size_of::<Instance>() as u64,
                step_mode: wgpu::VertexStepMode::Instance,
                attributes: &wgpu::vertex_attr_array![0 => Float32x2, 1 => Uint32],
            })],
            format,
            gpu::STRAIGHT_ALPHA,
        );
        let mut tints = [Color::rgb(1.0, 1.0, 1.0); LAYERS];
        // Back layer (walls, ground under objects) a little darker by default.
        if LAYERS > 1 {
            tints[0] = Color::rgb(0.55, 0.55, 0.6);
        }
        log::info!(
            "tilemap: {max_slots} chunk slots, {:.1} MiB of tiles on the GPU",
            max_slots as f64 * slot_bytes as f64 / 1048576.0
        );
        TilemapRenderer {
            pipeline,
            tiles,
            instances,
            view,
            layers,
            main_group,
            layer_group,
            max_slots,
            atlas_cells: atlas.cells_per_row,
            atlas_cell_px: atlas.cell_px,
            atlas_levels,
            slots: HashMap::new(),
            free_slots: (0..max_slots).rev().collect(),
            tints,
            liquid_layers: [false; LAYERS],
            liquid_colors: [Color::default(); 16],
            liquid_full: 1024,
            liquid_down: 0,
            plain_from: 0,
            frame_instances: Vec::new(),
            draw_count: 0,
            frame: 0,
            stats: TilemapStats { slots_total: max_slots, ..Default::default() },
        }
    }

    pub fn set_layer_tint(&mut self, layer: usize, tint: Color) {
        if layer < LAYERS {
            self.tints[layer] = tint;
        }
    }

    /// Tiles with ids from here on are drawn in their own colours on every layer, the layer's tint left out
    /// (pictures that came with a level, say); 0: every tile takes its layer's tint.
    pub fn set_plain_from(&mut self, id: u16) {
        self.plain_from = id as u32;
    }

    /// Draws a layer as liquids: `colors[kind]` for kinds 1..15, cells filled from the side gravity points to.
    pub fn set_layer_liquid(&mut self, layer: usize, colors: &[Color], full: u32) {
        if layer >= LAYERS {
            return;
        }
        self.liquid_layers[layer] = true;
        self.liquid_full = full;
        for (i, c) in self.liquid_colors.iter_mut().enumerate() {
            *c = colors.get(i).copied().unwrap_or_default();
        }
    }

    /// Where liquids settle: 0 +y, 1 -x, 2 -y, 3 +x.
    pub fn set_liquid_down(&mut self, down: u32) {
        self.liquid_down = down & 3;
    }

    pub fn stats(&self) -> TilemapStats {
        self.stats
    }

    /// Frees the slots of chunks that left memory. Call with the world's events before they are cleared.
    pub fn on_world_events(&mut self, events: &[Event]) {
        for e in events {
            if let Event::Unloading(c) = e
                && let Some(s) = self.slots.remove(c)
            {
                self.free_slots.push(s.index);
            }
        }
    }

    fn acquire_slot(&mut self, coord: (i32, i32)) -> bool {
        if self.slots.contains_key(&coord) {
            return true;
        }
        let index = match self.free_slots.pop() {
            Some(i) => i,
            None => {
                // Every slot is taken: reuse the one off screen the longest.
                let frame = self.frame;
                let Some(oldest) = self
                    .slots
                    .iter()
                    .filter(|(_, s)| s.last_seen < frame)
                    .min_by_key(|(_, s)| s.last_seen)
                    .map(|(c, _)| *c)
                else {
                    return false; // all on screen this frame
                };
                self.slots.remove(&oldest).unwrap().index
            }
        };
        self.slots.insert(coord, Slot { index, revision: 0, last_seen: 0, uploaded: false });
        true
    }

    /// Each frame, before the render pass: uploads changed chunks and the list of chunks to draw.
    pub fn prepare(&mut self, queue: &wgpu::Queue, world: &World, camera: &Camera2D, width: u32, height: u32) {
        self.frame += 1;
        self.frame_instances.clear();
        self.stats.visible_chunks = 0;
        let mut uploads = 0;
        // Snap the camera to whole pixels so tiles do not shimmer while scrolling.
        let (cx, cy) = (camera.snapped_x(), camera.snapped_y());
        let tiles = camera.visible_tiles(width, height);
        let (x0, y0) = (tiles.x0.div_euclid(CHUNK), tiles.y0.div_euclid(CHUNK));
        let (x1, y1) = ((tiles.x1 - 1).div_euclid(CHUNK) + 1, (tiles.y1 - 1).div_euclid(CHUNK) + 1);
        let slot_bytes = (SLOT_TILES * 2) as u64;
        for y in y0..y1 {
            for x in x0..x1 {
                let Some(chunk) = world.chunks.get(&(x, y)).filter(|c| c.ready) else { continue };
                self.stats.visible_chunks += 1;
                if !self.acquire_slot((x, y)) {
                    continue;
                }
                let frame = self.frame;
                let slot = self.slots.get_mut(&(x, y)).unwrap();
                slot.last_seen = frame;
                if (!slot.uploaded || slot.revision != chunk.revision) && uploads < UPLOAD_BUDGET {
                    queue.write_buffer(&self.tiles, slot.index as u64 * slot_bytes, bytemuck::cast_slice(&chunk.tiles));
                    slot.uploaded = true;
                    slot.revision = chunk.revision;
                    uploads += 1;
                }
                if !slot.uploaded {
                    continue;
                }
                self.frame_instances.push(Instance {
                    x: (x as f64 * CHUNK as f64 - cx) as f32,
                    y: (y as f64 * CHUNK as f64 - cy) as f32,
                    slot: slot.index,
                });
            }
        }
        self.draw_count = self.frame_instances.len() as u32;
        self.stats.drawn_chunks = self.draw_count;
        self.stats.uploads = uploads;
        self.stats.slots_used = self.max_slots - self.free_slots.len() as u32;
        if self.draw_count > 0 {
            queue.write_buffer(&self.instances, 0, bytemuck::cast_slice(&self.frame_instances));
        }
        let zoom = camera.zoom as f64;
        let view = ViewUniforms {
            scale: [(2.0 * zoom / width as f64) as f32, (-2.0 * zoom / height as f64) as f32],
            unused: [0.0; 2],
        };
        queue.write_buffer(&self.view, 0, bytemuck::bytes_of(&view));
        let lod = (self.atlas_cell_px as f32 / camera.zoom).log2().clamp(0.0, (self.atlas_levels - 1) as f32);
        for layer in 0..LAYERS {
            let u = LayerUniforms {
                tint: rgba(self.tints[layer]),
                slot_words: (slot_bytes / 4) as u32,
                layer_words: (layer * CHUNK_TILES / 2) as u32,
                atlas_cells: self.atlas_cells,
                atlas_lod: lod,
                liquid: self.liquid_layers[layer] as u32,
                full: self.liquid_full,
                down: self.liquid_down,
                plain_from: self.plain_from,
                liquid_colors: self.liquid_colors.map(rgba),
            };
            queue.write_buffer(&self.layers, layer as u64 * LAYER_STRIDE, bytemuck::bytes_of(&u));
        }
    }

    /// Inside a render pass on the target: every layer, back to front.
    pub fn draw(&self, pass: &mut wgpu::RenderPass) {
        self.draw_layers(pass, 0, LAYERS);
    }

    /// Only layers [first, first + count) (liquids after the sprites, say).
    pub fn draw_layers(&self, pass: &mut wgpu::RenderPass, first: usize, count: usize) {
        if self.draw_count == 0 || first >= LAYERS {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &self.main_group, &[]);
        pass.set_vertex_buffer(0, self.instances.slice(..));
        for layer in first..LAYERS.min(first + count) {
            pass.set_bind_group(1, &self.layer_group, &[(layer as u64 * LAYER_STRIDE) as u32]);
            pass.draw(0..6, 0..self.draw_count);
        }
    }
}
