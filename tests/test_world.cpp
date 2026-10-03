#include "forge/core/jobs.h"
#include "forge/world/generators.h"
#include "forge/world/world.h"

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace forge;
using namespace forge::world;

namespace {

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

// Counts listener calls and checks they pair up.
struct Counter final : WorldListener {
    int loaded = 0;
    int unloading = 0;
    void on_chunk_loaded(Chunk&) override { ++loaded; }
    void on_chunk_unloading(Chunk&) override { ++unloading; }
};

Rect view_at(i32 x, i32 y, i32 w = 200, i32 h = 120) { return {x, y, x + w, y + h}; }

} // namespace

TEST_CASE("chunk coordinates round toward negative infinity") {
    CHECK(chunk_of(0, 0) == ChunkCoord{0, 0});
    CHECK(chunk_of(63, 63) == ChunkCoord{0, 0});
    CHECK(chunk_of(64, -1) == ChunkCoord{1, -1});
    CHECK(chunk_of(-64, -65) == ChunkCoord{-1, -2});
    CHECK(local_index(-1, -1) == kChunkTiles - 1);
    CHECK(local_index(65, 2) == 2u * kChunkSize + 1);

    const Rect c = chunks_of({-1, 0, 64, 65});
    CHECK(c == Rect{-1, 0, 1, 2});
    CHECK(chunks_of({}).empty());
}

TEST_CASE("chunk encoding round-trips and rejects damage") {
    std::vector<TileId> tiles(2 * kChunkTiles, TileStone);
    for (u32 i = 0; i < 300; ++i) tiles[i * 7] = static_cast<TileId>(i);
    tiles.back() = 0xffff;

    const std::vector<u8> bytes = encode_chunk(tiles.data(), 2);
    CHECK(bytes.size() < tiles.size() * sizeof(TileId) / 4);

    std::vector<TileId> back(2 * kChunkTiles, 0);
    REQUIRE(decode_chunk(bytes.data(), bytes.size(), back.data(), 2));
    CHECK(back == tiles);

    CHECK_FALSE(decode_chunk(bytes.data(), bytes.size() - 1, back.data(), 2)); // truncated
    CHECK_FALSE(decode_chunk(bytes.data(), bytes.size(), back.data(), 3));     // wrong layer count
    std::vector<u8> extra = bytes;
    extra.push_back(1);
    CHECK_FALSE(decode_chunk(extra.data(), extra.size(), back.data(), 2)); // trailing bytes
}

TEST_CASE("generators are deterministic") {
    SideViewGenerator side(42, 0);
    TopDownGenerator top(42);
    std::vector<TileId> a(2 * kChunkTiles), b(2 * kChunkTiles);
    for (const Generator* g : {static_cast<const Generator*>(&side), static_cast<const Generator*>(&top)}) {
        g->generate({3, 1}, {a.data(), 2});
        g->generate({3, 1}, {b.data(), 2});
        CHECK(a == b);
    }
    // Side view: sky high above, solid ground deep below.
    side.generate({0, -20}, {a.data(), 2});
    CHECK(a[0] == TileAir);
    side.generate({0, 20}, {a.data(), 2});
    u32 solid = 0;
    for (u32 i = 0; i < kChunkTiles; ++i) solid += a[kChunkTiles + i] != TileAir;
    CHECK(solid > kChunkTiles / 2);
}

TEST_CASE("world streams chunks around the focus") {
    PoolScope pool;
    auto gen = std::make_shared<TopDownGenerator>(7);
    WorldDesc desc;
    desc.load_margin = 1;
    desc.keep_extra = 1;
    World w(desc, gen);
    Counter counter;
    w.add_listener(&counter);

    const Rect view = view_at(0, 0); // 200×120 tiles: chunks [0,4)×[0,2)
    w.update(view);
    w.finish_loading();
    // Nearest first, but within a few updates every wanted chunk is there.
    for (int i = 0; i < 10; ++i) {
        w.update(view);
        w.finish_loading();
    }
    const Rect want = chunks_of(view).expanded(1);
    for (i32 y = want.y0; y < want.y1; ++y)
        for (i32 x = want.x0; x < want.x1; ++x) CHECK(w.find_chunk({x, y}) != nullptr);
    CHECK(w.stats().resident == static_cast<u32>((want.x1 - want.x0) * (want.y1 - want.y0)));
    CHECK(counter.loaded == static_cast<int>(w.stats().resident));

    // Tiles match the generator.
    std::vector<TileId> direct(2 * kChunkTiles);
    gen->generate({2, 1}, {direct.data(), 2});
    CHECK(w.tile(0, 2 * kChunkSize + 5, kChunkSize + 9) == direct[local_index(5, 9)]);

    // Far away: the old chunks go, new ones come.
    for (int i = 0; i < 10; ++i) {
        w.update(view_at(100'000, -50'000));
        w.finish_loading();
    }
    CHECK(w.find_chunk({0, 0}) == nullptr);
    CHECK(w.find_chunk(chunk_of(100'000, -50'000)) != nullptr);
    CHECK(counter.loaded - counter.unloading == static_cast<int>(w.stats().resident));
    w.remove_listener(&counter);
}

TEST_CASE("edits survive unloading") {
    PoolScope pool;
    WorldDesc desc;
    desc.load_margin = 0;
    desc.keep_extra = 0;
    World w(desc, std::make_shared<SideViewGenerator>(1, 0));

    const Rect here = view_at(0, 0, 64, 64);
    w.update(here);
    w.finish_loading();
    REQUIRE(w.find_chunk({0, 0}) != nullptr);
    const u32 rev = w.find_chunk({0, 0})->revision;
    CHECK(w.set_tile(1, 10, 20, TileGold));
    CHECK(w.find_chunk({0, 0})->revision == rev + 1);
    CHECK_FALSE(w.set_tile(1, 5000, 5000, TileGold)); // not loaded: nothing happens

    w.update(view_at(10'000, 0, 64, 64));
    w.finish_loading();
    CHECK(w.find_chunk({0, 0}) == nullptr);
    CHECK(w.stats().stored_edits == 1);

    w.update(here);
    w.finish_loading();
    CHECK(w.tile(1, 10, 20) == TileGold);
    CHECK(w.find_chunk({0, 0})->edited);
    CHECK(w.stats().restored == 1);
    CHECK(w.stats().stored_edits == 0);
}

TEST_CASE("bounded worlds stop at their edges") {
    PoolScope pool;
    WorldDesc desc;
    desc.bounds = {0, 0, 4, 4};
    World w(desc, std::make_shared<TopDownGenerator>(3));
    for (int i = 0; i < 5; ++i) {
        w.update(view_at(-500, -500, 1000, 1000));
        w.finish_loading();
    }
    CHECK(w.stats().resident == 16);
    CHECK(w.find_chunk({-1, 0}) == nullptr);
}

TEST_CASE("world works without a job pool") {
    World w(WorldDesc{}, std::make_shared<TopDownGenerator>(3));
    w.update(view_at(0, 0, 10, 10));
    w.finish_loading();
    CHECK(w.find_chunk({0, 0}) != nullptr);
}
