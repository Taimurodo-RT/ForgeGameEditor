// A game's copies of the author's files and the assets of «Ресурсы» they were made of (step 14.1b): the same
// asset picked again gives its copy, a changed asset updates it under the same name, a missing copy comes back,
// a missing asset is said; a broken asset file is not taken; a copy's name stays its own while its file is missing;
// a change that cannot be recorded leaves nothing of it; sources.json names nothing outside the game.

#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"
#include "forge/audio/audio.h"
#include "forge/core/file.h"
#include "forge/core/jobs.h"
#include "forge/core/path.h"
#include "forge/editor/sources.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::editor::sources;
namespace fs = std::filesystem;

namespace {

// Real files the game reads: a picture of one colour, a sound of some frames.
std::vector<u8> png(u8 r, u8 g, u8 b) {
    assets::CookedTexture t;
    t.width = t.height = 2;
    for (int i = 0; i < 4; ++i) t.rgba8.insert(t.rgba8.end(), {r, g, b, 255});
    std::vector<u8> out;
    if (!assets::encode_image(t, ".png", out)) out.clear(); // checked below: usable() takes none of an empty one
    return out;
}
std::vector<u8> wav(u32 frames) {
    audio::Clip clip;
    clip.samples.assign(static_cast<usize>(frames) * 2, 0.25f);
    std::vector<u8> out;
    if (!audio::encode_wav(clip, out)) out.clear();
    return out;
}
const std::vector<u8> kBlue = png(40, 90, 200), kRed = png(200, 40, 40), kGreen = png(40, 180, 60), kGray = png(128, 128, 128);
const std::vector<u8> kRing = wav(100), kMine = wav(50), kLong = wav(300);

void put(const fs::path& file, std::span<const u8> bytes) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    REQUIRE(write_file_atomic(file, bytes));
}
void put(const fs::path& file, std::string_view text) { put(file, std::span(reinterpret_cast<const u8*>(text.data()), text.size())); }
std::vector<u8> bytes_of(const fs::path& file) {
    std::vector<u8> bytes;
    return read_file(file, bytes) ? bytes : std::vector<u8>();
}
std::string text_of(const fs::path& file) {
    const std::vector<u8> bytes = bytes_of(file);
    return std::string(bytes.begin(), bytes.end());
}
std::vector<std::string> names(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) out.push_back(path_to_utf8(it->path().filename()));
    std::sort(out.begin(), out.end());
    return out;
}
// A file changed in another program: a time «Ресурсы» sees as new.
void touch(const fs::path& file, int seconds) {
    std::error_code ec;
    fs::last_write_time(file, fs::file_time_type::clock::now() + std::chrono::seconds(seconds), ec);
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
        put(assets / utf8_path("находки/кристалл.png"), kBlue);
        put(assets / utf8_path("звуки/звон.wav"), kRing);
        where[crystal] = "находки/кристалл.png";
        where[ring] = "звуки/звон.wav";
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path copy(const char* file) const { return game / utf8_path(file); }
    Asset asset(const Guid& id) { return {id, where.at(id), assets / utf8_path(where.at(id)), {}}; }
    std::function<std::optional<Asset>(const Guid&)> finder() {
        return [this](const Guid& id) -> std::optional<Asset> {
            if (!where.count(id) || !fs::exists(assets / utf8_path(where.at(id)))) return std::nullopt;
            return asset(id);
        };
    }
};

struct PoolScope {
    PoolScope() { jobs::init(3); }
    ~PoolScope() { jobs::shutdown(); }
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
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
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
    put(f.assets / utf8_path("находки/кристалл синий.png"), kRed);
    CHECK(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"кристалл.png"});
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kRed);
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
    put(f.copy("pictures/кристалл.png"), kGray);
    Sources s;
    REQUIRE(s.load(f.game));
    CHECK(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл 2.png");
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kGray);
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kBlue);

    put(f.copy("sounds/звон.wav"), kRing);
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
    CHECK((r.gone.empty() && r.lost.empty() && r.refused.empty() && r.errors.empty()));

    // Edited in another program: its copy follows, under the same name.
    put(f.assets / utf8_path("находки/кристалл.png"), kGreen);
    r = s.sync(f.finder());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].file == "pictures/кристалл.png");
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kGreen);
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
    put(f.copy("sounds/звон.wav"), kMine);
    CHECK(!s.sync(f.finder()).changed());
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kMine);

    // The copy deleted: it comes back from its asset.
    fs::remove(f.copy("pictures/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.restored.size() == 1);
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kGreen);

    // The asset deleted: said, its copy stays.
    fs::remove(f.assets / utf8_path("находки/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.gone.size() == 1);
    CHECK(r.gone[0].file == "pictures/кристалл.png");
    CHECK(r.gone[0].from == "находки/кристалл.png");
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kGreen);

    // Neither: said as lost; the asset back (an undo in «Ресурсы»), the copy is made again.
    fs::remove(f.copy("pictures/кристалл.png"));
    r = s.sync(f.finder());
    REQUIRE(r.lost.size() == 1);
    CHECK(!fs::exists(f.copy("pictures/кристалл.png")));
    put(f.assets / utf8_path("находки/кристалл.png"), kGreen);
    r = s.sync(f.finder());
    REQUIRE(r.restored.size() == 1);
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kGreen);
}

TEST_CASE("sources: a broken asset file is not taken; its copy stays whole until the file is put right") {
    PoolScope pool;
    Fixture f;
    assets::AssetPipeline pipeline(f.assets, f.root / "Library");
    REQUIRE(pipeline.open());
    pipeline.add_default_importers();
    CHECK(pipeline.refresh().failed == 0);
    // «Ресурсы» as the editor gives them: the record of the last import without an error.
    auto as_now = [&](const assets::AssetRecord& a) { return Asset{a.id, a.path, f.assets / utf8_path(a.path), a.source_hash.to_hex()}; };
    auto find = [&](const Guid& id) -> std::optional<Asset> {
        const std::optional<assets::AssetRecord> a = pipeline.database().find(id);
        return a ? std::optional<Asset>(as_now(*a)) : std::nullopt;
    };
    auto record = [&](const char* rel) {
        const std::optional<assets::AssetRecord> a = pipeline.database().find_by_path(rel);
        REQUIRE(a);
        return *a;
    };
    const fs::path picture = f.assets / utf8_path("находки/кристалл.png"), sound = f.assets / utf8_path("звуки/звон.wav");
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(as_now(record("находки/кристалл.png")), "pictures") == "кристалл.png");
    REQUIRE(s.copy_in(as_now(record("звуки/звон.wav")), "sounds") == "звон.wav");
    const Guid picture_id = record("находки/кристалл.png").id, sound_id = record("звуки/звон.wav").id;
    const std::string json = text_of(f.game / "sources.json");

    // Cut short by another program. The picture does not import (its record is the old one); the sound imports
    // (sounds are taken as they are) but does not play.
    put(picture, "PNG, оборванный другой программой");
    put(sound, "WAV, оборванный другой программой");
    touch(picture, 1);
    touch(sound, 1);
    CHECK(pipeline.refresh().failed == 1);
    Report r = s.sync(find);
    CHECK(!r.changed());
    REQUIRE(r.refused.size() == 2);
    for (const Refusal& no : r.refused) {
        CHECK(!no.missing);
        CHECK(!no.why.empty());
    }
    CHECK(r.errors.empty());
    // The copies as they were, still a picture and a sound; sources.json as it was.
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kRing);
    assets::CookedTexture read;
    CHECK(assets::decode_image(bytes_of(f.copy("pictures/кристалл.png")), read));
    CHECK(audio::decode(bytes_of(f.copy("sounds/звон.wav"))) != nullptr);
    CHECK(text_of(f.game / "sources.json") == json);
    // Picked again while broken: not taken, nothing changes.
    std::string error;
    CHECK(s.copy_in(as_now(record("находки/кристалл.png")), "pictures", &error).empty());
    CHECK(error.find("«находки/кристалл.png»") != std::string::npos);
    CHECK(s.copy_in(as_now(record("звуки/звон.wav")), "sounds", &error).empty());
    CHECK(error.find("звук") != std::string::npos);
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
    CHECK(text_of(f.game / "sources.json") == json);
    // The copy gone too: not made of the broken file; said as missing.
    fs::remove(f.copy("pictures/кристалл.png"));
    r = s.sync(find);
    CHECK(r.restored.empty());
    REQUIRE(r.refused.size() == 2);
    const auto gone = std::find_if(r.refused.begin(), r.refused.end(), [](const Refusal& no) { return no.entry.file == "pictures/кристалл.png"; });
    REQUIRE(gone != r.refused.end());
    CHECK(gone->missing);
    CHECK(!fs::exists(f.copy("pictures/кристалл.png")));

    // Put right with new content: the next look takes it, under the same names, from the same assets.
    put(picture, kRed);
    put(sound, kLong);
    touch(picture, 2);
    touch(sound, 2);
    // Not looked at by «Ресурсы» yet: their records are of the content imported before, so both wait for it.
    r = s.sync(find);
    CHECK(!r.changed());
    CHECK(r.refused.size() == 2);
    CHECK(!fs::exists(f.copy("pictures/кристалл.png")));
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kRing);
    CHECK(pipeline.refresh().failed == 0);
    r = s.sync(find);
    REQUIRE(r.restored.size() == 1);
    CHECK(r.restored[0].file == "pictures/кристалл.png");
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].file == "sounds/звон.wav");
    CHECK(r.refused.empty());
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kRed);
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kLong);
    CHECK(audio::decode(bytes_of(f.copy("sounds/звон.wav")))->frames() == audio::decode(kLong)->frames());
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"кристалл.png"});
    CHECK(record("находки/кристалл.png").id == picture_id);
    CHECK(record("звуки/звон.wav").id == sound_id);
    CHECK(s.of_file("pictures/кристалл.png")->asset == picture_id);
    CHECK(s.of_file("sounds/звон.wav")->asset == sound_id);
    CHECK(s.of_file("pictures/кристалл.png")->hash == hash_of(kRed));
}

TEST_CASE("sources: a copy's name stays its own while its file is missing") {
    Fixture f;
    const Guid other = Guid::generate(), twin = Guid::generate();
    put(f.assets / utf8_path("другие/кристалл.png"), kRed);
    f.where[other] = "другие/кристалл.png";
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");

    // A's asset and its copy both gone.
    fs::remove(f.assets / utf8_path("находки/кристалл.png"));
    fs::remove(f.copy("pictures/кристалл.png"));
    REQUIRE(s.sync(f.finder()).lost.size() == 1);
    // B, another asset of the same file name: a name of its own; A's name stays A's.
    CHECK(s.copy_in(f.asset(other), "pictures") == "кристалл 2.png");
    CHECK(!fs::exists(f.copy("pictures/кристалл.png")));
    REQUIRE(s.entries().size() == 2);
    CHECK(s.of_file("pictures/кристалл.png")->asset == f.crystal);
    CHECK(s.of_file("pictures/кристалл 2.png")->asset == other);
    const std::string json = text_of(f.game / "sources.json");
    CHECK(json.find("\"pictures/кристалл.png\"") == json.rfind("\"pictures/кристалл.png\"")); // one key, once

    // A back: its copy is A's again, B's stays B's.
    put(f.assets / utf8_path("находки/кристалл.png"), kBlue);
    Report r = s.sync(f.finder());
    REQUIRE(r.restored.size() == 1);
    CHECK(r.restored[0].file == "pictures/кристалл.png");
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kRed);
    // B changed: only B's copy follows.
    put(f.assets / utf8_path("другие/кристалл.png"), kGreen);
    r = s.sync(f.finder());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].file == "pictures/кристалл 2.png");
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kGreen);

    // Read again: the same two.
    Sources again;
    REQUIRE(again.load(f.game));
    REQUIRE(again.entries().size() == 2);
    CHECK(again.of_file("pictures/кристалл.png")->asset == f.crystal);
    CHECK(again.of_file("pictures/кристалл 2.png")->asset == other);

    // The same content, another Guid: a copy of its own, which A's changes do not touch.
    put(f.assets / utf8_path("близнец/кристалл.png"), kBlue);
    f.where[twin] = "близнец/кристалл.png";
    CHECK(s.copy_in(f.asset(twin), "pictures") == "кристалл 3.png");
    put(f.assets / utf8_path("находки/кристалл.png"), kGray);
    r = s.sync(f.finder());
    REQUIRE(r.updated.size() == 1);
    CHECK(r.updated[0].file == "pictures/кристалл.png");
    CHECK(bytes_of(f.copy("pictures/кристалл 3.png")) == kBlue);

    // Only B's copy missing: picked again, B gets its own name back.
    fs::remove(f.copy("pictures/кристалл 2.png"));
    CHECK(s.copy_in(f.asset(other), "pictures") == "кристалл 2.png");
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kGreen);
    CHECK(s.entries().size() == 3);
}

TEST_CASE("sources: names that differ only in case are one file: each asset keeps a copy of its own") {
    // Where case does not count (Windows) «Crystal.png» and «crystal.PNG» are one file, «Кристалл» and «кристалл»
    // too: a game's folder keeps them apart on every system.
    Fixture f;
    const Guid a = Guid::generate(), b = Guid::generate(), ca = Guid::generate(), cb = Guid::generate();
    put(f.assets / utf8_path("first/Crystal.png"), kBlue);
    put(f.assets / utf8_path("second/crystal.PNG"), kRed);
    put(f.assets / utf8_path("первые/Кристалл.png"), kBlue);
    put(f.assets / utf8_path("вторые/кристалл.png"), kRed);
    f.where[a] = "first/Crystal.png";
    f.where[b] = "second/crystal.PNG";
    f.where[ca] = "первые/Кристалл.png";
    f.where[cb] = "вторые/кристалл.png";
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(f.asset(a), "pictures") == "Crystal.png");
    REQUIRE(s.copy_in(f.asset(ca), "pictures") == "Кристалл.png");

    // A's asset and copy gone (A's name stays A's): B gets a name of its own.
    fs::remove(f.assets / utf8_path("first/Crystal.png"));
    fs::remove(f.assets / utf8_path("первые/Кристалл.png"));
    fs::remove(f.copy("pictures/Crystal.png"));
    fs::remove(f.copy("pictures/Кристалл.png"));
    CHECK(s.sync(f.finder()).lost.size() == 2);
    CHECK(s.copy_in(f.asset(b), "pictures") == "crystal 2.PNG");
    CHECK(s.copy_in(f.asset(cb), "pictures") == "кристалл 2.png");
    CHECK(names(f.game / "pictures") == std::vector<std::string>{"crystal 2.PNG", "кристалл 2.png"});
    CHECK(s.of_file("pictures/Crystal.png")->asset == a);
    CHECK(s.of_file("pictures/Кристалл.png")->asset == ca);
    CHECK(s.of_file("pictures/crystal.png")->asset == a); // one file, whatever the case it is asked in
    CHECK(s.of_file("pictures/КРИСТАЛЛ.PNG")->asset == ca);

    // A back: its own copy again; B's stays B's.
    put(f.assets / utf8_path("first/Crystal.png"), kBlue);
    put(f.assets / utf8_path("первые/Кристалл.png"), kBlue);
    Report r = s.sync(f.finder());
    CHECK(r.restored.size() == 2);
    CHECK(bytes_of(f.copy("pictures/Crystal.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/Кристалл.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/crystal 2.PNG")) == kRed);
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kRed);
    // B changed: only B's copies follow.
    put(f.assets / utf8_path("second/crystal.PNG"), kGreen);
    put(f.assets / utf8_path("вторые/кристалл.png"), kGreen);
    r = s.sync(f.finder());
    REQUIRE(r.updated.size() == 2);
    CHECK(bytes_of(f.copy("pictures/Crystal.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/Кристалл.png")) == kBlue);
    CHECK(bytes_of(f.copy("pictures/crystal 2.PNG")) == kGreen);
    CHECK(bytes_of(f.copy("pictures/кристалл 2.png")) == kGreen);
    // Read again: four copies, each of its own asset.
    Sources again;
    REQUIRE(again.load(f.game));
    REQUIRE(again.entries().size() == 4);
    CHECK(again.of_file("pictures/Crystal.png")->asset == a);
    CHECK(again.of_file("pictures/crystal 2.PNG")->asset == b);
    CHECK(again.of_file("pictures/Кристалл.png")->asset == ca);
    CHECK(again.of_file("pictures/кристалл 2.png")->asset == cb);

    // A file of the folder in another case, of no asset and another content, is not written over.
    const Guid c = Guid::generate();
    put(f.assets / utf8_path("третьи/камень.png"), kGreen);
    f.where[c] = "третьи/камень.png";
    put(f.copy("pictures/КАМЕНЬ.png"), kGray);
    CHECK(s.copy_in(f.asset(c), "pictures") == "камень 2.png");
    CHECK(bytes_of(f.copy("pictures/КАМЕНЬ.png")) == kGray);
    // One with the same content is taken as it is, under its own name.
    const Guid d = Guid::generate();
    put(f.assets / utf8_path("третьи/ЛИСТ.png"), kGray);
    f.where[d] = "третьи/ЛИСТ.png";
    put(f.copy("pictures/лист.png"), kGray);
    CHECK(s.copy_in(f.asset(d), "pictures") == "лист.png");
}

TEST_CASE("sources: one name where case does not count, in any alphabet") {
    // Unicode case folding, not a list of alphabets: Latin Extended-A, -B, -C, -D and Additional, Cyrillic and its
    // Supplement and Extended-B, Greek, Armenian, Cherokee, Deseret (outside 16 bits), signs with letters' cases.
    const char* one[][2] = {
        {"Crystal.png", "crystal.PNG"}, {"Кристалл.png", "кристалл.png"}, {"Łódź.png", "łódź.png"},
        {"Ґрунт.png", "ґрунт.png"},     {"ŐRSÉG.png", "őrség.png"},       {"Ǆ.png", "ǆ.png"},
        {"Ǆ.png", "ǅ.png"},             {"ẠNH.png", "ạnh.png"},           {"Ⱡ.png", "ⱡ.png"},
        {"Ꜳ.png", "ꜳ.png"},             {"Ԁ.png", "ԁ.png"},               {"Ꙁ.png", "ꙁ.png"},
        {"ЀЍЎ.png", "ѐѝў.png"},         {"Ӂ.png", "ӂ.png"},               {"ΩΣ.png", "ως.png"},
        {"Ա.png", "ա.png"},             {"Ꭰ.png", "ꭰ.png"},               {"𐐀.png", "𐐨.png"},
        {"Ⅻ.png", "ⅻ.png"},             {"Ⓐ.png", "ⓐ.png"},               {"k.png", "K.png"}, // the Kelvin sign
        {"Straße.png", "STRASSE.png"}, // wider than NTFS: only another name for a copy
        {"pictures/Łódź.png", "PICTURES/łódź.png"},
    };
    for (const auto& [a, b] : one) {
        CAPTURE(a);
        CAPTURE(b);
        CHECK(same_name(a, b));
        CHECK(same_name(b, a));
    }
    const char* two[][2] = {
        {"e.png", "е.png"}, // Latin e, Cyrillic е
        {"a.png", "b.png"}, {"Łódź.png", "Lodz.png"}, {"ґрунт.png", "грунт.png"}, {"pictures/a.png", "sounds/a.png"},
        {"кристалл.png", "кристалл 2.png"},
    };
    for (const auto& [a, b] : two) {
        CAPTURE(a);
        CAPTURE(b);
        CHECK_FALSE(same_name(a, b));
    }
}

TEST_CASE("sources: a name that differs only in case, in any alphabet, with the first copy there or missing") {
    // Two assets with different Guids whose file names are one file where case does not count. With the first
    // copy there, or with it and its asset gone: the second gets a name of its own; the first comes back as its
    // own; the second changed changes only its own; read again, each copy is its asset's.
    const char* pairs[][3] = {
        {"Crystal.png", "crystal.PNG", "crystal 2.PNG"}, {"Кристалл.png", "кристалл.png", "кристалл 2.png"},
        {"Łódź.png", "łódź.png", "łódź 2.png"},          {"Ґрунт.png", "ґрунт.png", "ґрунт 2.png"},
        {"Ԁара.png", "ԁара.png", "ԁара 2.png"},          {"Ǆemal.png", "ǆemal.png", "ǆemal 2.png"},
    };
    for (const bool missing : {false, true})
        for (const auto& [first, second, own] : pairs) {
            CAPTURE(missing);
            CAPTURE(first);
            Fixture f;
            const Guid a = Guid::generate(), b = Guid::generate();
            const std::string asset_a = std::string("первые/") + first, asset_b = std::string("вторые/") + second;
            const std::string copy_a = std::string("pictures/") + first, copy_b = std::string("pictures/") + own;
            put(f.assets / utf8_path(asset_a), kBlue);
            put(f.assets / utf8_path(asset_b), kRed);
            f.where[a] = asset_a;
            f.where[b] = asset_b;
            Sources s;
            REQUIRE(s.load(f.game));
            REQUIRE(s.copy_in(f.asset(a), "pictures") == first);
            if (missing) {
                fs::remove(f.assets / utf8_path(asset_a));
                fs::remove(f.game / utf8_path(copy_a));
                CHECK(s.sync(f.finder()).lost.size() == 1);
            }
            std::string error;
            CHECK(s.copy_in(f.asset(b), "pictures", &error) == own);
            CHECK(error.empty());
            CHECK(bytes_of(f.game / utf8_path(copy_b)) == kRed);
            CHECK(names(f.game / "pictures") == (missing ? std::vector<std::string>{own} : std::vector<std::string>{first, own}));
            if (!missing) CHECK(bytes_of(f.game / utf8_path(copy_a)) == kBlue); // not written over
            REQUIRE(s.of_file(std::string("pictures/") + second));
            CHECK(s.of_file(std::string("pictures/") + second)->asset == a); // A's name, whatever the case
            CHECK(s.of_file(copy_b)->asset == b);

            if (missing) {
                put(f.assets / utf8_path(asset_a), kBlue);
                const Report back = s.sync(f.finder());
                CHECK(back.restored.size() == 1);
                CHECK(back.errors.empty());
            }
            CHECK(bytes_of(f.game / utf8_path(copy_a)) == kBlue);
            CHECK(bytes_of(f.game / utf8_path(copy_b)) == kRed);
            put(f.assets / utf8_path(asset_b), kGreen);
            const Report r = s.sync(f.finder());
            REQUIRE(r.updated.size() == 1);
            CHECK(r.updated[0].asset == b);
            CHECK(bytes_of(f.game / utf8_path(copy_a)) == kBlue);
            CHECK(bytes_of(f.game / utf8_path(copy_b)) == kGreen);

            Sources again;
            error.clear();
            REQUIRE(again.load(f.game, &error));
            CHECK(error.empty());
            REQUIRE(again.entries().size() == 2);
            REQUIRE(again.of_file(copy_a));
            REQUIRE(again.of_file(copy_b));
            CHECK(again.of_file(copy_a)->asset == a);
            CHECK(again.of_file(copy_b)->asset == b);
            CHECK_FALSE(again.sync(f.finder()).changed());
        }
}

TEST_CASE("sources: two names of one file of the disk: neither copy is written over for the other") {
    // Where the file system takes two names for one file (as Windows takes «Łódź.png» for «łódź.png»; here a
    // hard link makes it so on any system), the other asset's copy is not written through the other name.
    Fixture f;
    const Guid a = Guid::generate(), b = Guid::generate(), d = Guid::generate();
    put(f.assets / utf8_path("первые/один.png"), kBlue);
    put(f.assets / utf8_path("вторые/другой.png"), kRed);
    f.where[a] = "первые/один.png";
    f.where[b] = "вторые/другой.png";
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(f.asset(a), "pictures") == "один.png");
    REQUIRE(s.copy_in(f.asset(b), "pictures") == "другой.png");
    // B's copy gone, and its name now one file with A's.
    REQUIRE(fs::remove(f.copy("pictures/другой.png")));
    std::error_code ec;
    fs::create_hard_link(f.copy("pictures/один.png"), f.copy("pictures/другой.png"), ec);
    REQUIRE_FALSE(ec);

    put(f.assets / utf8_path("вторые/другой.png"), kGreen);
    const Report r = s.sync(f.finder());
    CHECK(r.updated.empty());
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("один файл") != std::string::npos);
    CHECK(bytes_of(f.copy("pictures/один.png")) == kBlue);
    std::string error;
    CHECK(s.copy_in(f.asset(b), "pictures", &error).empty());
    CHECK(error.find("один файл") != std::string::npos);
    CHECK(bytes_of(f.copy("pictures/один.png")) == kBlue);

    // A file of the same content that is another copy on the disk is not taken as a new asset's.
    put(f.assets / utf8_path("третьи/лист.png"), kBlue);
    f.where[d] = "третьи/лист.png";
    fs::create_hard_link(f.copy("pictures/один.png"), f.copy("pictures/лист.png"), ec);
    REQUIRE_FALSE(ec);
    CHECK(s.copy_in(f.asset(d), "pictures") == "лист 2.png");

    // Read again: one file of the disk named twice is one entry, the first named, and said.
    Sources again;
    error.clear();
    REQUIRE(again.load(f.game, &error));
    CHECK(error.find("назван дважды") != std::string::npos);
    CHECK(again.entries().size() == 2); // «другой.png» (first by name) and «лист 2.png»
    CHECK(again.of_file("pictures/лист 2.png"));
}

TEST_CASE("sources: a change sources.json cannot record leaves nothing of it") {
    Fixture f;
    const Guid stone = Guid::generate();
    put(f.assets / utf8_path("другие/камень.png"), kGray);
    f.where[stone] = "другие/камень.png";
    Sources s;
    REQUIRE(s.load(f.game));
    REQUIRE(s.copy_in(f.asset(f.crystal), "pictures") == "кристалл.png");
    REQUIRE(s.copy_in(f.asset(f.ring), "sounds") == "звон.wav");
    const std::string json = text_of(f.game / "sources.json");
    const std::string picture_hash = s.of_file("pictures/кристалл.png")->hash, sound_hash = s.of_file("sounds/звон.wav")->hash;

    // sources.json cannot be written: a folder where its temporary file goes.
    fs::create_directories(f.game / "sources.json.tmp");
    put(f.assets / utf8_path("находки/кристалл.png"), kRed);
    put(f.assets / utf8_path("звуки/звон.wav"), kLong);
    // The copy it has, picked again after its asset changed: as it was.
    std::string error;
    CHECK(s.copy_in(f.asset(f.crystal), "pictures", &error).empty());
    CHECK(error.find("sources.json") != std::string::npos);
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kBlue);
    CHECK(s.of_file("pictures/кристалл.png")->hash == picture_hash);
    // A new copy: not left behind.
    CHECK(s.copy_in(f.asset(stone), "pictures", &error).empty());
    CHECK(!fs::exists(f.copy("pictures/камень.png")));
    CHECK(s.entries().size() == 2);
    // A look at several: nothing said done, every copy as it was, a missing one not made.
    fs::remove(f.copy("pictures/кристалл.png"));
    Report r = s.sync(f.finder());
    CHECK(!r.changed());
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("ничего не перенесено") != std::string::npos);
    CHECK(!fs::exists(f.copy("pictures/кристалл.png")));
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kRing);
    CHECK(s.of_file("pictures/кристалл.png")->hash == picture_hash);
    CHECK(s.of_file("sounds/звон.wav")->hash == sound_hash);
    CHECK(text_of(f.game / "sources.json") == json);
    CHECK(names(f.game / "pictures").empty());

    // Unblocked: the same goes through.
    fs::remove_all(f.game / "sources.json.tmp");
    r = s.sync(f.finder());
    CHECK(r.errors.empty());
    REQUIRE(r.restored.size() == 1);
    REQUIRE(r.updated.size() == 1);
    CHECK(bytes_of(f.copy("pictures/кристалл.png")) == kRed);
    CHECK(bytes_of(f.copy("sounds/звон.wav")) == kLong);
    CHECK(s.copy_in(f.asset(stone), "pictures") == "камень.png");
    Sources again;
    REQUIRE(again.load(f.game));
    REQUIRE(again.entries().size() == 3);
    CHECK(again.of_file("pictures/кристалл.png")->hash == hash_of(kRed));
    CHECK(again.of_file("sounds/звон.wav")->hash == hash_of(kLong));
}

TEST_CASE("sources: entries that name a file outside the game, or one named twice (in any case), are left out") {
    Fixture f;
    const std::string id = Guid::generate().to_string(), other = Guid::generate().to_string();
    put(f.game / "sources.json",
        "{\"files\": {\"../чужое.png\": {\"asset\": \"" + id + "\"}, \"/tmp/x.png\": {\"asset\": \"" + id +
            "\"}, \"pictures\\\\x.png\": {\"asset\": \"" + id + "\"}, \"C:x.png\": {\"asset\": \"" + id +
            "\"}, \"x.png\": {\"asset\": \"" + id + "\"}, \"pictures/ok.png\": {\"asset\": \"" + id +
            "\"}, \"pictures/no id.png\": {\"asset\": \"нет\"}, \"pictures/ok.png\": {\"asset\": \"" + other +
            "\"}, \"pictures/OK.PNG\": {\"asset\": \"" + other + "\"}}}");
    Sources s;
    std::string error;
    REQUIRE(s.load(f.game, &error));
    REQUIRE(s.entries().size() == 1);
    CHECK(s.entries()[0].file == "pictures/ok.png");
    CHECK(s.entries()[0].asset.to_string() == id);
    CHECK(error.find("../чужое.png") != std::string::npos);
    CHECK(error.find("/tmp/x.png") != std::string::npos);
    CHECK(error.find("назван дважды") != std::string::npos);
    CHECK(error.find("pictures/OK.PNG") != std::string::npos); // the same file where case does not count

    // Latin Extended: one file too.
    put(f.game / "sources.json", "{\"files\": {\"pictures/Łódź.png\": {\"asset\": \"" + id + "\"}, \"pictures/łódź.png\": {\"asset\": \"" +
                                     other + "\"}}}");
    error.clear();
    REQUIRE(s.load(f.game, &error));
    REQUIRE(s.entries().size() == 1);
    REQUIRE(s.of_file("pictures/ŁÓDŹ.PNG"));
    CHECK(s.of_file("pictures/ŁÓDŹ.PNG")->asset.to_string() == id);
    CHECK(error.find("pictures/łódź.png") != std::string::npos);

    put(f.game / "sources.json", "не json");
    CHECK(!s.load(f.game, &error));
}

TEST_CASE("sources: only a picture or a sound the game reads goes to its folder") {
    for (const std::vector<u8>* made : {&kBlue, &kRed, &kGreen, &kGray, &kRing, &kMine, &kLong}) CHECK(!made->empty());
    std::string why;
    CHECK(usable("pictures", kBlue));
    CHECK(!usable("pictures", kRing, &why));
    CHECK(why.find("картинк") != std::string::npos);
    CHECK(usable("sounds", kRing));
    CHECK(!usable("sounds", kBlue, &why));
    CHECK(why.find("звук") != std::string::npos);
    const std::string text = "просто текст";
    CHECK(usable("data", std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}
