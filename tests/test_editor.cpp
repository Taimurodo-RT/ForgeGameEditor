#include "forge/data/reflect.h"
#include "forge/editor/commands.h"
#include "forge/editor/document.h"
#include "forge/editor/inspector.h"
#include "forge/editor/undo.h"
#include "forge/data/json.h"

#include <doctest/doctest.h>

#include <memory>

namespace edtest {
using namespace forge;

enum class Mood : u8 { Calm, Angry };

struct Stats {
    f32 speed = 2;
    i32 level = 1;
    bool alive = true;
    Mood mood = Mood::Calm;
    forge::Vec2 home{};
    forge::Color tint{};
    std::vector<i32> loot;
};

struct Tag {
    std::string text = "tag";
};

} // namespace edtest

FORGE_REFLECT_DECLARE(edtest::Mood)
FORGE_REFLECT_DECLARE(edtest::Stats)
FORGE_REFLECT_DECLARE(edtest::Tag)

FORGE_REFLECT(edtest::Mood, 1) {
    t.value("Calm", edtest::Mood::Calm);
    t.value("Angry", edtest::Mood::Angry);
}
FORGE_REFLECT(edtest::Stats, 1) {
    t.field("speed", &edtest::Stats::speed).range(0, 10).label("Скорость");
    t.field("level", &edtest::Stats::level);
    t.field("alive", &edtest::Stats::alive);
    t.field("mood", &edtest::Stats::mood);
    t.field("home", &edtest::Stats::home);
    t.field("tint", &edtest::Stats::tint);
    t.field("loot", &edtest::Stats::loot);
}
FORGE_REFLECT(edtest::Tag, 1) { t.field("text", &edtest::Tag::text); }

using namespace forge::editor;
using edtest::Stats;
using forge::reflect::type_of;

namespace {

// Edits one field through the inspector path, as the editor does.
void edit(UndoStack& history, Document& doc, ObjectId id, const char* path, const char* text) {
    const auto* type = type_of<Stats>();
    const std::string before = doc.component_json(id, type);
    Stats copy = *doc.component<Stats>(id);
    REQUIRE(set_field_text(type, &copy, path, text));
    history.execute(std::make_unique<SetComponent>(id, type, before, forge::data::to_json(copy, false), path));
}

} // namespace


TEST_CASE("editor document: objects, tree and JSON round trip") {
    Document doc;
    const ObjectId a = doc.create("A");
    const ObjectId b = doc.create("B", a);
    const ObjectId c = doc.create("C", a, 0);
    CHECK(doc.roots().size() == 1);
    CHECK(doc.children_of(a) == std::vector<ObjectId>{c, b});
    doc.add<Stats>(b)->speed = 7;
    doc.add<edtest::Tag>(b)->text = "привет";
    CHECK_FALSE(doc.reparent(a, b)); // not into itself
    CHECK(doc.reparent(c, kNoObject));
    CHECK(doc.roots().size() == 2);

    const std::string saved = doc.save_json();
    Document copy;
    REQUIRE(copy.load_json(saved));
    CHECK(copy.object_count() == 3);
    REQUIRE(copy.component<Stats>(b));
    CHECK(copy.component<Stats>(b)->speed == 7);
    CHECK(copy.component<edtest::Tag>(b)->text == "привет");
    CHECK(copy.find(b)->parent == a);
    // New ids never collide with loaded ones.
    CHECK(copy.create("D") > c);
}

TEST_CASE("editor undo: any action can be undone and redone") {
    Document doc;
    UndoStack history(doc);
    auto create = CreateObject::named(doc, "Герой", kNoObject);
    const ObjectId hero = create->id();
    history.execute(std::move(create));
    history.execute(std::make_unique<AddComponent>(hero, type_of<Stats>()));
    edit(history, doc, hero, "speed", "5");
    history.execute(std::make_unique<RenameObject>(doc, hero, "Борин"));
    CHECK(doc.find(hero)->name == "Борин");
    CHECK(history.size() == 4);
    CHECK(history.undo_label() == "Переименовать");

    // Undo everything, then redo everything: same ids, same values.
    while (history.undo()) {}
    CHECK(doc.object_count() == 0);
    while (history.redo()) {}
    REQUIRE(doc.find(hero));
    CHECK(doc.find(hero)->name == "Борин");
    CHECK(doc.component<Stats>(hero)->speed == 5);

    // Delete restores the subtree with its ids and position.
    const ObjectId child = doc.create("Меч", hero);
    doc.create("Щит", kNoObject);
    history.execute(std::make_unique<DeleteObject>(doc, hero));
    CHECK_FALSE(doc.find(child));
    history.undo();
    REQUIRE(doc.find(child));
    CHECK(doc.find(child)->parent == hero);
    CHECK(doc.index_in_parent(hero) == 0);

    // Remove a component and get it back at the same place with its value.
    history.execute(std::make_unique<AddComponent>(hero, type_of<edtest::Tag>()));
    history.execute(std::make_unique<RemoveComponent>(doc, hero, type_of<Stats>()));
    CHECK_FALSE(doc.component<Stats>(hero));
    history.undo();
    CHECK(doc.component_index(hero, type_of<Stats>()) == 0);
    CHECK(doc.component<Stats>(hero)->speed == 5);
}

TEST_CASE("editor undo: dragging a value is one entry") {
    Document doc;
    UndoStack history(doc);
    const ObjectId id = doc.create("X");
    doc.add<Stats>(id);
    for (int i = 1; i <= 100; ++i) edit(history, doc, id, "speed", std::to_string(i * 0.05).c_str());
    CHECK(history.size() == 1);
    CHECK(doc.component<Stats>(id)->speed == doctest::Approx(5.0));
    // Another field is another entry; after seal() the same field is too.
    edit(history, doc, id, "level", "3");
    history.seal();
    edit(history, doc, id, "level", "4");
    CHECK(history.size() == 3);
    history.undo();
    history.undo();
    history.undo();
    CHECK(doc.component<Stats>(id)->speed == 2);
    CHECK(doc.component<Stats>(id)->level == 1);
}

TEST_CASE("editor undo: groups, redo branch, limit, dirty flag") {
    Document doc;
    UndoStack history(doc);
    history.begin_group("Создать отряд");
    for (int i = 0; i < 12; ++i) history.execute(CreateObject::named(doc, "Солдат", kNoObject));
    history.end_group();
    CHECK(history.size() == 1);
    CHECK(history.undo_label() == "Создать отряд");
    CHECK(doc.object_count() == 12);
    history.undo();
    CHECK(doc.object_count() == 0);

    // A new action after undo drops the redo branch.
    history.execute(CreateObject::named(doc, "Один", kNoObject));
    CHECK_FALSE(history.can_redo());
    CHECK(history.size() == 1);

    // Dirty: saved, change, undo back to saved = clean. Selection is not a change.
    history.mark_saved();
    CHECK_FALSE(history.dirty());
    history.execute(std::make_unique<Select>(doc, std::vector<ObjectId>{doc.roots()[0]}));
    CHECK_FALSE(history.dirty());
    CHECK(doc.selection().size() == 1);
    history.execute(std::make_unique<RenameObject>(doc, doc.roots()[0], "Два"));
    CHECK(history.dirty());
    history.undo();
    CHECK_FALSE(history.dirty());
    history.undo();
    CHECK(doc.selection().empty());

    history.set_limit(5);
    for (int i = 0; i < 20; ++i) history.execute(CreateObject::named(doc, "x", kNoObject));
    CHECK(history.size() == 5);
}

TEST_CASE("editor play: Stop restores the scene, play-time edits are not recorded") {
    Document doc;
    UndoStack history(doc);
    const ObjectId id = doc.create("Игрок");
    doc.add<Stats>(id);
    history.execute(std::make_unique<Select>(doc, std::vector<ObjectId>{id}));
    PlaySession play;
    REQUIRE(play.start(doc, history));
    const auto entries = history.size();
    edit(history, doc, id, "speed", "9");
    doc.component<Stats>(id)->level = 50; // the game changes things
    history.execute(std::make_unique<DeleteObject>(doc, id));
    CHECK(history.size() == entries);
    REQUIRE(play.stop(doc, history));
    REQUIRE(doc.find(id));
    CHECK(doc.component<Stats>(id)->speed == 2);
    CHECK(doc.component<Stats>(id)->level == 1);
    CHECK(doc.selection() == std::vector<ObjectId>{id});
    CHECK(history.recording());
}

TEST_CASE("editor inspector: rows and text edits") {
    Stats s;
    s.loot = {3, 4};
    std::vector<FieldRow> rows;
    describe_fields(type_of<Stats>(), &s, rows);
    REQUIRE(rows.size() == 9); // 7 fields + 2 loot elements
    CHECK(rows[0].label == "Скорость");
    CHECK(rows[0].has_range);
    CHECK(rows[3].options == std::vector<std::string>{"Calm", "Angry"});
    CHECK(rows[4].value == "0, 0");
    CHECK(rows[5].value == "#ffffffff");
    CHECK(rows[7].path == "loot.0");
    CHECK(rows[7].depth == 1);

    const auto* type = type_of<Stats>();
    CHECK(set_field_text(type, &s, "speed", "0,5"));
    CHECK(s.speed == doctest::Approx(0.5));
    CHECK(set_field_text(type, &s, "speed", "99")); // clamped to the range
    CHECK(s.speed == 10);
    CHECK(set_field_text(type, &s, "mood", "Angry"));
    CHECK(s.mood == edtest::Mood::Angry);
    CHECK(set_field_text(type, &s, "home", "1.5, -2"));
    CHECK(s.home.y == -2);
    CHECK(set_field_text(type, &s, "tint", "#ff000080"));
    CHECK(s.tint.g == 0);
    CHECK(set_field_text(type, &s, "loot.1", "40"));
    CHECK(s.loot[1] == 40);
    std::string error;
    CHECK_FALSE(set_field_text(type, &s, "level", "abc", &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(set_field_text(type, &s, "loot.5", "1"));
    CHECK_FALSE(set_field_text(type, &s, "nope", "1"));
}

// --- docking ---------------------------------------------------------------

#include "forge/editor/dock.h"

namespace {

using forge::editor::DockLayout;
using forge::editor::DockRect;

const char* kDockDefault =
    R"({"row":0.2,"a":{"panels":["palette"]},"b":{"row":0.75,"a":{"column":0.7,"a":{"view":true},"b":{"panels":["log","history"]}},"b":{"column":0.4,"a":{"panels":["minimap"]},"b":{"panels":["props"]}}}})";
const std::vector<std::string> kDockPanels{"palette", "log", "history", "minimap", "props"};

} // namespace

TEST_CASE("dock: a layout is read, laid out and written back") {
    DockLayout d;
    REQUIRE(d.load(kDockDefault, kDockPanels));
    d.layout({0, 0, 1000, 600}, 8, 30);
    REQUIRE(d.stacks().size() == 5);
    CHECK(d.splitters().size() == 4);
    DockRect r;
    REQUIRE(d.panel_rect("palette", r));
    CHECK(r.x == 0);
    CHECK(r.y == 30); // below its header
    CHECK(r.w == 198); // (1000 - 8) * 0.2
    CHECK(!d.panel_rect("history", r)); // a hidden tab
    const DockRect view = d.view_rect();
    CHECK(view.x == 206);
    CHECK(view.y == 0);
    // Written and read back: the same places.
    DockLayout e;
    REQUIRE(e.load(d.save(), kDockPanels));
    e.layout({0, 0, 1000, 600}, 8, 30);
    CHECK(e.view_rect() == view);

    // Not a layout of these panels: refused, nothing changes.
    CHECK(!e.load(R"({"row":0.5,"a":{"view":true},"b":{"panels":["palette"]}})", kDockPanels));
    CHECK(!e.load(R"({"row":0.5,"a":{"panels":["palette","log","history","minimap","props"]},"b":{"panels":["props"]}})",
                  kDockPanels));
    CHECK(!e.load("not json", kDockPanels));
    e.layout({0, 0, 1000, 600}, 8, 30);
    CHECK(e.view_rect() == view);
}

TEST_CASE("dock: tabs, moves and splitters") {
    DockLayout d;
    REQUIRE(d.load(kDockDefault, kDockPanels));
    d.layout({0, 0, 1000, 600}, 8, 30);
    DockRect r;

    d.activate("history");
    d.layout({0, 0, 1000, 600}, 8, 30);
    CHECK(d.panel_rect("history", r));
    CHECK(!d.panel_rect("log", r));

    // Minimap dropped on the palette's header: a tab there; its old stack
    // closes and the properties take the whole right column.
    DockRect palette_frame;
    for (const auto& s : d.stacks())
        if (!s.panels.empty() && s.panels[0] == "palette") palette_frame = s.frame;
    auto drop = d.drop_at(palette_frame.x + 20, palette_frame.y + 10, "minimap");
    CHECK(drop.zone == DockLayout::Zone::Center);
    REQUIRE(d.move("minimap", drop));
    d.layout({0, 0, 1000, 600}, 8, 30);
    CHECK(d.stacks().size() == 4);
    REQUIRE(d.panel_rect("minimap", r));
    CHECK(r.x == 0);
    REQUIRE(d.panel_rect("props", r));
    CHECK(r.y == 30); // the whole column now

    // The log dropped on the left edge of the view: a new stack beside it.
    const DockRect view = d.view_rect();
    drop = d.drop_at(view.x + 5, view.y + view.h * 0.5f, "log");
    CHECK(drop.zone == DockLayout::Zone::Left);
    REQUIRE(d.move("log", drop));
    d.layout({0, 0, 1000, 600}, 8, 30);
    REQUIRE(d.panel_rect("log", r));
    CHECK(r.x == view.x);
    CHECK(d.view_rect().x > view.x);
    CHECK(d.panel_rect("history", r)); // alone in its stack now, so shown

    // Into the middle of the view: not a place.
    drop = d.drop_at(d.view_rect().x + d.view_rect().w * 0.5f, d.view_rect().y + d.view_rect().h * 0.5f, "palette");
    CHECK(drop.zone == DockLayout::Zone::None);
    CHECK(!d.move("palette", drop));

    // Every panel is still there exactly once.
    DockLayout check;
    CHECK(check.load(d.save(), kDockPanels));

    // Splitters keep a stack at least kMinSize wide.
    const forge::u64 v = d.version();
    d.drag_splitter(0, -500, 0);
    d.layout({0, 0, 1000, 600}, 8, 30);
    CHECK(d.version() > v);
    REQUIRE(d.panel_rect("minimap", r));
    CHECK(r.w >= DockLayout::kMinSize - 1);
    CHECK(r.w <= DockLayout::kMinSize + 1);
}
