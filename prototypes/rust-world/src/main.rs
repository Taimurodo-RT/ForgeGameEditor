//! Forge big-world check on Rust: the same 64k × 64k world as forge_world_demo (same generators, chunk size,
//! streaming rules, flight speed and tile renderer design), plus a crowd of bevy_ecs creatures and GPU particles.
//!
//!   forge-rust-world [--topdown] [--crowd N] [--particles N] [--seconds S] [--vsync] [--dig]
//!   forge-rust-world --headless S [--topdown] [--crowd N]     no window: streaming and the crowd only
//!
//! With --seconds the flight runs 3 s to warm up, then measures S seconds, prints a summary, appends it to
//! bench-results.txt and quits. Esc quits, F toggles the automatic flight, the mouse wheel zooms.

mod ents;
mod worldgen;
mod render;
mod world;

use worldgen::{CHUNK, Generator};
use render::{Camera, Renderer};
use std::sync::Arc;
use std::time::Instant;
use winit::application::ApplicationHandler;
use winit::event::{ElementState, MouseScrollDelta, WindowEvent};
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
use winit::keyboard::{KeyCode, PhysicalKey};
use winit::window::{Window, WindowId};
use world::{Rect, World};

const WORLD_CHUNKS: i32 = 1024;
const WORLD_TILES: i32 = WORLD_CHUNKS * CHUNK;
const SURFACE_Y: i32 = 2000;

#[derive(Clone)]
struct Options {
    top_down: bool,
    crowd: u32,
    particles: u32,
    seconds: Option<f64>,
    headless: Option<f64>,
    vsync: bool,
    dig: bool,
}

fn parse() -> Options {
    let mut o = Options { top_down: false, crowd: 0, particles: 0, seconds: None, headless: None, vsync: false, dig: false };
    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        let next = |i: usize| args.get(i + 1).and_then(|s| s.parse::<f64>().ok());
        match args[i].as_str() {
            "--topdown" => o.top_down = true,
            "--vsync" => o.vsync = true,
            "--dig" => o.dig = true,
            "--crowd" => {
                o.crowd = next(i).unwrap_or(200_000.0) as u32;
                i += 1
            }
            "--particles" => {
                o.particles = next(i).unwrap_or(1_000_000.0) as u32;
                i += 1
            }
            "--seconds" => {
                o.seconds = next(i);
                i += 1
            }
            "--headless" => {
                o.headless = next(i);
                i += 1
            }
            other => eprintln!("unknown argument {other}"),
        }
        i += 1;
    }
    if o.crowd > 0 || o.particles > 0 {
        o.top_down = true; // the crowd walks over the top-down world
    }
    o
}

struct Stats {
    frames: Vec<f64>,
    world_ms: Vec<f64>,
    crowd_ms: Vec<f64>,
    prepare_ms: Vec<f64>,
}

impl Stats {
    fn new() -> Self {
        Self { frames: vec![], world_ms: vec![], crowd_ms: vec![], prepare_ms: vec![] }
    }
}

fn summary(v: &mut [f64]) -> String {
    if v.is_empty() {
        return "-".into();
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let avg = v.iter().sum::<f64>() / v.len() as f64;
    let p = |q: f64| v[((v.len() - 1) as f64 * q) as usize];
    format!("avg {:.3} ms, p50 {:.3}, p99 {:.3}, worst {:.3}", avg, p(0.5), p(0.99), v[v.len() - 1])
}

struct Sim {
    opt: Options,
    world: World,
    crowd: Option<ents::Crowd>,
    cam: Camera,
    fly: bool,
    fly_d: (f64, f64),
    area: [f32; 4],
    dig_seed: u32,
}

impl Sim {
    fn new(opt: Options) -> Self {
        let generator = if opt.top_down { Generator::TopDown { seed: 2026 } } else { Generator::Side { seed: 2026, surface_y: SURFACE_Y } };
        let centre = [WORLD_TILES / 2, if opt.top_down { WORLD_TILES / 2 } else { SURFACE_Y }];
        let crowd = (opt.crowd > 0).then(|| {
            let t = Instant::now();
            let c = ents::Crowd::new(opt.crowd, generator, centre);
            println!("crowd: {} creatures ready in {:.0} ms", opt.crowd, t.elapsed().as_secs_f64() * 1000.0);
            c
        });
        let half = (ents::AREA / 2) as f32;
        let area = [centre[0] as f32 - half, centre[1] as f32 - half, centre[0] as f32 + half, centre[1] as f32 + half];
        // The crowd is watched from far away and the camera stays over it; the world alone is flown across at zoom 8.
        let watching = opt.crowd > 0 || opt.particles > 0;
        Sim {
            world: World::new(generator, Rect { x0: 0, y0: 0, x1: WORLD_CHUNKS, y1: WORLD_CHUNKS }),
            crowd,
            cam: Camera { x: centre[0] as f64, y: centre[1] as f64, zoom: if watching { 0.5 } else { 8.0 } },
            fly: opt.seconds.is_some() || opt.headless.is_some(),
            fly_d: (0.8, 0.6),
            area,
            dig_seed: 1,
            opt,
        }
    }

    fn visible(&self, w: u32, h: u32) -> Rect {
        let (hw, hh) = (w as f64 / 2.0 / self.cam.zoom as f64, h as f64 / 2.0 / self.cam.zoom as f64);
        Rect { x0: (self.cam.x - hw).floor() as i32, y0: (self.cam.y - hh).floor() as i32, x1: (self.cam.x + hw).ceil() as i32, y1: (self.cam.y + hh).ceil() as i32 }
    }

    /// One tick: flight, world streaming, crowd. Returns (world ms, crowd ms).
    fn step(&mut self, dt: f64, w: u32, h: u32) -> (f64, f64) {
        if self.fly {
            let watching = self.crowd.is_some() || self.opt.particles > 0;
            // The world flight is 3000 tiles a second as in forge_world_demo; over the crowd the camera sweeps slowly.
            let (speed, lo, hi) = if watching {
                (300.0, self.area[0] as f64 + 1000.0, self.area[2] as f64 - 1000.0)
            } else {
                (3000.0, 1000.0, (WORLD_TILES - 1000) as f64)
            };
            self.cam.x += self.fly_d.0 * speed * dt;
            self.cam.y += self.fly_d.1 * speed * dt;
            if self.cam.x < lo || self.cam.x > hi {
                self.fly_d.0 = -self.fly_d.0;
            }
            if watching && (self.cam.y < lo || self.cam.y > hi) {
                self.fly_d.1 = -self.fly_d.1;
            }
            if !watching && (self.cam.y < lo || self.cam.y > hi) {
                self.fly_d.1 = -self.fly_d.1;
            }
        }
        if self.opt.dig {
            // 64 tiles changed a frame around the camera: edits on screen re-sent to the GPU.
            for _ in 0..64 {
                self.dig_seed = self.dig_seed.wrapping_mul(1664525).wrapping_add(1013904223);
                let dx = (self.dig_seed >> 8) % 200;
                let dy = (self.dig_seed >> 20) % 120;
                let _ = self.world.set_tile(self.cam.x as i32 - 100 + dx as i32, self.cam.y as i32 - 60 + dy as i32, 1, worldgen::AIR);
            }
        }
        let t = Instant::now();
        self.world.update(self.visible(w, h));
        let world_ms = t.elapsed().as_secs_f64() * 1000.0;
        let t = Instant::now();
        if let Some(c) = &mut self.crowd {
            c.step(dt as f32);
        }
        (world_ms, t.elapsed().as_secs_f64() * 1000.0)
    }
}

fn headless(opt: Options, seconds: f64) {
    let mut sim = Sim::new(opt.clone());
    let (w, h) = (2560, 1440);
    let mut stats = Stats::new();
    let frames = (seconds * 60.0) as usize;
    let start = Instant::now();
    for _ in 0..frames {
        let (wm, cm) = sim.step(1.0 / 60.0, w, h);
        stats.world_ms.push(wm);
        stats.crowd_ms.push(cm);
        sim.world.events.clear();
        let next = start + std::time::Duration::from_secs_f64(stats.world_ms.len() as f64 / 60.0);
        if let Some(d) = next.checked_duration_since(Instant::now()) {
            std::thread::sleep(d);
        }
    }
    println!(
        "headless {} frames, {} threads: world.update {} | crowd {} | {} chunks generated, {} in memory",
        frames,
        rayon::current_num_threads(),
        summary(&mut stats.world_ms),
        summary(&mut stats.crowd_ms),
        sim.world.generated,
        sim.world.resident()
    );
}

struct Gfx {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    renderer: Renderer,
    adapter: String,
}

struct App {
    sim: Sim,
    gfx: Option<Gfx>,
    last: Instant,
    started: Instant,
    measuring: bool,
    stats: Stats,
    title_at: Instant,
}

impl App {
    fn finish(&mut self, event_loop: &ActiveEventLoop) {
        let g = self.gfx.as_ref().unwrap();
        let mode = if self.sim.crowd.is_some() || self.sim.opt.particles > 0 {
            format!("crowd {} + particles {} (zoom 0.5)", self.sim.opt.crowd, self.sim.opt.particles)
        } else {
            format!("{} flight 3000 tiles/s (zoom 8){}", if self.sim.opt.top_down { "top-down" } else { "side view" }, if self.sim.opt.dig { " + digging" } else { "" })
        };
        let n = self.stats.frames.len();
        let secs: f64 = self.stats.frames.iter().sum::<f64>() / 1000.0;
        let text = format!(
            "Forge Rust world check: {mode}\n  GPU {} | {}x{} | vsync {}\n  {} frames in {:.1} s = {:.0} FPS\n  frame        {}\n  world.update {}\n  crowd (ECS)  {}\n  render prep  {}\n  chunks: {} generated, {} in memory, {} GPU slots\n",
            g.adapter,
            g.config.width,
            g.config.height,
            self.sim.opt.vsync,
            n,
            secs,
            n as f64 / secs.max(1e-9),
            summary(&mut self.stats.frames),
            summary(&mut self.stats.world_ms),
            summary(&mut self.stats.crowd_ms),
            summary(&mut self.stats.prepare_ms),
            self.sim.world.generated,
            self.sim.world.resident(),
            g.renderer.max_slots(),
        );
        println!("{text}");
        use std::io::Write;
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open("bench-results.txt") {
            let _ = writeln!(f, "{text}");
        }
        event_loop.exit();
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.gfx.is_some() {
            return;
        }
        let attrs = Window::default_attributes().with_title("Forge Rust world check").with_inner_size(winit::dpi::LogicalSize::new(1600, 900));
        let window = Arc::new(event_loop.create_window(attrs).expect("window"));
        let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_with_display_handle_from_env(Box::new(event_loop.owned_display_handle())));
        let surface = instance.create_surface(window.clone()).expect("surface");
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
            apply_limit_buckets: false,
        }))
        .expect("adapter");
        let info = adapter.get_info();
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: None,
            required_features: wgpu::Features::empty(),
            required_limits: adapter.limits(),
            ..Default::default()
        }))
        .expect("device");
        let size = window.inner_size();
        let caps = surface.get_capabilities(&adapter);
        let format = caps.formats.iter().copied().find(|f| !f.is_srgb()).unwrap_or(caps.formats[0]);
        let present_mode = if self.sim.opt.vsync {
            wgpu::PresentMode::AutoVsync
        } else if caps.present_modes.contains(&wgpu::PresentMode::Immediate) {
            wgpu::PresentMode::Immediate
        } else {
            wgpu::PresentMode::AutoNoVsync
        };
        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode,
            desired_maximum_frame_latency: 2,
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            color_space: wgpu::SurfaceColorSpace::Auto,
        };
        surface.configure(&device, &config);
        let mut renderer = Renderer::new(&device, &queue, format, self.sim.opt.crowd as usize, self.sim.opt.particles, self.sim.area);
        if self.sim.opt.top_down {
            renderer.clear = wgpu::Color { r: 0.07, g: 0.09, b: 0.10, a: 1.0 };
            renderer.tints[0] = [1.0; 4];
        }
        println!("GPU: {} ({:?}), {} chunk slots", info.name, info.backend, renderer.max_slots());
        // Start like the C++ demo: the first screen already loaded.
        let vis = self.sim.visible(config.width, config.height);
        self.sim.world.update(vis);
        self.sim.world.finish_loading();
        self.gfx = Some(Gfx { window, surface, device, queue, config, renderer, adapter: format!("{} ({:?})", info.name, info.backend) });
        self.last = Instant::now();
        self.started = Instant::now();
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => event_loop.exit(),
            WindowEvent::Resized(s) => {
                if let Some(g) = &mut self.gfx {
                    g.config.width = s.width.max(1);
                    g.config.height = s.height.max(1);
                    g.surface.configure(&g.device, &g.config);
                }
            }
            WindowEvent::KeyboardInput { event, .. } if event.state == ElementState::Pressed => match event.physical_key {
                PhysicalKey::Code(KeyCode::Escape) => event_loop.exit(),
                PhysicalKey::Code(KeyCode::KeyF) => self.sim.fly = !self.sim.fly,
                _ => {}
            },
            WindowEvent::MouseWheel { delta, .. } => {
                let d = match delta {
                    MouseScrollDelta::LineDelta(_, y) => y,
                    MouseScrollDelta::PixelDelta(p) => p.y as f32 / 60.0,
                };
                self.sim.cam.zoom = (self.sim.cam.zoom * 1.15f32.powf(d)).clamp(0.5, 64.0);
            }
            WindowEvent::RedrawRequested => self.frame(event_loop),
            _ => {}
        }
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        if let Some(g) = &self.gfx {
            g.window.request_redraw();
        }
    }
}

impl App {
    fn frame(&mut self, event_loop: &ActiveEventLoop) {
        let now = Instant::now();
        let frame_ms = (now - self.last).as_secs_f64() * 1000.0;
        let dt = (frame_ms / 1000.0).min(0.1);
        self.last = now;
        let (w, h) = {
            let g = self.gfx.as_ref().unwrap();
            (g.config.width, g.config.height)
        };
        let (world_ms, crowd_ms) = self.sim.step(dt, w, h);
        let g = self.gfx.as_mut().unwrap();
        let t = Instant::now();
        g.renderer.on_world_events(&self.sim.world);
        self.sim.world.events.clear();
        let sprites: &[[u32; 3]] = self.sim.crowd.as_ref().map(|c| c.drawn()).unwrap_or(&[]);
        g.renderer.prepare(&g.queue, &self.sim.world, &self.sim.cam, w, h, sprites, dt as f32, self.sim.area);
        let prepare_ms = t.elapsed().as_secs_f64() * 1000.0;
        let frame = match g.surface.get_current_texture() {
            wgpu::CurrentSurfaceTexture::Success(f) | wgpu::CurrentSurfaceTexture::Suboptimal(f) => f,
            _ => {
                g.surface.configure(&g.device, &g.config);
                return;
            }
        };
        let view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = g.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });
        g.renderer.draw(&mut encoder, &view);
        g.queue.submit([encoder.finish()]);
        g.window.pre_present_notify();
        g.queue.present(frame);

        let elapsed = self.started.elapsed().as_secs_f64();
        if let Some(seconds) = self.sim.opt.seconds {
            if elapsed > 3.0 {
                self.measuring = true;
            }
            if self.measuring {
                self.stats.frames.push(frame_ms);
                self.stats.world_ms.push(world_ms);
                self.stats.crowd_ms.push(crowd_ms);
                self.stats.prepare_ms.push(prepare_ms);
            }
            if elapsed > 3.0 + seconds {
                self.finish(event_loop);
                return;
            }
        }
        if self.title_at.elapsed().as_secs_f64() > 0.5 {
            self.title_at = Instant::now();
            let g = self.gfx.as_ref().unwrap();
            g.window.set_title(&format!(
                "Forge Rust world check | {:.0} FPS ({:.2} ms) | chunks {} on screen, {} in memory | world {:.2} ms, crowd {:.2} ms{}",
                1000.0 / frame_ms.max(1e-6),
                frame_ms,
                g.renderer.drawn_chunks,
                self.sim.world.resident(),
                world_ms,
                crowd_ms,
                if self.sim.fly { " [flight]" } else { "" }
            ));
        }
    }
}

fn main() {
    let opt = parse();
    if let Some(s) = opt.headless {
        headless(opt, s);
        return;
    }
    let event_loop = EventLoop::new().expect("event loop");
    event_loop.set_control_flow(ControlFlow::Poll);
    let now = Instant::now();
    let mut app = App { sim: Sim::new(opt), gfx: None, last: now, started: now, measuring: false, stats: Stats::new(), title_at: now };
    event_loop.run_app(&mut app).expect("run");
}
