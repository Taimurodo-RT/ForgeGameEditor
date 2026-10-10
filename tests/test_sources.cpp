// A game's copies of the author's files and the assets of «Ресурсы» they were made of (step 14.1b): the same
// asset picked again gives its copy, a changed asset updates it under the same name, a missing copy comes back,
// a missing asset is said; sources.json names nothing outside the game.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/editor/sources.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::editor::sources;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& file, std::string_view text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    REQUIRE(write_file_atomic(file, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}
std::string text_of(const fs::path& file) {
    std::vector<u8> bytes;
    return read_file(file, bytes) ? std::string(bytes.begin(), bytes.end()) : std::string("<нет>");
}
std::vector<std::string> names(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(path_to_utf8(it->path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}

// A game's folder and «Ресурсы» next to it, with Cyrillic and spaces in the names.
struct Fixture {
    fs::path root = fs::temp_directory_path() / utf8_path("forge_tests_источники игры");
    fs::path game = root / "game", assets = root / "assets";
    Guid crystal = Guid::generate(), ring = Guid::generate();
    std::map<Guid, std::string> where; // the assets as «Ресурсы» has them now
    Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(game, ec);
        put(assets / utf8_path("находки/кристалл.png"), "синий");
        put(assets / utf8_path("звуки/звон.wav"), "дзынь");
        where[crystal] = "находки/кристалл.png";
        where[ring] = "звуки/звон.wav";
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Asset asset(const Guid& id) { return {id, where.at(id), assets / utf8_path(where.at(id))}; }
    std::function<std::optional<Asset>(const Guid&)> finder() {
        return [this](const Guid& id) -> std::optional<Asset> {
            if (!where.count(id) || !fs::exists(assets / utf8_path(where.at(id)))) return std::nullopt;
            return asset(id);
        };
    }
};

} // namespace

TEST_CASE("sources: an asset picked again gives its copy, a changed one updates it under the same name") {
    Fixture f;
    Sources s;
    REQUIRE(s.load(f.game));
    CHECK(s.entries().empty());
    std::string error;
    CHECK(s.copy_in(f.asset(f.crystal), "pictures", &error) == "кристалл.png");
    CHECK(error.empty());
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "синий");
    CHECK(s.copy_in(f.asset(f.ring), "sounds") == "звон.wav");
    const std::string json = text_of(f.game / "sources.json");
    CHECK(json.find("\"pictures/кристалл.png\"") != std::string::npos);
    CHECK(json.find(f.crystal.to_string()) != std::string::npos);
    CHECK(json.find("\"находки/кристалл.png\"") != std::string::npos);
    CHECK(json.find(path_to_utf8(f.root)) == std::string::npos); // nothing but paths inside the game and «Ресурсы»

    // Picked again: the same copy, nothing new.
    CHECK(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"кристалл.png"});

    // Renamed in «Ресурсы» (its Guid kept) and changed: picked again, its copy has the new content, same name.
    fs::rename(f.assets / utf8_path("находки/кристалл.png"), f.assets / utf8_path("находки/кристалл синий.png"));
    f.where[f.crystal] = "находки/кристалл синий.png";
    put(f.assets / utf8_path("находки/кристалл синий.png"), "красный");
    CHECK(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"кристалл.png"});
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "красный");
    CHECK(s.of_file("pictures/кристалл.png")->from == "находки/кристалл синий.png");

    // Read again: the same.
    Sources again;
    REQUIRE(again.load(f.game));
    REQUIRE(again.entries().size() == 2);
    CHECK(again.of_file("pictures/кристалл.png")->asset == f.crystal);
    CHECK(again.of_file("sounds/звон.wav")->asset == f.ring);
}

TEST_CASE("sources: another file of the name stays; the same content with no asset is taken") {
    Fixture f;
    put(f.game / utf8_path("pictures/кристалл.png"), "чужой");
    Sources s;
    REQUIRE(s.load(f.game));
    CHECK(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл 2.png");
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "чужой");
    CHECK(text_of(f.game / utf8_path("pictures/кристалл 2.png")) == "синий");

    put(f.game / utf8_path("sounds/звон.wav"), "дзынь");
    CHECK(s.copy_in(f.asset(f.ring), "sounds") == "звон.wav");
    CHECK(names(f.game / "sounds") == std::vector<std::string>{"звон.wav"});
    CHECK(s.of_file("sounds/звон.wav") != nullptr);
}

TEST_CASE("sources: sync updates changed assets' copies, brings missing copies back and says what is gone") {
    Fixture f;
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");
    REQUIRE(s.copy_in(f.asset(f.ring), "sounds") == "звон.wav");

    Report r = s.sync(f.finder());
    CHECK(!r.changed());
    CHECK((r.gone.empty() && r.lost.empty() && r.errors.empty()));
    const std::string json = text_of(f.game / "sources.json");

    // Edited in another program: its copy follows, under the same name.
    put(f.assets / utf8_path("находки/кристалл.png"), "зелёный");
    r = s.sync(f.finder());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].file == "pictures/кристалл.png");
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "зелёный");
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"кристалл.png"});
    CHECK(!s.sync(f.finder()).changed());

    // Moved in «Ресурсы»: nothing to copy, the new place is remembered.
    fs::create_directories(f.assets / utf8_path("звуки/старые"));
    fs::rename(f.assets / utf8_path("звуки/звон.wav"), f.assets / utf8_path("звуки/старые/звон.wav"));
    f.where[f.ring] = "звуки/старые/звон.wav";
    r = s.sync(f.finder());
    CHECK(!r.changed());
    CHECK(s.of_file("sounds/звон.wav")->from == "звуки/старые/звон.wav");

    // A copy changed in the game's folder and not in «Ресурсы» is left as it is.
    put(f.game / utf8_path("sounds/звон.wav"), "мой");
    CHECK(!s.sync(f.finder()).changed());
    CHECK(text_of(f.game / utf8_path("sounds/звон.wav")) == "мой");

    // The copy deleted: it comes back from its asset.
    fs::remove(f.game / utf8_path("pictures/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.restored.size() == 1);
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "зелёный");

    // The asset deleted: said, its copy stays.
    fs::remove(f.assets / utf8_path("находки/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.gone.size() == 1);
    CHECK(r.gone[0].file == "pictures/кристалл.png");
    CHECK(r.gone[0].from == "находки/кристалл.png");
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "зелёный");

    // Neither: said as lost; the asset back (an undo in «Ресурсы»), the copy is made again.
    fs::remove(f.game / utf8_path("pictures/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.lost.size() == 1);
    CHECK(!fs::exists(f.game / utf8_path("pictures/кристалл.png")));
    put(f.assets / utf8_path("находки/кристалл.png"), "зелёный");
    r = s.sync(f.finder());
    REQUIRE(r.restored.size() == 1);
    CHECK(text_of(f.game / utf8_path("pictures/кристалл.png")) == "зелёный");
}

TEST_CASE("sources: entries that name a file outside the game are left out") {
    Fixture f;
    const std::string id = Guid::generate().to_string();
    put(f.game / "sources.json",
        "{\"files\": {\"../чужое.png\": {\"asset\": \"" + id + "\"}, \"/tmp/x.png\": {\"asset\": \"" + id +
            "\"}, \"pictures\\\\x.png\": {\"asset\": \"" + id + "\"}, \"C:x.png\": {\"asset\": \"" + id +
            "\"}, \"x.png\": {\"asset\": \"" + id + "\"}, \"pictures/ok.png\": {\"asset\": \"" + id +
            "\"}, \"pictures/no id.png\": {\"asset\": \"нет\"}}}");
    Sources s;
    std::string error;
    REQUIRE(s.load(f.game, &error));
    REQUIRE(s.entries().size() == 1);
    CHECK(s.entries()[0].file == "pictures/ok.png");
    CHECK(error.find("../чужое.png") != std::string::npos);
    CHECK(error.find("/tmp/x.png") != std::string::npos);

    put(f.game / "sources.json", "не json");
    CHECK(!s.load(f.game, &error));
}
