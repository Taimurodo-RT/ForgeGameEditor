#include "test_types.h"

#include "forge/data/json.h"

#include <doctest/doctest.h>

using namespace forge;
using namespace forge::data;

namespace {
test::Character sample() {
    test::Character c;
    c.id = Guid::generate();
    c.name = "Кузнец Борин";
    c.faction = test::Faction::Player;
    c.position = {12.5f, -3.25f};
    c.tint = {1, 0.5f, 0.25f, 1};
    c.stats = {80, 120, 7};
    c.inventory = {{"Железная руда", 3}, {"Стальной меч", 1}};
    c.flags = {1, 2, 65535};
    c.runtime_timer = 42;
    return c;
}
} // namespace

TEST_CASE("JSON round-trip keeps every saved field") {
    const test::Character in = sample();
    const std::string text = to_json(in);
    CHECK(text.find("\"$type\": \"test::Character\"") != std::string::npos);
    CHECK(text.find("\"faction\": \"Player\"") != std::string::npos);
    CHECK(text.find("runtime_timer") == std::string::npos); // transient
    CHECK(text.find("0.25") != std::string::npos);           // floats stay short

    test::Character out;
    LoadReport report;
    REQUIRE(from_json(out, text, report));
    CHECK(report.warnings.empty());
    CHECK(out.id == in.id);
    CHECK(out.name == in.name);
    CHECK(out.faction == in.faction);
    CHECK(out.position == in.position);
    CHECK(out.tint == in.tint);
    CHECK(out.stats.level == 7);
    REQUIRE(out.inventory.size() == 2);
    CHECK(out.inventory[1].name == "Стальной меч");
    CHECK(out.flags == in.flags);
    CHECK(out.runtime_timer == 0);
}

TEST_CASE("Missing fields keep defaults, unknown fields warn") {
    test::Character c;
    LoadReport report;
    REQUIRE(from_json(c, R"({"$type": "test::Character", "$v": 2, "name": "Стражник", "colour": "red"})", report));
    CHECK(c.name == "Стражник");
    CHECK(c.stats.health == 100);
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0].find("colour") != std::string::npos);
}

TEST_CASE("Wrong value types warn with the field path") {
    test::Character c;
    LoadReport report;
    REQUIRE(from_json(c, R"({"$v": 2, "stats": {"level": "high"}, "inventory": [{"count": -1}]})", report));
    CHECK(c.stats.level == 1);
    REQUIRE(report.warnings.size() == 2);
    CHECK(report.warnings[0].rfind("stats.level", 0) == 0);
    CHECK(report.warnings[1].rfind("inventory[0].count", 0) == 0);
}

TEST_CASE("Old versions are migrated before reading") {
    // Version 1: "title" instead of "name", level at the top level.
    test::Character c;
    LoadReport report;
    REQUIRE(from_json(c, R"({"title": "Старый герой", "level": 12, "stats": {"health": 50}})", report));
    CHECK(report.warnings.empty());
    CHECK(c.name == "Старый герой");
    CHECK(c.stats.level == 12);
    CHECK(c.stats.health == 50);
}

TEST_CASE("Broken or foreign files are rejected with a reason") {
    test::Character c;
    LoadReport report;
    CHECK_FALSE(from_json(c, R"({"name": )", report));
    CHECK(report.error.find("syntax") != std::string::npos);

    LoadReport report2;
    CHECK_FALSE(from_json(c, R"({"$type": "test::Item"})", report2));
    CHECK_FALSE(report2.ok());
}

TEST_CASE("Recursive data round-trips") {
    test::Node root{"мир", {{"зона А", {{"дом", {}}}}, {"зона Б", {}}}};
    test::Node out;
    LoadReport report;
    REQUIRE(from_json(out, to_json(root), report));
    REQUIRE(out.children.size() == 2);
    CHECK(out.children[0].children[0].label == "дом");
}
