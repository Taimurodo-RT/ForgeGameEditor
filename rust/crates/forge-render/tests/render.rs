//! The renderers draw on a real video card (software Vulkan on CI): tiles from the atlas with their layer's tint,
//! sprites, particles and light, each checked at known pixels of a picture read back.

use forge_render::*;
use forge_world::{CHUNK, CHUNK_TILES, Generator, Rect, World};

const SIZE: u32 = 128;
const FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Rgba8Unorm;

fn device() -> Option<(wgpu::Device, wgpu::Queue)> {
    let d = offscreen_device();
    if d.is_none() {
        // CI has a software video card; without one the test cannot be skipped there.
        assert!(std::env::var_os("CI").is_none(), "no video card on CI");
        eprintln!("no video card: rendering not checked");
    }
    d
}

/// Chunk (0, 0): front layer red (id 1) where x < 16, green (id 2) where 16 ≤ x < 32 and y < 16; back layer blue
/// (id 3) everywhere.
fn world() -> World {
    let mut w = World::new(Generator::Empty, Rect { x0: -256, y0: -256, x1: 256, y1: 256 });
    for _ in 0..10 {
        w.update(Rect { x0: 0, y0: 0, x1: CHUNK, y1: CHUNK });
        w.finish_loading();
    }
    let mut tiles = vec![3u16; 2 * CHUNK_TILES];
    for y in 0..CHUNK as usize {
        for x in 0..CHUNK as usize {
            tiles[CHUNK_TILES + y * CHUNK as usize + x] = if x < 16 {
                1
            } else if x < 32 && y < 16 {
                2
            } else {
                0
            };
        }
    }
    assert!(w.set_chunk_tiles((0, 0), &tiles, true));
    w
}

/// 4 × 4 cells of 4 pixels: cell 1 red, 2 green, 3 blue.
fn atlas() -> Vec<u8> {
    let colors = [[0, 0, 0, 0], [255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255]];
    let mut rgba = vec![0u8; 16 * 16 * 4];
    for y in 0..16 {
        for x in 0..16 {
            let cell = (y / 4) * 4 + x / 4;
            if cell < 4 {
                rgba[(y * 16 + x) * 4..][..4].copy_from_slice(&colors[cell]);
            }
        }
    }
    rgba
}

const WHITE: [u8; 8 * 8 * 4] = [255; 8 * 8 * 4];
const FRAMES: [SpriteRect; 1] = [SpriteRect { x: 0, y: 0, w: 8, h: 8 }];

fn sheet() -> SpriteSheet<'static> {
    SpriteSheet { rgba: &WHITE, width: 8, height: 8, frames: &FRAMES, smooth: false }
}

/// Camera on tile (32, 32), 2 pixels a tile: the picture shows tiles 0..64; tile (x, y) is around pixel (2x, 2y).
const CAMERA: Camera2D = Camera2D { x: 32.0, y: 32.0, zoom: 2.0 };

fn pixel(rgba: &[u8], tx: f32, ty: f32) -> [u8; 4] {
    let (px, py) = ((tx * 2.0) as usize, (ty * 2.0) as usize);
    rgba[(py * SIZE as usize + px) * 4..][..4].try_into().unwrap()
}

fn render(
    device: &wgpu::Device,
    queue: &wgpu::Queue,
    draw: impl FnOnce(&mut wgpu::CommandEncoder, &wgpu::TextureView),
) -> Vec<u8> {
    let target = render_target(device, SIZE, SIZE);
    let view = target.create_view(&wgpu::TextureViewDescriptor::default());
    let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("test") });
    draw(&mut encoder, &view);
    queue.submit([encoder.finish()]);
    read_pixels(device, queue, &target)
}

fn pass<'e>(encoder: &'e mut wgpu::CommandEncoder, view: &wgpu::TextureView) -> wgpu::RenderPass<'e> {
    encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: Some("test"),
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view,
            depth_slice: None,
            resolve_target: None,
            ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
        })],
        depth_stencil_attachment: None,
        timestamp_writes: None,
        occlusion_query_set: None,
        multiview_mask: None,
    })
}

#[test]
fn tiles_sprites_particles_and_light() {
    let Some((device, queue)) = device() else { return };
    let world = world();
    let atlas_rgba = atlas();
    let mut tiles = TilemapRenderer::new(
        &device,
        &queue,
        FORMAT,
        &TileAtlas { rgba: &atlas_rgba, cell_px: 4, cells_per_row: 4 },
        64,
    );

    // Tiles alone: the front layer over the back one, darkened by its tint (0.55, 0.55, 0.6).
    tiles.prepare(&queue, &world, &CAMERA, SIZE, SIZE);
    let stats = tiles.stats();
    assert!(stats.drawn_chunks >= 1 && stats.drawn_chunks == stats.visible_chunks, "{stats:?}");
    assert_eq!(stats.uploads, stats.drawn_chunks);
    let picture = render(&device, &queue, |e, v| tiles.draw(&mut pass(e, v)));
    assert_eq!(pixel(&picture, 4.5, 4.5), [255, 0, 0, 255]);
    assert_eq!(pixel(&picture, 20.5, 4.5), [0, 255, 0, 255]);
    assert_eq!(pixel(&picture, 40.5, 40.5), [0, 0, 153, 255]);
    tiles.prepare(&queue, &world, &CAMERA, SIZE, SIZE);
    assert_eq!(tiles.stats().uploads, 0, "an unchanged chunk is not sent again");

    // A yellow sprite 4 tiles wide at (48, 8), and particles standing at (24, 48).
    let mut sprites = SpriteRenderer::new(&device, &queue, FORMAT, &sheet(), 1024);
    let mut batch = SpriteBatch::default();
    batch.begin(0.0, 0.0, 16);
    batch.push(Sprite { x: 48.0, y: 8.0, w: 4.0, h: 4.0, color: pack_color(255, 255, 0, 255), ..Sprite::default() });
    batch.push(Sprite { x: -100.0, y: 8.0, ..Sprite::default() }); // off screen: not drawn
    sprites.prepare(&queue, &mut batch, &CAMERA, SIZE, SIZE, true);
    assert_eq!(sprites.stats().drawn, 1);
    let mut particles = ParticleSystem::new(&device, &queue, FORMAT, &sheet(), 1024, 0.0, 0.0, ParticleLook::default());
    particles.emit(&ParticleEmit {
        x: 24.0,
        y: 48.0,
        count: 200,
        speed_min: 0.0,
        speed_max: 0.0,
        life_min: 5.0,
        life_max: 5.0,
        size_start: 3.0,
        size_end: 3.0,
        ..Default::default()
    });
    tiles.prepare(&queue, &world, &CAMERA, SIZE, SIZE);
    particles.prepare(&queue, &CAMERA, SIZE, SIZE);
    let picture = render(&device, &queue, |e, v| {
        particles.simulate(&queue, e, 1.0 / 60.0);
        let mut p = pass(e, v);
        tiles.draw(&mut p);
        sprites.draw(&mut p);
        particles.draw(&mut p);
    });
    assert_eq!(particles.stats().slots_used, 200);
    assert_eq!(pixel(&picture, 48.5, 8.5), [255, 255, 0, 255]);
    assert_eq!(pixel(&picture, 52.5, 8.5), [0, 0, 153, 255], "past the sprite's edge");
    assert_eq!(pixel(&picture, 24.5, 48.5), [255, 255, 255, 255]);
    assert_eq!(pixel(&picture, 28.5, 48.5), [0, 0, 153, 255], "past the particles");

    // Light: dark but for the ambient 0.1 and a lamp of radius 10 at (40.5, 40.5).
    let mut light = LightRenderer::new(&device, FORMAT);
    light.rules_mut().ambient = Color::rgb(0.1, 0.1, 0.1);
    light.add(PointLight::new(40.5, 40.5, 1.0, 1.0, 1.0, 10.0));
    tiles.prepare(&queue, &world, &CAMERA, SIZE, SIZE);
    let picture = render(&device, &queue, |e, v| {
        light.prepare(&queue, e, &world, &CAMERA, SIZE, SIZE);
        let mut p = pass(e, v);
        tiles.draw(&mut p);
        light.draw(&mut p);
    });
    assert_eq!(light.stats().lamps, 1);
    let lit = pixel(&picture, 40.5, 40.5);
    assert!(lit[2] >= 150, "by the lamp: {lit:?}");
    let far = pixel(&picture, 8.5, 56.5);
    assert!(far[0] >= 20 && far[0] <= 32 && far[2] == 0, "only the ambient light far away: {far:?}");
    let edge = pixel(&picture, 47.5, 40.5);
    assert!(edge[2] > far[2] && edge[2] < lit[2], "weaker towards the radius: {edge:?}");
}
