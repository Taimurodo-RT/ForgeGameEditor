//! Streaming follows the rules of engine/world (tests/test_world.cpp): chunks near the focus load, far ones leave,
//! and edited chunks come back with their edits.

use forge_world::{CHUNK, CHUNK_TILES, Event, Generator, Rect, World};

/// Updates until nothing is left to load or drop around `focus`.
fn settle(world: &mut World, focus: Rect) {
    for _ in 0..10 {
        world.update(focus);
        world.finish_loading();
    }
}

fn bounds() -> Rect {
    Rect { x0: -512, y0: -512, x1: 512, y1: 512 }
}

#[test]
fn chunks_load_around_the_focus_and_far_ones_leave() {
    let mut world = World::new(Generator::TopDown { seed: 7 }, bounds());
    let view = Rect { x0: 0, y0: 0, x1: CHUNK, y1: CHUNK };
    settle(&mut world, view);
    // The focus chunk plus a margin of two on each side.
    for y in -2..=2 {
        for x in -2..=2 {
            assert!(world.chunks.get(&(x, y)).is_some_and(|c| c.ready), "chunk {x},{y}");
        }
    }
    assert!(!world.chunks.contains_key(&(5, 0)));

    let far = Rect { x0: 100 * CHUNK, y0: 0, x1: 101 * CHUNK, y1: CHUNK };
    settle(&mut world, far);
    assert!(world.events.iter().any(|e| matches!(e, Event::Unloading((0, 0)))));
    assert!(!world.chunks.contains_key(&(0, 0)));
    assert!(world.chunks.get(&(100, 0)).is_some_and(|c| c.ready));
}

#[test]
fn edits_survive_unloading() {
    let mut world = World::new(Generator::Side { seed: 1, surface_y: 0 }, bounds());
    let view = Rect { x0: 0, y0: 0, x1: CHUNK, y1: CHUNK };
    settle(&mut world, view);
    assert!(world.set_tile(10, 20, 1, 7));
    assert!(!world.set_tile(10, 20, 1, 7), "the same id again is no change");

    let far = Rect { x0: 100 * CHUNK, y0: 0, x1: 101 * CHUNK, y1: CHUNK };
    settle(&mut world, far);
    assert!(!world.chunks.contains_key(&(0, 0)));
    settle(&mut world, view);
    let chunk = &world.chunks[&(0, 0)];
    assert!(chunk.edited);
    assert_eq!(chunk.tiles[CHUNK_TILES + 20 * CHUNK as usize + 10], 7);
}
