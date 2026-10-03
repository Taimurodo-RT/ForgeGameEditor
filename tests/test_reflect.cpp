#include "test_types.h"

#include <doctest/doctest.h>

using namespace forge;
using namespace forge::reflect;

TEST_CASE("Reflected struct describes its fields") {
    const TypeInfo* t = type_of<test::Character>();
    CHECK(t->kind == Kind::Struct);
    CHECK(t->name == "test::Character");
    CHECK(t->version == 2);
    CHECK(find_type("test::Character") == t);
    CHECK(find_type_by_id(t->id) == t);

    const FieldInfo* stats = t->find_field("stats");
    REQUIRE(stats != nullptr);
    CHECK(stats->type == type_of<test::Stats>());
    CHECK(stats->offset == offsetof(test::Character, stats));

    const FieldInfo* health = type_of<test::Stats>()->find_field("health");
    REQUIRE(health != nullptr);
    CHECK(health->has_range);
    CHECK(health->max == 10000);
    CHECK(health->label == "Здоровье");

    CHECK((t->find_field("runtime_timer")->flags & FieldTransient) != 0);
    CHECK(t->find_field("name")->former_name == "title");
}

TEST_CASE("Arrays and enums are described") {
    const TypeInfo* inv = type_of<std::vector<test::Item>>();
    CHECK(inv->kind == Kind::Array);
    CHECK(inv->element == type_of<test::Item>());
    CHECK(inv->name == "Array<test::Item>");

    const TypeInfo* faction = type_of<test::Faction>();
    CHECK(faction->kind == Kind::Enum);
    CHECK(faction->size == 1);
    REQUIRE(faction->find_enum("Enemy") != nullptr);
    CHECK(faction->find_enum("Enemy")->value == 2);
}

TEST_CASE("A type may contain a list of itself") {
    const TypeInfo* node = type_of<test::Node>();
    CHECK(node->find_field("children")->type->element == node);
    CHECK(node->schema_hash() != 0);
}

TEST_CASE("Schema hash depends on layout, not on values") {
    CHECK(type_of<test::Character>()->schema_hash() == type_of<test::Character>()->schema_hash());
    CHECK(type_of<test::Character>()->schema_hash() != type_of<test::Stats>()->schema_hash());
    CHECK(type_of<test::Stats>()->trivially_copyable);
    CHECK_FALSE(type_of<test::Character>()->trivially_copyable);
}

TEST_CASE("Guid text form round-trips") {
    const Guid g = Guid::generate();
    CHECK_FALSE(g.is_null());
    const std::string s = g.to_string();
    CHECK(s.size() == 36);
    CHECK(Guid::parse(s) == g);
    CHECK_FALSE(Guid::parse("not-a-guid").has_value());
    CHECK(Guid::generate() != g);
}
