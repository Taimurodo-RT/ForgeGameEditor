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

// --- older layouts ----------------------------------------------------------

namespace {

enum class MigKind1 : u8 { A, B, C };
enum class MigKind2 : u16 { C, A, B, D }; // renumbered and widened

struct MigPoint1 {
    f32 x = 0;
    i32 y = 0;
};
struct MigPoint2 {
    f64 y = 0;
    f64 x = 0;
    f32 z = 5; // new
};
struct MigItem1 {
    std::string what;
    u8 count = 0;
};
struct MigItem2 {
    u32 count = 0;
    std::string what;
    bool rare = true; // new
};
struct MigV1 {
    std::string name;
    i32 hp = 0;
    f32 speed = 0;
    MigKind1 kind = MigKind1::A;
    MigPoint1 at;
    std::vector<MigItem1> items;
    i32 gone = 0;
    std::string title;
    std::vector<i16> marks;
};
struct MigV2 {
    f32 hp = 0; // was i32
    std::string name;
    f64 speed = 0;
    MigKind2 kind = MigKind2::D;
    MigPoint2 at;
    std::vector<MigItem2> items;
    i32 added = 77;      // new
    std::string caption; // was "title"
    std::vector<i32> marks;
};

} // namespace

FORGE_REFLECT_DECLARE(MigKind1)
FORGE_REFLECT_DECLARE(MigKind2)
FORGE_REFLECT_DECLARE(MigPoint1)
FORGE_REFLECT_DECLARE(MigPoint2)
FORGE_REFLECT_DECLARE(MigItem1)
FORGE_REFLECT_DECLARE(MigItem2)
FORGE_REFLECT_DECLARE(MigV1)
FORGE_REFLECT_DECLARE(MigV2)
FORGE_REFLECT(MigKind1, 1) {
    t.value("A", MigKind1::A);
    t.value("B", MigKind1::B);
    t.value("C", MigKind1::C);
}
FORGE_REFLECT(MigKind2, 1) {
    t.value("C", MigKind2::C);
    t.value("A", MigKind2::A);
    t.value("B", MigKind2::B);
    t.value("D", MigKind2::D);
}
FORGE_REFLECT(MigPoint1, 1) {
    t.field("x", &MigPoint1::x);
    t.field("y", &MigPoint1::y);
}
FORGE_REFLECT(MigPoint2, 1) {
    t.field("y", &MigPoint2::y);
    t.field("x", &MigPoint2::x);
    t.field("z", &MigPoint2::z);
}
FORGE_REFLECT(MigItem1, 1) {
    t.field("what", &MigItem1::what);
    t.field("count", &MigItem1::count);
}
FORGE_REFLECT(MigItem2, 1) {
    t.field("count", &MigItem2::count);
    t.field("what", &MigItem2::what);
    t.field("rare", &MigItem2::rare);
}
FORGE_REFLECT(MigV1, 1) {
    t.field("name", &MigV1::name);
    t.field("hp", &MigV1::hp);
    t.field("speed", &MigV1::speed);
    t.field("kind", &MigV1::kind);
    t.field("at", &MigV1::at);
    t.field("items", &MigV1::items);
    t.field("gone", &MigV1::gone);
    t.field("title", &MigV1::title);
    t.field("marks", &MigV1::marks);
}
FORGE_REFLECT(MigV2, 2) {
    t.field("hp", &MigV2::hp);
    t.field("name", &MigV2::name);
    t.field("speed", &MigV2::speed);
    t.field("kind", &MigV2::kind);
    t.field("at", &MigV2::at);
    t.field("items", &MigV2::items);
    t.field("added", &MigV2::added);
    t.field("caption", &MigV2::caption).renamed_from("title");
    t.field("marks", &MigV2::marks);
}

TEST_CASE("Data of an older layout is read through its saved description") {
    MigV1 in;
    in.name = "Сундук";
    in.hp = 42;
    in.speed = 1.5f;
    in.kind = MigKind1::B;
    in.at = {3.25f, -7};
    in.items = {{"руда", 3}, {"ключ", 1}};
    in.gone = 9;
    in.title = "Старый";
    in.marks = {-2, 300};

    std::vector<u8> bytes, layout;
    append_binary(reflect::type_of<MigV1>(), &in, bytes);
    append_schema(reflect::type_of<MigV1>(), layout);
    const usize payload_size = bytes.size();
    bytes.push_back(0xAB); // the next payload

    SavedSchema schema;
    std::span<const u8> l(layout);
    REQUIRE(schema.parse(l));
    CHECK(l.empty());

    MigV2 out;
    std::span<const u8> b(bytes);
    REQUIRE(schema.read(reflect::type_of<MigV2>(), &out, b) == BinaryError::None);
    CHECK(b.size() == bytes.size() - payload_size); // stops at its own end
    CHECK(out.name == "Сундук");
    CHECK(out.hp == 42.0f);
    CHECK(out.speed == 1.5);
    CHECK(out.kind == MigKind2::B); // by name
    CHECK(out.at.x == 3.25);
    CHECK(out.at.y == -7.0);
    CHECK(out.at.z == 5.0f); // new field keeps its default
    REQUIRE(out.items.size() == 2);
    CHECK(out.items[0].what == "руда");
    CHECK(out.items[0].count == 3);
    CHECK(out.items[0].rare);
    CHECK(out.items[1].what == "ключ");
    CHECK(out.added == 77);
    CHECK(out.caption == "Старый"); // renamed
    CHECK(out.marks == std::vector<i32>{-2, 300});

    // The same layout reads into itself.
    MigV1 same;
    std::span<const u8> b2(bytes.data(), payload_size);
    REQUIRE(schema.read(reflect::type_of<MigV1>(), &same, b2) == BinaryError::None);
    CHECK(same.gone == 9);
    CHECK(same.items[1].count == 1);

    // Broken input is refused, never trusted.
    std::span<const u8> cut(bytes.data(), payload_size - 3);
    MigV2 broken;
    CHECK(schema.read(reflect::type_of<MigV2>(), &broken, cut) != BinaryError::None);
    for (usize n = 0; n + 1 < layout.size(); ++n) {
        SavedSchema s;
        std::span<const u8> part(layout.data(), n);
        CHECK_FALSE(s.parse(part));
    }
}
