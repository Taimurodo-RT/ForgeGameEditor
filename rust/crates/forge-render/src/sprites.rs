//! Draws a `SpriteBatch`, as `SpriteRenderer` in engine/render: copied to upload memory and (when sorted) culled
//! and sorted on the processor in parallel, then one upload and one draw call for all of it.

use crate::camera::Camera2D;
use crate::gpu;
use crate::sprite_batch::{Sprite, SpriteBatch, ViewBox};
use bytemuck::{Pod, Zeroable};
use std::time::Instant;

/// A picture inside the sprite sheet, in pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SpriteRect {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

/// One RGBA8 picture holding every frame (straight, not premultiplied, alpha).
pub struct SpriteSheet<'a> {
    pub rgba: &'a [u8],
    pub width: u32,
    pub height: u32,
    pub frames: &'a [SpriteRect],
    /// Linear filtering; off keeps pixel art crisp.
    pub smooth: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SpriteStats {
    pub submitted: u32,
    pub drawn: u32,
    pub dropped: u32,
    /// Copy to upload memory, cull and sort.
    pub cpu_ms: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub(crate) struct ViewUniforms {
    pub scale: [f32; 2],
    pub offset: [f32; 2],
    pub sorted: u32,
    pub pad: [u32; 3],
}

/// Frame rectangles as texture coordinates (u0, v0, u1, v1), `inset` texels inside.
pub(crate) fn frame_uvs(sheet: &SpriteSheet, inset: f32) -> Vec<f32> {
    let (w, h) = (sheet.width as f32, sheet.height as f32);
    let (hx, hy) = (inset / w, inset / h);
    let mut uv: Vec<f32> = sheet
        .frames
        .iter()
        .flat_map(|r| {
            [r.x as f32 / w + hx, r.y as f32 / h + hy, (r.x + r.w) as f32 / w - hx, (r.y + r.h) as f32 / h - hy]
        })
        .collect();
    if uv.is_empty() {
        uv.resize(4, 0.0);
    }
    uv
}

/// The sheet's bind group: view, frames, picture, sampler (group 0 of sprites.wgsl).
pub(crate) struct SheetBinding {
    pub layout: wgpu::BindGroupLayout,
    pub group: wgpu::BindGroup,
    pub view: wgpu::Buffer,
}

pub(crate) fn sheet_binding(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    label: &str,
    sheet: &SpriteSheet,
    inset: f32,
    smooth: bool,
) -> SheetBinding {
    use wgpu::BufferUsages as U;
    use wgpu::ShaderStages as S;
    let layout = gpu::layout(
        device,
        label,
        &[
            (0, S::VERTEX, gpu::uniform(false)),
            (1, S::VERTEX, gpu::storage(true)),
            (2, S::FRAGMENT, gpu::texture()),
            (3, S::FRAGMENT, gpu::sampler()),
        ],
    );
    let uv = frame_uvs(sheet, inset);
    let frames = gpu::buffer(device, label, (uv.len() * 4) as u64, U::STORAGE | U::COPY_DST);
    queue.write_buffer(&frames, 0, bytemuck::cast_slice(&uv));
    let view = gpu::buffer(device, label, std::mem::size_of::<ViewUniforms>() as u64, U::UNIFORM | U::COPY_DST);
    let texture = gpu::texture_rgba8(device, queue, label, &[sheet.rgba.to_vec()], sheet.width, sheet.height);
    let texture_view = texture.create_view(&wgpu::TextureViewDescriptor::default());
    let filter = if smooth { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest };
    let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some(label),
        mag_filter: filter,
        min_filter: filter,
        ..Default::default()
    });
    let group = gpu::bind_group(
        device,
        label,
        &layout,
        &[
            view.as_entire_binding(),
            frames.as_entire_binding(),
            wgpu::BindingResource::TextureView(&texture_view),
            wgpu::BindingResource::Sampler(&sampler),
        ],
    );
    SheetBinding { layout, group, view }
}

pub struct SpriteRenderer {
    pipeline: wgpu::RenderPipeline,
    sheet: SheetBinding,
    sprites: wgpu::Buffer,
    order: wgpu::Buffer,
    sprite_group: wgpu::BindGroup,
    sprite_layout: wgpu::BindGroupLayout,
    upload: Vec<Sprite>,
    indices: Vec<u32>,
    max_sprites: u32,
    draw_count: u32,
    stats: SpriteStats,
}

impl SpriteRenderer {
    /// `max_sprites` bounds how many can be drawn in one frame (36 bytes each, twice: upload memory and GPU memory).
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        format: wgpu::TextureFormat,
        sheet: &SpriteSheet,
        max_sprites: u32,
    ) -> SpriteRenderer {
        use wgpu::BufferUsages as U;
        use wgpu::ShaderStages as S;
        let binding = sheet_binding(device, queue, "sprite sheet", sheet, 0.0, sheet.smooth);
        let sprites = gpu::buffer(device, "sprites", max_sprites as u64 * 32, U::STORAGE | U::COPY_DST);
        let order = gpu::buffer(device, "sprite order", max_sprites as u64 * 4, U::STORAGE | U::COPY_DST);
        let sprite_layout =
            gpu::layout(device, "sprites", &[(0, S::VERTEX, gpu::storage(true)), (1, S::VERTEX, gpu::storage(true))]);
        let sprite_group = gpu::bind_group(
            device,
            "sprites",
            &sprite_layout,
            &[sprites.as_entire_binding(), order.as_entire_binding()],
        );
        let module = gpu::shader(device, "sprites", include_str!("shaders/sprites.wgsl"));
        let pipeline = gpu::render_pipeline(
            device,
            "sprites",
            &gpu::pipeline_layout(device, "sprites", &[&binding.layout, &sprite_layout]),
            &module,
            "sprite_vs",
            "fs",
            &[],
            format,
            gpu::premultiplied(false),
        );
        log::info!(
            "sprites: up to {max_sprites} per frame, {} frames in a {}x{} sheet",
            sheet.frames.len(),
            sheet.width,
            sheet.height
        );
        SpriteRenderer {
            pipeline,
            sheet: binding,
            sprites,
            order,
            sprite_group,
            sprite_layout,
            upload: vec![Sprite::default(); max_sprites as usize],
            indices: vec![0; max_sprites as usize],
            max_sprites,
            draw_count: 0,
            stats: SpriteStats::default(),
        }
    }

    /// Swaps the sheet for another (more frames, other pictures). Frame numbers already in use keep pointing at
    /// their rects.
    pub fn set_sheet(&mut self, device: &wgpu::Device, queue: &wgpu::Queue, sheet: &SpriteSheet) {
        let binding = sheet_binding(device, queue, "sprite sheet", sheet, 0.0, sheet.smooth);
        // The pipeline's layout stays valid: the new group has the same shape.
        let _ = &self.sprite_layout;
        self.sheet.group = binding.group;
        self.sheet.view = binding.view;
    }

    pub fn stats(&self) -> SpriteStats {
        self.stats
    }

    pub fn max_sprites(&self) -> u32 {
        self.max_sprites
    }

    /// Each frame, before the render pass. `sorted` false keeps push order (cheaper; fine for effects where order
    /// does not matter).
    pub fn prepare(
        &mut self,
        queue: &wgpu::Queue,
        batch: &mut SpriteBatch,
        camera: &Camera2D,
        width: u32,
        height: u32,
        sorted: bool,
    ) {
        let t0 = Instant::now();
        let (cx, cy) = (camera.snapped_x(), camera.snapped_y());
        let zoom = camera.zoom as f64;
        let half_w = width as f64 / (2.0 * zoom);
        let half_h = height as f64 / (2.0 * zoom);
        let (ox, oy) = batch.origin();
        let view = ViewBox {
            x0: (cx - half_w - ox) as f32,
            y0: (cy - half_h - oy) as f32,
            x1: (cx + half_w - ox) as f32,
            y1: (cy + half_h - oy) as f32,
        };
        let u = ViewUniforms {
            scale: [(2.0 * zoom / width as f64) as f32, (-2.0 * zoom / height as f64) as f32],
            offset: [(ox - cx) as f32, (oy - cy) as f32],
            sorted: sorted as u32,
            pad: [0; 3],
        };
        queue.write_buffer(&self.sheet.view, 0, bytemuck::bytes_of(&u));
        let list = if batch.is_empty() {
            Default::default()
        } else {
            batch.finish(&view, sorted, &mut self.upload, &mut self.indices)
        };
        self.draw_count = list.draws;
        if list.sprites > 0 {
            queue.write_buffer(&self.sprites, 0, bytemuck::cast_slice(&self.upload[..list.sprites as usize]));
            if sorted && list.draws > 0 {
                queue.write_buffer(&self.order, 0, bytemuck::cast_slice(&self.indices[..list.draws as usize]));
            }
        }
        let bs = batch.stats();
        self.stats = SpriteStats {
            submitted: bs.submitted,
            drawn: self.draw_count,
            dropped: bs.dropped,
            cpu_ms: t0.elapsed().as_secs_f64() * 1000.0,
        };
    }

    pub fn draw(&self, pass: &mut wgpu::RenderPass) {
        if self.draw_count == 0 {
            return;
        }
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, &self.sheet.group, &[]);
        pass.set_bind_group(1, &self.sprite_group, &[]);
        pass.draw(0..6, 0..self.draw_count);
    }
}
