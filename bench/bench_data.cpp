// Data benchmark: how fast project data moves between memory, the JSON
// source form and the binary cooked form. Two shapes that dominate big
// games: many small records (entities in a save) and big plain arrays
// (tile chunks of a world).

#include "forge/core/time.h"
#include "forge/data/binary.h"
#include "forge/data/json.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace forge;

namespace bench {

enum class Team : u8 { Neutral, Player, Enemy };

struct Entity {
    Guid id;
    std::string name;
    Team team = Team::Neutral;
    Vec2 position;
    f32 health = 100;
    std::vector<u32> tags;
};

struct Save {
    std::vector<Entity> entities;
};

struct Chunk {
    i32 cx = 0;
    i32 cy = 0;
    std::vector<u16> tiles;
};

struct World {
    std::vector<Chunk> chunks;
};

} // namespace bench

FORGE_REFLECT_DECLARE(bench::Team)
FORGE_REFLECT_DECLARE(bench::Entity)
FORGE_REFLECT_DECLARE(bench::Save)
FORGE_REFLECT_DECLARE(bench::Chunk)
FORGE_REFLECT_DECLARE(bench::World)

FORGE_REFLECT(bench::Team, 1) {
    t.value("Neutral", bench::Team::Neutral);
    t.value("Player", bench::Team::Player);
    t.value("Enemy", bench::Team::Enemy);
}
FORGE_REFLECT(bench::Entity, 1) {
    t.field("id", &bench::Entity::id);
    t.field("name", &bench::Entity::name);
    t.field("team", &bench::Entity::team);
    t.field("position", &bench::Entity::position);
    t.field("health", &bench::Entity::health);
    t.field("tags", &bench::Entity::tags);
}
FORGE_REFLECT(bench::Save, 1) { t.field("entities", &bench::Save::entities); }
FORGE_REFLECT(bench::Chunk, 1) {
    t.field("cx", &bench::Chunk::cx);
    t.field("cy", &bench::Chunk::cy);
    t.field("tiles", &bench::Chunk::tiles);
}
FORGE_REFLECT(bench::World, 1) { t.field("chunks", &bench::World::chunks); }

namespace {

template <typename Fn>
f64 median_ms(int runs, Fn&& fn) {
    std::vector<f64> samples;
    for (int i = 0; i < runs; ++i) {
        const u64 t0 = time_now_ns();
        fn();
        samples.push_back(ns_to_ms(time_now_ns() - t0));
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

template <typename T>
void measure(const char* label, const T& object, int runs) {
    std::string text;
    std::vector<u8> bytes;
    const f64 json_save = median_ms(runs, [&] { text = data::to_json(object, false); });
    const f64 json_load = median_ms(runs, [&] {
        T out;
        data::LoadReport report;
        data::from_json(out, text, report);
    });
    const f64 bin_save = median_ms(runs, [&] { bytes = data::to_binary(object); });
    const f64 bin_load = median_ms(runs, [&] {
        T out;
        data::from_binary(out, bytes);
    });
    std::printf("%s\n", label);
    std::printf("  JSON    %8.1f MB   save %8.2f ms   load %8.2f ms\n", static_cast<f64>(text.size()) / 1e6, json_save, json_load);
    std::printf("  binary  %8.1f MB   save %8.2f ms   load %8.2f ms\n", static_cast<f64>(bytes.size()) / 1e6, bin_save, bin_load);
}

} // namespace

int main() {
    bench::Save save;
    save.entities.resize(200'000); // the "active objects" target
    for (u32 i = 0; i < save.entities.size(); ++i) {
        bench::Entity& e = save.entities[i];
        e.id = Guid::generate();
        e.name = "Объект " + std::to_string(i);
        e.team = static_cast<bench::Team>(i % 3);
        e.position = {static_cast<f32>(i % 4096) * 16.0f, static_cast<f32>(i / 4096) * 16.0f};
        e.health = static_cast<f32>(i % 100);
        e.tags = {i % 7, i % 11};
    }

    // 4 096 chunks of 64x64 tiles: a 4 096 x 4 096 tile region, one layer.
    bench::World world;
    world.chunks.resize(4096);
    for (u32 i = 0; i < world.chunks.size(); ++i) {
        bench::Chunk& c = world.chunks[i];
        c.cx = static_cast<i32>(i % 64);
        c.cy = static_cast<i32>(i / 64);
        c.tiles.resize(64 * 64);
        for (u32 k = 0; k < c.tiles.size(); ++k) c.tiles[k] = static_cast<u16>((k * 31 + i) & 0x3FF);
    }

    measure("200 000 entities (save game)", save, 5);
    measure("4 096 tile chunks, 16.7M tiles (world layer)", world, 5);
    return 0;
}
