#include "forge/core/memory.h"

#include <doctest/doctest.h>

using namespace forge;

TEST_CASE("LinearArena hands out aligned memory and resets") {
    LinearArena arena(1024, MemoryCategory::Frame);
    void* a = arena.alloc(3, 1);
    void* b = arena.alloc(16, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<usize>(b) % 16 == 0);
    CHECK(arena.used() >= 19);

    const usize mark = arena.marker();
    arena.alloc(100);
    arena.rewind(mark);
    CHECK(arena.used() == mark);

    arena.reset();
    CHECK(arena.used() == 0);
    CHECK(arena.peak() >= 119);
}

TEST_CASE("LinearArena returns null when full instead of overrunning") {
    LinearArena arena(64, MemoryCategory::Frame);
    CHECK(arena.alloc(64, 1) != nullptr);
    CHECK(arena.alloc(1, 1) == nullptr);
}

TEST_CASE("Memory is charged to its category") {
    const usize before = memory_stats().bytes[static_cast<usize>(MemoryCategory::Audio)];
    {
        LinearArena arena(4096, MemoryCategory::Audio);
        CHECK(memory_stats().bytes[static_cast<usize>(MemoryCategory::Audio)] == before + 4096);
    }
    CHECK(memory_stats().bytes[static_cast<usize>(MemoryCategory::Audio)] == before);
}

TEST_CASE("BlockPool reuses freed blocks and grows by pages") {
    BlockPool pool(24, 8, 4, MemoryCategory::World);
    void* blocks[6];
    for (auto& b : blocks) b = pool.alloc();
    CHECK(pool.live_blocks() == 6);
    CHECK(pool.page_count() == 2);
    for (auto* b : blocks) CHECK(reinterpret_cast<usize>(b) % 8 == 0);

    pool.free(blocks[2]);
    CHECK(pool.alloc() == blocks[2]);
    for (auto* b : blocks) pool.free(b);
    CHECK(pool.live_blocks() == 0);
}
