#include "test_types.h"

#include "forge/data/binary.h"

#include <doctest/doctest.h>

#include <cstring>

using namespace forge;
using namespace forge::data;

TEST_CASE("Binary round-trip") {
    test::Character in;
    in.id = Guid::generate();
    in.name = "Борин";
    in.faction = test::Faction::Enemy;
    in.inventory = {{"руда", 3}};
    in.flags = {7, 8, 9};
    in.runtime_timer = 5;

    const std::vector<u8> bytes = to_binary(in);
    test::Character out;
    REQUIRE(from_binary(out, bytes) == BinaryError::None);
    CHECK(out.id == in.id);
    CHECK(out.name == in.name);
    CHECK(out.faction == in.faction);
    CHECK(out.inventory.size() == 1);
    CHECK(out.flags == in.flags);
    CHECK(out.runtime_timer == 0);
}

TEST_CASE("Plain arrays are stored as one block") {
    test::TileChunk chunk;
    chunk.tiles.resize(64 * 64);
    for (usize i = 0; i < chunk.tiles.size(); ++i) chunk.tiles[i] = static_cast<u16>(i * 7);
    const std::vector<u8> bytes = to_binary(chunk);
    CHECK(bytes.size() == sizeof(BinaryHeader) + 4 + 4 + 4 + 64 * 64 * 2);

    test::TileChunk out;
    REQUIRE(from_binary(out, bytes) == BinaryError::None);
    CHECK(out.tiles == chunk.tiles);
}

TEST_CASE("Truncated, foreign and corrupt data is refused") {
    test::Character in;
    in.name = "Борин";
    in.inventory = {{"руда", 3}};
    std::vector<u8> bytes = to_binary(in);
    test::Character out;

    CHECK(from_binary(out, std::span(bytes).first(bytes.size() - 1)) == BinaryError::Truncated);
    CHECK(from_binary(out, std::span(bytes).first(10)) == BinaryError::Truncated);

    test::Item other;
    CHECK(from_binary(other, bytes) == BinaryError::WrongType);

    std::vector<u8> bad = bytes;
    bad[0] ^= 0xFF;
    CHECK(from_binary(out, bad) == BinaryError::BadMagic);

    std::vector<u8> stale = bytes;
    stale[16] ^= 0x01; // schema hash
    CHECK(from_binary(out, stale) == BinaryError::StaleSchema);

    // A huge array count must not allocate gigabytes.
    test::TileChunk chunk;
    std::vector<u8> chunk_bytes = to_binary(chunk);
    const u32 huge = 0x7FFFFFFF;
    std::memcpy(chunk_bytes.data() + sizeof(BinaryHeader) + 8, &huge, 4);
    test::TileChunk chunk_out;
    CHECK(from_binary(chunk_out, chunk_bytes) == BinaryError::Corrupt);
}
