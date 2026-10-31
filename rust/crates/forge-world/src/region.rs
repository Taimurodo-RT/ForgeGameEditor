//! Changed chunks on disk, grouped into region files of 32 × 32 chunks, the same files as
//! engine/world/src/region_store.cpp, so a world saved by the C++ version opens here and back. A save touches only the
//! regions that changed; untouched chunks are never written, the generator makes them again.
//!
//! Region file `<prefix>.<x>.<y>.fwr` (prefix `r` for tiles, `e` for entities), little endian:
//! - header: `"FWR1"`, u32 version, i32 region x, i32 region y, u32 tag, u32 count
//! - entries: count × { u32 chunk index in region (y * 32 + x), u32 offset, u32 size }, by index
//! - data: encoded chunks (see `codec`), in the order of the entries

use std::collections::BTreeMap;
use std::fs;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

pub const REGION_SHIFT: i32 = 5;
pub const REGION_SIZE: i32 = 1 << REGION_SHIFT; // 32 chunks = 2048 tiles

const MAGIC: &[u8; 4] = b"FWR1";
const VERSION: u32 = 1;
const HEADER_BYTES: usize = 24;
const ENTRY_BYTES: usize = 12;
const MAX_ENTRIES: u32 = (REGION_SIZE * REGION_SIZE) as u32;

pub fn region_of(chunk: (i32, i32)) -> (i32, i32) {
    (chunk.0 >> REGION_SHIFT, chunk.1 >> REGION_SHIFT)
}

fn index_in_region(c: (i32, i32)) -> u32 {
    ((c.1 & (REGION_SIZE - 1)) * REGION_SIZE + (c.0 & (REGION_SIZE - 1))) as u32
}

/// Where one chunk's bytes are. Reading it needs nothing else, so any thread can.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChunkLocation {
    pub file: PathBuf,
    pub offset: u32,
    pub size: u32,
}

pub fn read_chunk_bytes(location: &ChunkLocation) -> io::Result<Vec<u8>> {
    let mut f = fs::File::open(&location.file)?;
    f.seek(SeekFrom::Start(location.offset as u64))?;
    let mut out = vec![0u8; location.size as usize];
    f.read_exact(&mut out)?;
    Ok(out)
}

/// Writes `bytes` to `<path>.tmp` and renames it over `path`, so a reader sees the old file or the new one, never half
/// of one. As `write_file_atomic` in engine/core: on Windows a reader holding the file is waited for up to a second.
pub fn write_file_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    fs::write(&tmp, bytes)?;
    let mut result = fs::rename(&tmp, path);
    if cfg!(windows) {
        let until = std::time::Instant::now() + std::time::Duration::from_secs(1);
        while result.as_ref().is_err_and(|e| e.kind() == io::ErrorKind::PermissionDenied)
            && std::time::Instant::now() < until
            && !path.is_dir()
        {
            std::thread::sleep(std::time::Duration::from_millis(10));
            result = fs::rename(&tmp, path);
        }
    }
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

#[derive(Clone, Copy, Debug)]
struct Entry {
    offset: u32,
    size: u32,
}

#[derive(Debug)]
struct Region {
    file: PathBuf,
    entries: BTreeMap<u32, Entry>, // by chunk index inside the region
}

/// One chunk to write: `Some(bytes)` replaces it, `None` removes it.
pub struct Write<'a> {
    pub chunk: (i32, i32),
    pub bytes: Option<&'a [u8]>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Written {
    pub ok: bool,
    pub regions: u32,
    pub bytes: usize,
}

#[derive(Debug)]
pub struct RegionStore {
    prefix: String,
    folder: PathBuf,
    tag: u32,
    regions: BTreeMap<(i32, i32), Region>,
}

impl RegionStore {
    /// Region files are named `<prefix>.<x>.<y>.fwr`, so several stores can share one save folder. Reads the headers
    /// of every region in the folder (creating the folder if needed). `tag` guards against data of another layout
    /// (tiles: the layer count). Damaged region files are skipped with a warning.
    pub fn open(folder: &Path, prefix: &str, tag: u32) -> io::Result<RegionStore> {
        fs::create_dir_all(folder)?;
        let mut store =
            RegionStore { prefix: prefix.to_owned(), folder: folder.to_owned(), tag, regions: BTreeMap::new() };
        for entry in fs::read_dir(folder)? {
            let Ok(entry) = entry else { break };
            if !entry.file_type().is_ok_and(|t| t.is_file()) {
                continue;
            }
            let Some(name) = entry.file_name().to_str().map(str::to_owned) else { continue };
            if parse_region_name(&name, prefix).is_none() {
                continue;
            }
            if !store.load_header(&entry.path()) {
                log::warn!("world save: skipping damaged region {}", entry.path().display());
            }
        }
        Ok(store)
    }

    fn load_header(&mut self, file: &Path) -> bool {
        let Ok(bytes) = fs::read(file) else { return false };
        if bytes.len() < HEADER_BYTES || &bytes[0..4] != MAGIC || u32_at(&bytes, 4) != VERSION {
            return false;
        }
        let region = (u32_at(&bytes, 8) as i32, u32_at(&bytes, 12) as i32);
        let count = u32_at(&bytes, 20);
        if u32_at(&bytes, 16) != self.tag || count > MAX_ENTRIES {
            return false;
        }
        let table_end = HEADER_BYTES + count as usize * ENTRY_BYTES;
        if bytes.len() < table_end {
            return false;
        }
        let mut r = Region { file: file.to_owned(), entries: BTreeMap::new() };
        for i in 0..count as usize {
            let e = HEADER_BYTES + i * ENTRY_BYTES;
            let (index, offset, size) = (u32_at(&bytes, e), u32_at(&bytes, e + 4), u32_at(&bytes, e + 8));
            if index >= MAX_ENTRIES || offset as u64 + size as u64 > bytes.len() as u64 {
                return false;
            }
            r.entries.insert(index, Entry { offset, size });
        }
        self.regions.insert(region, r);
        true
    }

    fn region_path(&self, r: (i32, i32)) -> PathBuf {
        self.folder.join(format!("{}.{}.{}.fwr", self.prefix, r.0, r.1))
    }

    pub fn locate(&self, chunk: (i32, i32)) -> Option<ChunkLocation> {
        let r = self.regions.get(&region_of(chunk))?;
        let e = r.entries.get(&index_in_region(chunk))?;
        Some(ChunkLocation { file: r.file.clone(), offset: e.offset, size: e.size })
    }

    pub fn chunk_count(&self) -> usize {
        self.regions.values().map(|r| r.entries.len()).sum()
    }

    pub fn region_count(&self) -> usize {
        self.regions.len()
    }

    /// Every chunk saved here, by region then by index.
    pub fn chunks(&self) -> Vec<(i32, i32)> {
        let mut out = Vec::new();
        for (region, r) in &self.regions {
            for index in r.entries.keys() {
                let index = *index as i32;
                out.push((region.0 * REGION_SIZE + index % REGION_SIZE, region.1 * REGION_SIZE + index / REGION_SIZE));
            }
        }
        out
    }

    /// Writes these chunks, keeping every other chunk already saved in the same regions. Each region file is replaced
    /// atomically; a region left with no chunks is removed. No chunk may be read from this store while it runs.
    pub fn write(&mut self, chunks: &[Write]) -> Written {
        let mut by_region: BTreeMap<(i32, i32), Vec<&Write>> = BTreeMap::new();
        for w in chunks {
            by_region.entry(region_of(w.chunk)).or_default().push(w);
        }
        let mut done = Written { ok: true, ..Written::default() };
        for (region, writes) in by_region {
            // Everything already in this region, unless it is being replaced.
            let existing = self.regions.get(&region);
            let old = existing.and_then(|r| fs::read(&r.file).ok()).unwrap_or_default();
            let mut items: BTreeMap<u32, &[u8]> = BTreeMap::new();
            let mut replaced = vec![false; MAX_ENTRIES as usize];
            for w in &writes {
                let index = index_in_region(w.chunk);
                replaced[index as usize] = true;
                items.remove(&index);
                if let Some(bytes) = w.bytes {
                    items.insert(index, bytes);
                }
            }
            if let Some(r) = existing
                && !old.is_empty()
            {
                for (index, e) in &r.entries {
                    let range = e.offset as usize..e.offset as usize + e.size as usize;
                    if !replaced[*index as usize] && range.end <= old.len() {
                        items.insert(*index, &old[range]);
                    }
                }
            }
            if items.is_empty() {
                // Nothing left in it: the region goes (nothing to do when it was never written).
                let Some(r) = existing else { continue };
                if let Err(e) = fs::remove_file(&r.file) {
                    log::error!("world save: cannot remove {}: {e}", r.file.display());
                    done.ok = false;
                    continue;
                }
                self.regions.remove(&region);
                done.regions += 1;
                continue;
            }

            let mut out = Vec::new();
            out.extend_from_slice(MAGIC);
            for v in [VERSION, region.0 as u32, region.1 as u32, self.tag, items.len() as u32] {
                out.extend_from_slice(&v.to_le_bytes());
            }
            let mut r = Region { file: self.region_path(region), entries: BTreeMap::new() };
            let mut offset = (HEADER_BYTES + items.len() * ENTRY_BYTES) as u32;
            for (index, data) in &items {
                for v in [*index, offset, data.len() as u32] {
                    out.extend_from_slice(&v.to_le_bytes());
                }
                r.entries.insert(*index, Entry { offset, size: data.len() as u32 });
                offset += data.len() as u32;
            }
            for data in items.values() {
                out.extend_from_slice(data);
            }
            if let Err(e) = write_file_atomic(&r.file, &out) {
                log::error!("world save: cannot write {}: {e}", r.file.display());
                done.ok = false;
                continue;
            }
            self.regions.insert(region, r);
            done.regions += 1;
            done.bytes += out.len();
        }
        done
    }

    /// The folder was moved (renamed) to another place, its files as they were: they are read and written there from
    /// now on.
    pub fn moved(&mut self, folder: &Path) {
        self.folder = folder.to_owned();
        for r in self.regions.values_mut() {
            if let Some(name) = r.file.file_name() {
                r.file = folder.join(name);
            }
        }
    }
}

fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// `<prefix>.<x>.<y>.fwr` → region coordinates.
fn parse_region_name(name: &str, prefix: &str) -> Option<(i32, i32)> {
    let rest = name.strip_prefix(prefix)?.strip_prefix('.')?.strip_suffix(".fwr")?;
    let (x, y) = rest.split_once('.')?;
    Some((x.parse().ok()?, y.parse().ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn region_names() {
        assert_eq!(parse_region_name("r.0.-1.fwr", "r"), Some((0, -1)));
        assert_eq!(parse_region_name("r.12.3.fwr", "e"), None);
        assert_eq!(parse_region_name("r.1.fwr", "r"), None);
        assert_eq!(parse_region_name("r.1.2.fwr.tmp", "r"), None);
        assert_eq!(region_of((-1, 32)), (-1, 1));
        assert_eq!(index_in_region((-1, -1)), 32 * 32 - 1);
    }
}
