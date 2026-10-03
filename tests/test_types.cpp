#include "test_types.h"

#include "forge/data/json.h"

FORGE_REFLECT(test::Faction, 1) {
    t.value("Neutral", test::Faction::Neutral);
    t.value("Player", test::Faction::Player);
    t.value("Enemy", test::Faction::Enemy);
}

FORGE_REFLECT(test::Stats, 1) {
    t.field("health", &test::Stats::health).range(0, 10000).label("Здоровье");
    t.field("max_health", &test::Stats::max_health).range(1, 10000);
    t.field("level", &test::Stats::level).range(1, 100);
}

FORGE_REFLECT(test::Item, 1) {
    t.field("name", &test::Item::name);
    t.field("count", &test::Item::count);
}

FORGE_REFLECT(test::Character, 2) {
    t.field("id", &test::Character::id).read_only();
    t.field("name", &test::Character::name).renamed_from("title");
    t.field("faction", &test::Character::faction);
    t.field("position", &test::Character::position);
    t.field("tint", &test::Character::tint);
    t.field("stats", &test::Character::stats);
    t.field("inventory", &test::Character::inventory);
    t.field("flags", &test::Character::flags);
    t.field("runtime_timer", &test::Character::runtime_timer).transient();
    // Version 1 kept the level at the top level; it moved into stats.
    t.migrate([](forge::u32 from, forge::data::JsonObjectEdit& obj) {
        if (from < 2) {
            if (auto level = obj.get_number("level")) {
                obj.child("stats").set_number("level", *level);
                obj.remove("level");
            }
        }
    });
}

FORGE_REFLECT(test::Node, 1) {
    t.field("label", &test::Node::label);
    t.field("children", &test::Node::children);
}

FORGE_REFLECT(test::TileChunk, 1) {
    t.field("cx", &test::TileChunk::cx);
    t.field("cy", &test::TileChunk::cy);
    t.field("tiles", &test::TileChunk::tiles);
}
