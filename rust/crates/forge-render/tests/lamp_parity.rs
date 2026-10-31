//! Lamps with a radius give the same light in C++ and Rust: tests/data/lamp_light.txt is the C++ version's answer
//! for one grid, and tests/test_level_light.cpp checks it there too.

use forge_render::{PointLight, lamp_light};
use std::collections::BTreeMap;
use std::path::Path;

/// The grid's kinds, as `kind_of` in tests/test_level_light.cpp.
fn kind_of(cx: i32, cy: i32) -> u32 {
    if cx == 20 && cy < 26 {
        2
    } else if (30..34).contains(&cx) && (10..16).contains(&cy) {
        3
    } else if cy == 28 || (cx < 10 && (cx + cy) % 11 == 0) {
        1
    } else {
        0
    }
}

#[test]
fn lamps_light_the_grid_as_the_cpp_version_does() {
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/data/lamp_light.txt");
    let text = std::fs::read_to_string(file).unwrap();
    let mut lamps = Vec::new();
    let mut want: BTreeMap<(u32, u32, u32), [f32; 3]> = BTreeMap::new();
    for line in text.lines().filter(|l| !l.starts_with('#') && !l.is_empty()) {
        let w: Vec<&str> = line.split(' ').collect();
        let f = |i: usize| w[i].parse::<f64>().unwrap();
        match w[0] {
            "lamp" => lamps.push(PointLight::new(f(1), f(2), f(3) as f32, f(4) as f32, f(5) as f32, f(6) as f32)),
            "cell" => {
                let n = |i: usize| w[i].parse::<u32>().unwrap();
                want.insert((n(1), n(2), n(3)), [f(4) as f32, f(5) as f32, f(6) as f32]);
            }
            _ => panic!("bad line {line}"),
        }
    }
    assert_eq!(lamps.len(), 6);
    let (w, h) = (48u32, 36u32);
    let cells: Vec<u32> = (0..w * h).map(|i| kind_of((i % w) as i32, (i / w) as i32) << 30).collect();
    let mut lit = 0;
    for step in [1u32, 2] {
        let mut rgb = Vec::new();
        lamp_light(&cells, w, h, -10, -6, step, &[0.93, 0.82, 0.72, 0.0], &lamps, &mut rgb);
        for i in 0..w * h {
            let got = [rgb[i as usize * 3], rgb[i as usize * 3 + 1], rgb[i as usize * 3 + 2]];
            let key = (step, i % w, i / w);
            match want.get(&key) {
                Some(c) => {
                    lit += 1;
                    for k in 0..3 {
                        assert!((got[k] - c[k]).abs() <= 1e-6 * c[k].max(1.0), "{key:?}: {got:?} against {c:?}");
                    }
                }
                None => assert_eq!(got, [0.0; 3], "{key:?} is dark in C++"),
            }
        }
    }
    assert_eq!(lit, want.len());
}
