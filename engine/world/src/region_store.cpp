#include "forge/world/region_store.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/profile.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace forge::world {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'F', 'W', 'R', '1'};
constexpr u32 kVersion = 1;
constexpr usize kHeaderBytes = 24;
constexpr usize kEntryBytes = 12;

template <typename T>
void put(std::vector<u8>& out, T v) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

template <typename T>
T get(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

u32 index_in_region(ChunkCoord c) {
    return static_cast<u32>((c.y & (kRegionSize - 1)) * kRegionSize + (c.x & (kRegionSize - 1)));
}

bool parse_region_name(const std::string& name, const std::string& prefix, ChunkCoord& out) {
    // "<prefix>.<x>.<y>.fwr"
    if (name.size() <= prefix.size() + 1 || name.compare(0, prefix.size(), prefix) != 0 || name[prefix.size()] != '.')
        return false;
    const char* p = name.data() + prefix.size() + 1;
    const char* end = name.data() + name.size();
    int x = 0, y = 0;
    auto r = std::from_chars(p, end, x);
    if (r.ec != std::errc() || r.ptr == end || *r.ptr != '.') return false;
    r = std::from_chars(r.ptr + 1, end, y);
    if (r.ec != std::errc() || std::string_view(r.ptr, static_cast<usize>(end - r.ptr)) != ".fwr") return false;
    out = {x, y};
    return true;
}

} // namespace

bool read_chunk_bytes(const ChunkLocation& location, std::vector<u8>& out) {
    std::ifstream in(location.file, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(location.offset));
    out.resize(location.size);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(location.size)));
}

fs::path RegionStore::region_path(ChunkCoord r) const {
    char name[48];
    std::snprintf(name, sizeof(name), ".%d.%d.fwr", r.x, r.y);
    return folder_ / (prefix_ + name);
}

bool RegionStore::open(const fs::path& folder, u32 tag, std::string* error) {
    FORGE_ZONE();
    folder_ = folder;
    tag_ = tag;
    regions_.clear();
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (!fs::is_directory(folder_, ec)) {
        if (error) *error = "cannot create " + path_to_utf8(folder_);
        return false;
    }
    for (fs::directory_iterator it(folder_, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        ChunkCoord region;
        if (!parse_region_name(path_to_utf8(it->path().filename()), prefix_, region)) continue;
        if (!load_header(it->path())) FORGE_WARN("world save: skipping damaged region %s", path_to_utf8(it->path()).c_str());
    }
    return true;
}

bool RegionStore::load_header(const fs::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff file_size = in.tellg();
    if (file_size < static_cast<std::streamoff>(kHeaderBytes)) return false;
    in.seekg(0);
    u8 header[kHeaderBytes];
    if (!in.read(reinterpret_cast<char*>(header), kHeaderBytes)) return false;
    if (std::memcmp(header, kMagic, 4) != 0 || get<u32>(header + 4) != kVersion) return false;
    const ChunkCoord region{get<i32>(header + 8), get<i32>(header + 12)};
    if (get<u32>(header + 16) != tag_) return false;
    const u32 count = get<u32>(header + 20);
    if (count > static_cast<u32>(kRegionSize * kRegionSize)) return false;
    std::vector<u8> table(count * kEntryBytes);
    if (count && !in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()))) return false;

    Region r;
    r.file = file;
    for (u32 i = 0; i < count; ++i) {
        const u8* e = table.data() + i * kEntryBytes;
        const u32 index = get<u32>(e), offset = get<u32>(e + 4), size = get<u32>(e + 8);
        if (index >= static_cast<u32>(kRegionSize * kRegionSize) ||
            static_cast<u64>(offset) + size > static_cast<u64>(file_size))
            return false;
        r.entries[index] = {offset, size};
    }
    regions_[region] = std::move(r);
    return true;
}

bool RegionStore::locate(ChunkCoord chunk, ChunkLocation& out) const {
    auto r = regions_.find(region_of(chunk));
    if (r == regions_.end()) return false;
    auto e = r->second.entries.find(index_in_region(chunk));
    if (e == r->second.entries.end()) return false;
    out.file = r->second.file;
    out.offset = e->second.offset;
    out.size = e->second.size;
    return true;
}

usize RegionStore::chunk_count() const {
    usize n = 0;
    for (const auto& [coord, r] : regions_) n += r.entries.size();
    return n;
}

std::vector<ChunkCoord> RegionStore::chunks() const {
    std::vector<ChunkCoord> out;
    for (const auto& [region, r] : regions_)
        for (const auto& [index, e] : r.entries)
            out.push_back({region.x * kRegionSize + static_cast<i32>(index % kRegionSize),
                           region.y * kRegionSize + static_cast<i32>(index / kRegionSize)});
    return out;
}

bool RegionStore::write(const std::vector<Write>& chunks, u32* regions_written, usize* bytes_written) {
    FORGE_ZONE();
    std::unordered_map<ChunkCoord, std::vector<const Write*>, ChunkCoordHash> by_region;
    for (const Write& w : chunks) by_region[region_of(w.chunk)].push_back(&w);

    bool ok = true;
    u32 regions = 0;
    usize bytes = 0;
    for (auto& [region_coord, writes] : by_region) {
        // Everything already in this region, unless it is being replaced.
        std::vector<u8> old;
        auto existing = regions_.find(region_coord);
        if (existing != regions_.end() && !read_file(existing->second.file, old)) old.clear();

        struct Item {
            u32 index;
            const u8* data;
            u32 size;
        };
        std::vector<Item> items;
        std::vector<bool> replaced(static_cast<usize>(kRegionSize * kRegionSize), false);
        for (const Write* w : writes) {
            const u32 index = index_in_region(w->chunk);
            replaced[index] = true;
            if (w->bytes) items.push_back({index, w->bytes->data(), static_cast<u32>(w->bytes->size())});
        }
        if (existing != regions_.end() && !old.empty()) {
            for (const auto& [index, e] : existing->second.entries)
                if (!replaced[index]) items.push_back({index, old.data() + e.offset, e.size});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.index < b.index; });
        if (items.empty()) {
            // Nothing left in it: the region goes (nothing to do when it was never written).
            if (existing == regions_.end()) continue;
            std::error_code ec;
            std::filesystem::remove(existing->second.file, ec);
            if (ec) {
                FORGE_ERROR("world save: cannot remove %s", path_to_utf8(existing->second.file).c_str());
                ok = false;
                continue;
            }
            regions_.erase(existing);
            ++regions;
            continue;
        }

        std::vector<u8> out;
        out.insert(out.end(), kMagic, kMagic + 4);
        put<u32>(out, kVersion);
        put<i32>(out, region_coord.x);
        put<i32>(out, region_coord.y);
        put<u32>(out, tag_);
        put<u32>(out, static_cast<u32>(items.size()));
        Region r;
        r.file = region_path(region_coord);
        u32 offset = static_cast<u32>(kHeaderBytes + items.size() * kEntryBytes);
        for (const Item& it : items) {
            put<u32>(out, it.index);
            put<u32>(out, offset);
            put<u32>(out, it.size);
            r.entries[it.index] = {offset, it.size};
            offset += it.size;
        }
        for (const Item& it : items) out.insert(out.end(), it.data, it.data + it.size);

        if (!write_file_atomic(r.file, out)) {
            FORGE_ERROR("world save: cannot write %s", path_to_utf8(r.file).c_str());
            ok = false;
            continue;
        }
        regions_[region_coord] = std::move(r);
        ++regions;
        bytes += out.size();
    }
    if (regions_written) *regions_written = regions;
    if (bytes_written) *bytes_written = bytes;
    return ok;
}

} // namespace forge::world
