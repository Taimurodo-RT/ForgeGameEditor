//! A world save is the same files in C++ and Rust. Both run the scenario below (tests/test_world.cpp has its twin)
//! and must write exactly the region files in tests/data/world_save/1 (first save) and tests/data/world_save/2
//! (second save). `FORGE_BLESS=1 cargo test -p forge-world --test save_parity` writes them anew.

use forge_world::{CHUNK, CHUNK_TILES, Generator, Rect, World};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

const GENERATOR: Generator = Generator::Side { seed: 1, surface_y: 0 };

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/data/world_save")
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("forge-world-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    dir
}

fn settle(world: &mut World, x: i32, y: i32) {
    let focus = Rect { x0: x, y0: y, x1: x + CHUNK, y1: y + CHUNK };
    for _ in 0..10 {
        world.update(focus);
        world.finish_loading();
    }
}

/// Every region file of a folder with its bytes.
fn files(dir: &Path) -> BTreeMap<String, Vec<u8>> {
    let mut out = BTreeMap::new();
    for e in fs::read_dir(dir).unwrap() {
        let e = e.unwrap();
        out.insert(e.file_name().into_string().unwrap(), fs::read(e.path()).unwrap());
    }
    out
}

fn check(save: &Path, step: &str) {
    let want = fixtures().join(step);
    if std::env::var_os("FORGE_BLESS").is_some() {
        let _ = fs::remove_dir_all(&want);
        fs::create_dir_all(&want).unwrap();
        for (name, bytes) in files(save) {
            fs::write(want.join(name), bytes).unwrap();
        }
    }
    let (got, want) = (files(save), files(&want));
    assert_eq!(got.keys().collect::<Vec<_>>(), want.keys().collect::<Vec<_>>(), "region files after save {step}");
    for (name, bytes) in &want {
        assert!(got[name] == *bytes, "{name} after save {step} differs from tests/data/world_save/{step}");
    }
}

#[test]
fn saves_are_the_files_of_the_cpp_version() {
    let dir = scratch("save-parity");
    let bounds = Rect { x0: -8192, y0: -8192, x1: 8192, y1: 8192 };
    let mut w = World::new(GENERATOR, bounds);
    w.open_save(&dir).unwrap();

    settle(&mut w, 0, 0);
    assert!(w.set_tile(3, 4, 1, 7));
    assert!(w.set_tile(10, 10, 0, 0xffff));
    settle(&mut w, -70, -70); // chunk (-2,-2), region (-1,-1)
    assert!(w.set_tile(-70, -70, 1, 9));
    settle(&mut w, 5000, 0); // chunk (78,0), region (2,0); (0,0) leaves memory changed
    assert!(w.set_tile(5001, 1, 0, 3));
    assert_eq!(w.stored_edits(), 2);
    let first = w.save();
    assert!(first.ok);
    assert_eq!((first.chunks, first.regions), (3, 3));
    assert_eq!(w.save().chunks, 0, "nothing changed since");
    check(&dir, "1");

    // (78,0) back to what the generator makes: its saved copy goes; (0,0) changes again.
    let mut generated = vec![0u16; 2 * CHUNK_TILES];
    GENERATOR.generate(78, 0, &mut generated);
    assert!(w.set_chunk_tiles((78, 0), &generated, false));
    settle(&mut w, 0, 0);
    assert!(w.set_tile(2, 2, 1, 5));
    let second = w.save();
    assert!(second.ok);
    assert_eq!((second.chunks, second.regions), (1, 2));
    check(&dir, "2");

    // A new session reads it all back.
    let mut again = World::new(GENERATOR, bounds);
    again.open_save(&dir).unwrap();
    for (coord, edited) in [((0, 0), true), ((-2, -2), true), ((78, 0), false), ((5, 5), false)] {
        assert_eq!(again.edited(coord), edited, "chunk {coord:?}");
    }
    settle(&mut again, 0, 0);
    let c = &again.chunks[&(0, 0)];
    assert_eq!(c.tiles[CHUNK_TILES + 4 * CHUNK as usize + 3], 7);
    assert_eq!(c.tiles[CHUNK_TILES + 2 * CHUNK as usize + 2], 5);
    assert_eq!(c.tiles[10 * CHUNK as usize + 10], 0xffff);
    assert!(again.restored >= 1);
    assert_eq!(again.peek_chunk((78, 0)).unwrap(), generated);
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn a_damaged_region_falls_back_to_the_generator() {
    let dir = scratch("damaged");
    fs::create_dir_all(&dir).unwrap();
    fs::write(dir.join("r.0.0.fwr"), b"not a region").unwrap();
    let mut w = World::new(Generator::TopDown { seed: 9 }, Rect { x0: -512, y0: -512, x1: 512, y1: 512 });
    w.open_save(&dir).unwrap();
    settle(&mut w, 0, 0);
    assert!(w.chunks[&(0, 0)].ready);
    assert_eq!(w.restored, 0);
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn a_copy_of_the_save_leaves_the_save_folder_alone() {
    let (dir, copy) = (scratch("copy-from"), scratch("copy-to"));
    let mut w = World::new(Generator::TopDown { seed: 9 }, Rect { x0: -512, y0: -512, x1: 8192, y1: 512 });
    w.open_save(&dir).unwrap();
    settle(&mut w, 0, 0);
    w.set_tile(1, 1, 0, 4);
    assert!(w.save().ok);
    w.set_tile(2, 2, 0, 6);
    let before = files(&dir);
    fs::create_dir_all(&copy).unwrap();
    for (name, bytes) in &before {
        fs::write(copy.join(name), bytes).unwrap();
    }
    let report = w.save_copy(&copy);
    assert!(report.ok);
    assert_eq!(report.chunks, 1);
    assert_eq!(files(&dir), before);

    let mut other = World::new(Generator::TopDown { seed: 9 }, Rect { x0: -512, y0: -512, x1: 512, y1: 512 });
    other.open_save(&copy).unwrap();
    settle(&mut other, 0, 0);
    assert_eq!(other.chunks[&(0, 0)].tiles[CHUNK as usize + 1], 4);
    assert_eq!(other.chunks[&(0, 0)].tiles[2 * CHUNK as usize + 2], 6);
    let _ = fs::remove_dir_all(&dir);
    let _ = fs::remove_dir_all(&copy);
}
