//! Chunk encoding, byte for byte as `encode_chunk` in engine/world/src/world.cpp:
//! `[u8 version][u8 layer_count]` then runs over all layers in order, `[varint run length][u16 tile, little endian]`.
//! Natural terrain is mostly long runs of air, dirt and stone, so a chunk shrinks from 16 KiB to a few hundred bytes.

use crate::worldgen::CHUNK_TILES;

const VERSION: u8 = 1;

fn put_varint(out: &mut Vec<u8>, mut v: u32) {
    while v >= 0x80 {
        out.push(v as u8 | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

fn get_varint(data: &[u8], at: &mut usize) -> Option<u32> {
    let mut v = 0u32;
    for shift in (0..35).step_by(7) {
        let b = *data.get(*at)?;
        *at += 1;
        v |= ((b & 0x7f) as u32) << shift;
        if b & 0x80 == 0 {
            return Some(v);
        }
    }
    None
}

/// `tiles` holds whole layers of a chunk, layer after layer.
pub fn encode_chunk(tiles: &[u16]) -> Vec<u8> {
    assert!(tiles.len().is_multiple_of(CHUNK_TILES), "a chunk is whole layers");
    let mut out = Vec::with_capacity(256);
    out.push(VERSION);
    out.push((tiles.len() / CHUNK_TILES) as u8);
    let mut i = 0;
    while i < tiles.len() {
        let value = tiles[i];
        let run = tiles[i..].iter().take_while(|&&t| t == value).count();
        put_varint(&mut out, run as u32);
        out.extend_from_slice(&value.to_le_bytes());
        i += run;
    }
    out
}

/// Fills `tiles` (whole layers); false for bytes of another layer count, a damaged run or bytes left over.
pub fn decode_chunk(data: &[u8], tiles: &mut [u16]) -> bool {
    let layers = tiles.len() / CHUNK_TILES;
    if data.len() < 2 || data[0] != VERSION || data[1] as usize != layers {
        return false;
    }
    let mut at = 2;
    let mut i = 0;
    while i < tiles.len() {
        let Some(run) = get_varint(data, &mut at) else { return false };
        let run = run as usize;
        if run == 0 || run > tiles.len() - i || data.len() - at < 2 {
            return false;
        }
        let value = u16::from_le_bytes([data[at], data[at + 1]]);
        at += 2;
        tiles[i..i + run].fill(value);
        i += run;
    }
    at == data.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_and_damage() {
        let mut tiles = vec![3u16; 2 * CHUNK_TILES];
        for i in 0..300 {
            tiles[i * 7] = i as u16;
        }
        *tiles.last_mut().unwrap() = 0xffff;
        let bytes = encode_chunk(&tiles);
        assert!(bytes.len() < tiles.len() * 2 / 4);
        let mut back = vec![0u16; 2 * CHUNK_TILES];
        assert!(decode_chunk(&bytes, &mut back));
        assert_eq!(back, tiles);

        let mut one_layer = vec![0u16; CHUNK_TILES];
        assert!(!decode_chunk(&bytes, &mut one_layer), "another layer count");
        assert!(!decode_chunk(&bytes[..bytes.len() - 1], &mut back), "cut short");
        let mut longer = bytes.clone();
        longer.push(0);
        assert!(!decode_chunk(&longer, &mut back), "bytes left over");
        let mut wrong = bytes;
        wrong[0] = 2;
        assert!(!decode_chunk(&wrong, &mut back), "another version");
    }
}
