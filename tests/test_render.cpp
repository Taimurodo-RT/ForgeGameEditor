#include "forge/core/jobs.h"
#include "forge/core/sort.h"
#include "forge/render/sprite_batch.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

using namespace forge;
using namespace forge::render;

namespace {
struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};
} // namespace

TEST_CASE("radix sort orders by the high 32 bits and keeps equal keys in order") {
    PoolScope pool;
    std::mt19937 rng(3);
    for (u32 n : {0u, 1u, 7u, 1000u, 100'000u}) {
        for (u32 key_bits : {4u, 16u, 32u}) {
            std::vector<u64> data(n), scratch(n);
            for (u32 i = 0; i < n; ++i) {
                const u32 key = key_bits == 32 ? rng() : rng() & ((1u << key_bits) - 1);
                data[i] = static_cast<u64>(key) << 32 | i;
            }
            std::vector<u64> expect = data;
            std::stable_sort(expect.begin(), expect.end(), [](u64 a, u64 b) { return (a >> 32) < (b >> 32); });
            const u64* got = radix_sort_by_high32(data.data(), scratch.data(), n);
            CHECK(std::equal(expect.begin(), expect.end(), got));
        }
    }
}

TEST_CASE("sprite batch: pushes from many threads, culls and sorts") {
    PoolScope pool;
    SpriteBatch batch;
    constexpr u32 kCount = 50'000;
    batch.begin(1000.0, 2000.0, kCount);
    jobs::parallel_for(kCount, 777, [&](u32 b, u32 e) {
        Sprite* s = batch.push(e - b);
        REQUIRE(s != nullptr);
        for (u32 i = b; i < e; ++i) {
            Sprite& sp = s[i - b];
            sp.x = static_cast<f32>(i % 500);  // 0 .. 499
            sp.y = static_cast<f32>(i / 500);  // 0 .. 99
            sp.frame = i;
            sp.order = draw_order(static_cast<u8>(i % 3), sp.y, 100.0f);
        }
    });
    CHECK(batch.push(1) == nullptr); // full
    CHECK(batch.size() == kCount);

    std::vector<Sprite> out(kCount);
    std::vector<u32> order(kCount);
    // View: x in [100, 200], all rows. Sprites are 1 tile, so x 99 .. 201 touch it.
    const SpriteList list = batch.finish({100, 0, 200, 100}, true, out.data(), order.data(), kCount);
    CHECK(list.sprites == kCount);
    CHECK(list.draws == 103 * 100);
    CHECK(batch.stats().dropped == 1);
    std::vector<u32> frames;
    for (const Sprite& sp : out) frames.push_back(sp.frame);
    std::sort(frames.begin(), frames.end());
    u32 missing = 0;
    for (u32 i = 0; i < kCount; ++i) missing += frames[i] != i ? 1 : 0;
    CHECK(missing == 0); // every sprite copied
    for (u32 i = 1; i < list.draws; ++i) {
        const Sprite& a = out[order[i - 1]];
        const Sprite& b = out[order[i]];
        REQUIRE(a.order <= b.order);
        if (a.order == b.order) REQUIRE(order[i - 1] < order[i]); // stable: push order kept
    }
    // Layers come first: everything on layer 0 is drawn before layer 1.
    const u32 first_layer = out[order[0]].order >> 16, last_layer = out[order[list.draws - 1]].order >> 16;
    CHECK(first_layer == 0);
    CHECK(last_layer == 2);

    const SpriteList all = batch.finish({100, 0, 200, 100}, false, out.data(), order.data(), kCount);
    CHECK(all.draws == kCount);
}
