//! The C++ version's tests of lamps with a radius and the sky through a day (tests/test_level_light.cpp).

use forge_render::light::LightKind;
use forge_render::*;

const TRANSMIT: [f32; 4] = [0.93, 0.82, 0.72, 0.0];

/// A grid of open cells, one tile each, its corner at 0, 0.
struct Grid {
    w: u32,
    h: u32,
    cells: Vec<u32>,
    rgb: Vec<f32>,
}

impl Grid {
    fn new(w: u32, h: u32) -> Grid {
        Grid { w, h, cells: vec![0; (w * h) as usize], rgb: Vec::new() }
    }
    fn set(&mut self, x: u32, y: u32, k: LightKind) {
        self.cells[(y * self.w + x) as usize] = (k as u32) << 30;
    }
    fn light(&mut self, lamps: &[PointLight], step: u32) {
        self.rgb.clear();
        lamp_light(&self.cells, self.w, self.h, 0, 0, step, &TRANSMIT, lamps, &mut self.rgb);
    }
    fn at(&self, x: u32, y: u32, c: usize) -> f32 {
        self.rgb[(y * self.w + x) as usize * 3 + c]
    }
    fn red(&self, x: u32, y: u32) -> f32 {
        self.at(x, y, 0)
    }
}

fn near(a: f32, b: f64, eps: f64) -> bool {
    ((a as f64) - b).abs() <= eps * (1.0 + b.abs().max((a as f64).abs()))
}

#[test]
fn the_falloff_is_full_at_the_centre_and_nothing_at_the_radius() {
    assert_eq!(lamp_falloff(0.0, 10.0), 1.0);
    assert!(near(lamp_falloff(5.0, 10.0), 0.5625, 1e-6));
    assert!(lamp_falloff(9.99, 10.0) > 0.0);
    assert_eq!(lamp_falloff(10.0, 10.0), 0.0);
    assert_eq!(lamp_falloff(11.0, 10.0), 0.0);
    assert_eq!(lamp_falloff(1.0, 0.0), 0.0);
    assert_eq!(lamp_falloff(f64::NAN, 10.0), 0.0);
    let mut last = 2.0;
    for i in 0..40 {
        let k = lamp_falloff(i as f64 * 0.25, 10.0);
        assert!(k < last);
        last = k;
    }
}

#[test]
fn in_the_open_it_reaches_every_cell_nearer_than_the_radius_and_none_farther() {
    let mut g = Grid::new(100, 100);
    let lamp = PointLight::new(50.5, 50.5, 1.0, 0.5, 0.25, 10.0);
    g.light(&[lamp], 1);
    assert_eq!((g.at(50, 50, 0), g.at(50, 50, 1), g.at(50, 50, 2)), (1.0, 0.5, 0.25));
    assert!(near(g.red(55, 50), 0.5625, 1e-6));
    assert_eq!(g.red(45, 50), g.red(55, 50));
    assert_eq!(g.red(50, 55), g.red(55, 50));
    let (mut lit, mut wrong) = (0, 0);
    for y in 30..=70 {
        for x in 30..=70 {
            let d = (x as f64 + 0.5 - lamp.x).hypot(y as f64 + 0.5 - lamp.y);
            let has = g.red(x, y) > 0.0;
            lit += has as u32;
            if has != (d < 10.0) {
                wrong += 1;
            }
            if d < 10.0 {
                assert!(near(g.red(x, y), lamp_falloff(d, 10.0) as f64, 1e-5));
            }
        }
    }
    assert_eq!(wrong, 0);
    assert_eq!(lit, 305);
    assert!(g.red(59, 54) > 0.0);
    assert_eq!(g.red(56, 58), 0.0);
}

#[test]
fn brightness_radius_and_colour_do_not_stand_in_for_each_other() {
    let (mut dim, mut bright, mut wide, mut blue) =
        (Grid::new(120, 120), Grid::new(120, 120), Grid::new(120, 120), Grid::new(120, 120));
    dim.light(&[PointLight::new(60.5, 60.5, 1.0, 1.0, 1.0, 12.0)], 1);
    bright.light(&[PointLight::new(60.5, 60.5, 4.0, 4.0, 4.0, 12.0)], 1);
    wide.light(&[PointLight::new(60.5, 60.5, 1.0, 1.0, 1.0, 24.0)], 1);
    blue.light(&[PointLight::new(60.5, 60.5, 0.0, 0.0, 1.0, 12.0)], 1);
    for x in 60..120 {
        assert_eq!(dim.red(x, 60) > 0.0, bright.red(x, 60) > 0.0, "{x}");
        assert!(near(bright.red(x, 60), dim.red(x, 60) as f64 * 4.0, 1e-5), "{x}");
        assert_eq!(blue.at(x, 60, 0), 0.0);
        assert_eq!(blue.at(x, 60, 2), dim.at(x, 60, 2));
    }
    assert!(dim.red(71, 60) > 0.0);
    assert_eq!(dim.red(72, 60), 0.0);
    assert_eq!(wide.red(60, 60), dim.red(60, 60));
    assert!(wide.red(83, 60) > 0.0);
    assert_eq!(wide.red(84, 60), 0.0);
}

#[test]
fn walls_take_light_away_unloaded_places_stop_it_corners_make_the_way_longer() {
    let (mut open, mut rock, mut shut) = (Grid::new(80, 80), Grid::new(80, 80), Grid::new(80, 80));
    for y in 0..80 {
        rock.set(43, y, LightKind::Solid);
        shut.set(43, y, LightKind::Opaque);
    }
    let lamp = [PointLight::new(40.5, 40.5, 1.0, 1.0, 1.0, 15.0)];
    open.light(&lamp, 1);
    rock.light(&lamp, 1);
    shut.light(&lamp, 1);
    assert_eq!(rock.red(42, 40), open.red(42, 40));
    assert!(rock.red(44, 40) > 0.0);
    assert!(rock.red(44, 40) < open.red(44, 40));
    assert_eq!(rock.red(52, 40), 0.0);
    assert!(open.red(52, 40) > 0.0);
    assert!(rock.red(51, 40) > 0.0);
    for x in 43..60 {
        assert_eq!(shut.red(x, 40), 0.0);
    }
    shut.set(43, 43, LightKind::Open);
    shut.light(&lamp, 1);
    assert!(shut.red(44, 43) > 0.0);
    assert!(shut.red(44, 40) > 0.0);
    assert!(shut.red(44, 40) < open.red(44, 40));
    assert_eq!(shut.red(44, 31), 0.0);
    let mut water = Grid::new(80, 80);
    for y in 0..80 {
        water.set(43, y, LightKind::Dense);
    }
    water.light(&lamp, 1);
    assert!(water.red(44, 40) < open.red(44, 40));
    assert!(water.red(44, 40) > rock.red(44, 40));
}

#[test]
fn two_lamps_the_brightest_wins_a_large_radius_is_kept_to_40() {
    let mut g = Grid::new(200, 200);
    g.light(&[PointLight::new(80.5, 100.5, 1.0, 0.2, 0.2, 10.0), PointLight::new(88.5, 100.5, 0.2, 0.3, 1.0, 6.0)], 1);
    assert_eq!(g.at(80, 100, 0), 1.0);
    assert_eq!(g.at(88, 100, 2), 1.0);
    assert!(near(g.at(84, 100, 0), lamp_falloff(4.0, 10.0) as f64, 1e-5));
    assert!(near(g.at(84, 100, 2), lamp_falloff(4.0, 6.0) as f64, 1e-5));
    let want = (0.2 * lamp_falloff(4.0, 10.0)).max(0.3 * lamp_falloff(4.0, 6.0));
    assert!(near(g.at(84, 100, 1), want as f64, 1e-5));

    let t0 = std::time::Instant::now();
    let mut big = Grid::new(200, 200);
    big.light(&[PointLight::new(100.5, 100.5, 1.0, 1.0, 1.0, 1e9)], 1);
    assert!(big.red(139, 100) > 0.0);
    assert_eq!(big.red(140, 100), 0.0);
    assert_eq!(big.red(100, 141), 0.0);
    assert!(t0.elapsed().as_millis() < 2000);
    let mut none = Grid::new(50, 50);
    none.light(
        &[
            PointLight::new(-5.0, 10.0, 1.0, 1.0, 1.0, 8.0),
            PointLight::new(10.5, 10.5, 1.0, 1.0, 1.0, 0.0),
            PointLight::new(f64::NAN, 10.0, 1.0, 1.0, 1.0, 8.0),
        ],
        1,
    );
    assert!(none.rgb.iter().all(|v| *v == 0.0));
    let mut coarse = Grid::new(100, 100);
    coarse.light(&[PointLight::new(100.5, 100.5, 1.0, 1.0, 1.0, 10.0)], 2);
    assert!(coarse.red(50, 50) > 0.0);
    assert!(coarse.red(54, 50) > 0.0);
    assert_eq!(coarse.red(55, 50), 0.0);
}

#[test]
fn sky_through_a_day() {
    let noon = Color::rgb(1.0, 0.98, 0.92);
    let night = Color::rgb(0.1, 0.12, 0.22);
    let dusk = Color::rgb(0.9, 0.5, 0.3);
    let key = |hour, color| SkyKey { hour, color };
    let keys = [key(5.0, night), key(8.0, noon), key(17.0, noon), key(19.0, dusk), key(21.0, night)];
    let none = Color::default();
    assert_eq!(sky_at(&keys, 12.0, none), noon);
    assert_eq!(sky_at(&keys, 8.0, none), noon);
    assert_eq!(sky_at(&keys, 36.0, none), noon);
    assert_eq!(sky_at(&keys, 2.0, none), night);
    assert_eq!(sky_at(&keys, 23.5, none), night);
    let half = sky_at(&keys, 18.0, none);
    assert!(near(half.r, 0.95, 1e-5));
    assert!(near(half.b, 0.61, 1e-5));
    assert_eq!(sky_at(&[], 18.0, noon), noon);
    assert_eq!(sky_at(&keys, f64::NAN, dusk), dusk);
}
