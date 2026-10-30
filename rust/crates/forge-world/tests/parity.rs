//! The Rust generators make the same chunks as the C++ ones: tests/data/world_chunks.txt lists chunks with the hash
//! of their tiles, and tests/test_world.cpp checks the same file.

use forge_world::{CHUNK_TILES, Generator};

fn fnv1a(tiles: &[u16]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for &t in tiles {
        for byte in t.to_le_bytes() {
            hash = (hash ^ byte as u64).wrapping_mul(0x100000001b3);
        }
    }
    hash
}

#[test]
fn generators_match_the_cpp_chunks() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../tests/data/world_chunks.txt");
    let text = std::fs::read_to_string(path).expect("tests/data/world_chunks.txt");
    let mut checked = 0;
    for line in text.lines().filter(|l| !l.is_empty() && !l.starts_with('#')) {
        let f: Vec<&str> = line.split_whitespace().collect();
        assert_eq!(f.len(), 7, "{line}");
        let seed: u64 = f[1].parse().unwrap();
        let surface_y: i32 = f[2].parse().unwrap();
        let (cx, cy): (i32, i32) = (f[3].parse().unwrap(), f[4].parse().unwrap());
        let layers: usize = f[5].parse().unwrap();
        let generator = match f[0] {
            "side" => Generator::Side { seed, surface_y },
            "top" => Generator::TopDown { seed },
            "empty" => Generator::Empty,
            other => panic!("unknown generator {other}"),
        };
        let mut tiles = vec![0xffff; layers * CHUNK_TILES];
        generator.generate(cx, cy, &mut tiles);
        assert_eq!(format!("{:016x}", fnv1a(&tiles)), f[6], "{line}");
        checked += 1;
    }
    assert!(checked >= 20);
}
