//! The C++ version's test of the sprite batch (tests/test_render.cpp): pushes from many threads, culls and sorts.

use forge_render::*;
use rayon::prelude::*;

#[test]
fn pushes_from_many_threads_culls_and_sorts() {
    const COUNT: u32 = 50_000;
    let mut batch = SpriteBatch::default();
    batch.begin(1000.0, 2000.0, COUNT);
    (0..COUNT).collect::<Vec<_>>().par_chunks(777).for_each(|part| {
        let s = batch.push_many(part.len() as u32).expect("room");
        for (sp, &i) in s.iter_mut().zip(part) {
            *sp = Sprite { x: (i % 500) as f32, y: (i / 500) as f32, frame: i, ..Sprite::default() };
            sp.order = draw_order((i % 3) as u8, sp.y, 100.0);
        }
    });
    assert!(batch.push_many(1).is_none()); // full
    assert_eq!(batch.len(), COUNT);

    let mut out = vec![Sprite::default(); COUNT as usize];
    let mut order = vec![0u32; COUNT as usize];
    // View: x in [100, 200], all rows. Sprites are 1 tile, so x 99 .. 201 touch it.
    let view = ViewBox { x0: 100.0, y0: 0.0, x1: 200.0, y1: 100.0 };
    let list = batch.finish(&view, true, &mut out, &mut order);
    assert_eq!(list.sprites, COUNT);
    assert_eq!(list.draws, 103 * 100);
    assert_eq!(batch.stats().dropped, 1);
    let mut frames: Vec<u32> = out.iter().map(|s| s.frame).collect();
    frames.sort();
    assert!(frames.iter().enumerate().all(|(i, f)| *f == i as u32)); // every sprite copied
    for w in order[..list.draws as usize].windows(2) {
        let (a, b) = (&out[w[0] as usize], &out[w[1] as usize]);
        assert!(a.order <= b.order);
        if a.order == b.order {
            assert!(w[0] < w[1]); // stable: push order kept
        }
    }
    assert_eq!(out[order[0] as usize].order >> 16, 0);
    assert_eq!(out[order[list.draws as usize - 1] as usize].order >> 16, 2);

    let all = batch.finish(&view, false, &mut out, &mut order);
    assert_eq!(all.draws, COUNT);
}
