#include "forge/render/sprite_batch.h"

#include "forge/core/jobs.h"
#include "forge/core/profile.h"
#include "forge/core/sort.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace forge::render {

namespace {

constexpr u32 kBlock = 8192;

bool visible(const Sprite& s, const ViewBox& v) {
    // Radius that covers the sprite at any angle.
    const f32 r = (std::fabs(s.w) + std::fabs(s.h)) * 0.5f;
    return s.x + r >= v.x0 && s.x - r <= v.x1 && s.y + r >= v.y0 && s.y - r <= v.y1;
}

} // namespace

void SpriteBatch::begin(f64 origin_x, f64 origin_y, u32 capacity) {
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    if (capacity > capacity_) {
        items_.reset(new Sprite[capacity]);
        capacity_ = capacity;
    }
    count_.store(0, std::memory_order_relaxed);
    stats_ = {};
}

Sprite* SpriteBatch::push(u32 count) {
    const u32 at = count_.fetch_add(count, std::memory_order_relaxed);
    if (at + count > capacity_ || at + count < at) return nullptr;
    return items_.get() + at;
}

u32 SpriteBatch::size() const { return std::min(count_.load(std::memory_order_relaxed), capacity_); }

SpriteList SpriteBatch::finish(const ViewBox& view, bool sorted, Sprite* out_sprites, u32* out_indices,
                              u32 capacity) {
    FORGE_ZONE_N("Sprite batch finish");
    const u32 pushed = count_.load(std::memory_order_relaxed);
    const u32 n = std::min(size(), capacity);
    stats_.submitted = size();
    stats_.dropped = pushed - n;
    const Sprite* items = items_.get();
    const u32 blocks = (n + kBlock - 1) / kBlock;
    SpriteList list;
    list.sprites = n;

    if (!sorted) {
        jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
            const u32 begin = b * kBlock, end = std::min(n, e * kBlock);
            std::memcpy(out_sprites + begin, items + begin, static_cast<usize>(end - begin) * sizeof(Sprite));
        });
        list.draws = n;
        stats_.drawn = n;
        return list;
    }

    // One pass over the sprites: copy each block to upload memory and note
    // (order, index) of the visible ones in that block's part of keys_.
    if (keys_.size() < n) {
        keys_.resize(n + n / 4);
        scratch_.resize(keys_.size());
    }
    u64* keys = keys_.data();
    block_counts_.assign(blocks + 1, 0);
    jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
        for (u32 k = b; k < e; ++k) {
            const u32 begin = k * kBlock, end = std::min(n, begin + kBlock);
            std::memcpy(out_sprites + begin, items + begin, static_cast<usize>(end - begin) * sizeof(Sprite));
            u32 at = begin;
            for (u32 i = begin; i < end; ++i)
                if (visible(items[i], view)) keys[at++] = static_cast<u64>(items[i].order) << 32 | i;
            block_counts_[k] = at - begin;
        }
    });
    // Close the gaps between blocks (into scratch), then sort.
    u32 total = 0;
    for (u32 k = 0; k < blocks; ++k) {
        const u32 c = block_counts_[k];
        block_counts_[k] = total;
        total += c;
    }
    u64* packed = scratch_.data();
    jobs::parallel_for(blocks, 1, [&](u32 b, u32 e) {
        for (u32 k = b; k < e; ++k) {
            const u32 begin = k * kBlock;
            const u32 count = (k + 1 < blocks ? block_counts_[k + 1] : total) - block_counts_[k];
            std::memcpy(packed + block_counts_[k], keys + begin, static_cast<usize>(count) * sizeof(u64));
        }
    });
    const u64* order = radix_sort_by_high32(packed, keys, total);
    jobs::parallel_for(total, kBlock, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) out_indices[i] = static_cast<u32>(order[i]);
    });
    list.draws = total;
    stats_.drawn = total;
    return list;
}

} // namespace forge::render
