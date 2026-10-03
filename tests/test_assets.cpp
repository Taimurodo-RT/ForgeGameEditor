#include "forge/assets/asset_pipeline.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/data/binary.h"
#include "forge/data/json.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace forge;
using namespace forge::assets;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("forge_test_" + Guid::generate().to_string());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// 2x2 uncompressed 32-bit TGA.
std::string tiny_tga() {
    std::string t(18, '\0');
    t[2] = 2;   // uncompressed true-color
    t[12] = 2;  // width
    t[14] = 2;  // height
    t[16] = 32; // bits per pixel
    t[17] = 8;  // alpha bits
    for (int i = 0; i < 4; ++i) t += std::string("\x10\x20\x30\xff", 4); // BGRA
    return t;
}

Guid meta_id(const fs::path& file) {
    std::ifstream in(fs::path(file) += ".meta");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    AssetMeta meta;
    data::LoadReport r;
    data::from_json(meta, text, r);
    return meta.id;
}

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
};

} // namespace

TEST_CASE("Asset database search: prefixes, Cyrillic, type filter") {
    AssetDatabase db;
    REQUIRE(db.open(":memory:"));
    AssetRecord a{Guid::generate(), "Персонажи/Кузнец/портрет.png", "image", "портрет", "нпс кузница", 10, 1, {}, {}, 1};
    AssetRecord b{Guid::generate(), "Оружие/меч_стальной.png", "image", "меч_стальной", "", 10, 1, {}, {}, 1};
    AssetRecord c{Guid::generate(), "Звуки/меч_удар.ogg", "audio", "меч_удар", "", 10, 1, {}, {}, 1};
    db.begin();
    for (auto* r : {&a, &b, &c}) REQUIRE(db.upsert(*r));
    db.commit();
    CHECK(db.count() == 3);

    CHECK(db.search("кузн").size() == 1);
    CHECK(db.search("КУЗНЕЦ").size() == 1);
    CHECK(db.search("меч").size() == 2);
    CHECK(db.search("меч", 100, "audio").size() == 1);
    CHECK(db.search("кузниц").size() == 1); // tag
    CHECK(db.search("нет такого").empty());
    CHECK(db.search("\"broken (query*").empty()); // punctuation never breaks the query
    CHECK(db.search("").size() == 3);

    // Renaming keeps the id, and search follows the new name.
    b.path = "Оружие/клинок.png";
    b.name = "клинок";
    REQUIRE(db.upsert(b));
    CHECK(db.search("клин").size() == 1);
    CHECK(db.find_by_path("Оружие/клинок.png")->id == b.id);

    db.set_dependencies(a.id, {b.id});
    REQUIRE(db.dependents_of(b.id).size() == 1);
    CHECK(db.dependents_of(b.id)[0] == a.id);

    REQUIRE(db.remove(a.id));
    CHECK_FALSE(db.find(a.id).has_value());
    CHECK(db.dependents_of(b.id).empty());
}

TEST_CASE("Pipeline cooks only what changed and keeps ids across moves") {
    PoolScope pool;
    TempDir dir;
    const fs::path assets = dir.path / utf8_path("Assets");
    write(assets / utf8_path("Тайлы/трава.tga"), tiny_tga());
    write(assets / utf8_path("Тексты/диалог.txt"), "Опять ты?");
    write(assets / utf8_path("битый.png"), "not a png");
    write(assets / utf8_path(".git/config"), "hidden, skipped");

    AssetPipeline pipeline(assets, dir.path / utf8_path("Library"));
    REQUIRE(pipeline.open());
    pipeline.add_default_importers();

    RefreshReport r = pipeline.refresh();
    CHECK(r.files == 3);
    CHECK(r.added == 3);
    CHECK(r.cooked == 2);
    CHECK(r.failed == 1);
    CHECK(fs::exists(fs::path(assets / utf8_path("Тайлы/трава.tga")) += ".meta"));

    // The cooked texture loads.
    auto grass = pipeline.database().find_by_path("Тайлы/трава.tga");
    REQUIRE(grass.has_value());
    CHECK(grass->type == "image");
    std::ifstream in(pipeline.cooked_path(grass->cook_key), std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
    CookedTexture tex;
    REQUIRE(data::from_binary(tex, bytes) == data::BinaryError::None);
    CHECK(tex.width == 2);
    CHECK(tex.height == 2);
    CHECK(tex.rgba8[0] == 0x30); // BGRA in the file, RGBA cooked

    // Nothing changed: nothing is read.
    r = pipeline.refresh();
    CHECK(r.unchanged == 2);
    CHECK(r.cooked == 0);

    // Content changed: cooked again, same id.
    const Guid text_id = meta_id(assets / utf8_path("Тексты/диалог.txt"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(assets / utf8_path("Тексты/диалог.txt"), "Опять ты? Меч ещё не готов.");
    r = pipeline.refresh();
    CHECK(r.cooked == 1);
    CHECK(pipeline.database().find_by_path("Тексты/диалог.txt")->id == text_id);

    // Moved outside the editor without its .meta: recognised by content.
    const Guid grass_id = grass->id;
    fs::create_directories(assets / utf8_path("Новое"));
    fs::rename(assets / utf8_path("Тайлы/трава.tga"), assets / utf8_path("Новое/трава2.tga"));
    fs::remove(fs::path(assets / utf8_path("Тайлы/трава.tga")) += ".meta");
    r = pipeline.refresh();
    CHECK(r.moved == 1);
    CHECK(r.cooked == 0);
    CHECK(r.removed == 0);
    CHECK(pipeline.database().find_by_path("Новое/трава2.tga")->id == grass_id);
    CHECK(pipeline.database().search("трава2").size() == 1);

    // Deleted.
    fs::remove(assets / utf8_path("Тексты/диалог.txt"));
    r = pipeline.refresh();
    CHECK(r.removed == 1);
    CHECK_FALSE(pipeline.database().find(text_id).has_value());
}

TEST_CASE("A copied file with its .meta gets its own id") {
    PoolScope pool;
    TempDir dir;
    const fs::path assets = dir.path / utf8_path("Assets");
    write(assets / utf8_path("a.txt"), "один");
    AssetPipeline pipeline(assets, dir.path / utf8_path("Library"));
    REQUIRE(pipeline.open());
    pipeline.add_default_importers();
    pipeline.refresh();

    fs::copy_file(assets / utf8_path("a.txt"), assets / utf8_path("b.txt"));
    fs::copy_file(fs::path(assets / utf8_path("a.txt")) += ".meta", fs::path(assets / utf8_path("b.txt")) += ".meta");
    RefreshReport r = pipeline.refresh();
    CHECK(r.messages.size() == 1);
    CHECK(pipeline.database().count() == 2);
    CHECK(meta_id(assets / utf8_path("a.txt")) != meta_id(assets / utf8_path("b.txt")));
}
