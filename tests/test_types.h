#pragma once

// Types shared by the data tests. They mimic real game data: nesting,
// lists, enums, text and ids.

#include "forge/data/reflect.h"

#include <string>
#include <vector>

namespace test {

enum class Faction : forge::u8 { Neutral, Player, Enemy };

struct Stats {
    forge::f32 health = 100;
    forge::f32 max_health = 100;
    forge::i32 level = 1;
};

struct Item {
    std::string name;
    forge::u32 count = 1;
};

struct Character {
    forge::Guid id;
    std::string name = "Безымянный";
    Faction faction = Faction::Neutral;
    forge::Vec2 position;
    forge::Color tint;
    Stats stats;
    std::vector<Item> inventory;
    std::vector<forge::u16> flags;
    forge::f64 runtime_timer = 0; // transient
};

// A tree: a type that contains a list of itself.
struct Node {
    std::string label;
    std::vector<Node> children;
};

struct TileChunk {
    forge::i32 cx = 0;
    forge::i32 cy = 0;
    std::vector<forge::u16> tiles;
};

} // namespace test

FORGE_REFLECT_DECLARE(test::Faction)
FORGE_REFLECT_DECLARE(test::Stats)
FORGE_REFLECT_DECLARE(test::Item)
FORGE_REFLECT_DECLARE(test::Character)
FORGE_REFLECT_DECLARE(test::Node)
FORGE_REFLECT_DECLARE(test::TileChunk)
