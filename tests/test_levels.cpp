// A game's levels (game/levels.json): a game without the file has its one level and nothing is written into it;
// ids that never change and the folders that come from them; names; a file that cannot be used, or is read only in
// part, is told and kept as it is; a change of the list that cannot be written leaves the list and the game as they
// were.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/level/levels.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace forge;
using namespace forge::level;
namespace fs = std::filesystem;

namespace {

struct Game {
    fs::path root;
    fs::path game;
    explicit Game(std::string_view name = "forge_tests_уровни игры") : root(fs::temp_directory_path() / utf8_path(name)), game(root / "game") {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(game / "level", ec);
        write_text(game / "level" / "areas.json", "{\"areas\":[]}\n");
    }
    ~Game() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void write_text(const fs::path& file, const std::string& text) {
        REQUIRE(write_file_atomic(file, {reinterpret_cast<const u8*>(text.data()), text.size()}));
    }
    std::string text(const fs::path& file) const {
        std::vector<u8> bytes;
        if (!read_file(file, bytes)) return {};
        return {bytes.begin(), bytes.end()};
    }
    std::string list_text() const { return text(game / kLevelsFile); }
};

bool has(const std::vector<std::string>& notes, std::string_view part) {
    for (const std::string& n : notes)
        if (n.find(part) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("levels: a game without levels.json has its one level, and nothing is written into it") {
    Game g;
    const LevelList list = read_levels(g.game);
    CHECK_FALSE(list.from_file);
    CHECK_FALSE(list.broken);
    REQUIRE(list.levels.size() == 1);
    CHECK(list.levels[0].id == "level");
    CHECK(list.levels[0].name == "Уровень 1");
    CHECK(list.start == "level");
    CHECK(list.start_level().id == "level");
    CHECK(level_folder(g.game, "level") == g.game / "level");
    std::string id;
    CHECK(start_level_folder(g.game, &id) == g.game / "level");
    CHECK(id == "level");
    std::error_code ec;
    CHECK_FALSE(fs::exists(g.game / kLevelsFile, ec));
    CHECK_FALSE(fs::exists(g.game / "levels", ec));
}

TEST_CASE("levels: a new level, renamed and made the start; ids and folders stay, the old level is not touched") {
    Game g;
    const std::string areas_before = g.text(g.game / "level" / "areas.json");
    LevelList list = read_levels(g.game);
    CHECK(free_level_name(list) == "Уровень 2");
    std::string id, error;
    REQUIRE(add_level(g.game, list, "Уровень 2", id, &error));
    CHECK(error.empty());
    CHECK(valid_level_id(id));
    CHECK(id.size() == 9);
    CHECK(id[0] == 'l');
    std::error_code ec;
    CHECK(fs::is_directory(g.game / "levels" / id, ec));
    CHECK(fs::is_empty(g.game / "levels" / id, ec));
    CHECK(level_folder(g.game, id) == g.game / "levels" / id);
    REQUIRE(list.levels.size() == 2);
    CHECK(list.levels[0].id == "level");
    CHECK(list.levels[1].id == id);
    CHECK(list.start == "level");
    CHECK(free_level_name(list) == "Уровень 3");

    REQUIRE(rename_level(g.game, list, id, "  Пещера ", &error));
    CHECK(list.find(id)->name == "Пещера");
    REQUIRE(set_start_level(g.game, list, id, &error));
    CHECK(list.start == id);

    // Read again: the same list; the file in the order of the model.
    const LevelList again = read_levels(g.game);
    CHECK(again.from_file);
    CHECK(again.notes.empty());
    REQUIRE(again.levels.size() == 2);
    CHECK(again.levels[0].id == "level");
    CHECK(again.levels[0].name == "Уровень 1");
    CHECK(again.levels[1].id == id);
    CHECK(again.levels[1].name == "Пещера");
    CHECK(again.start == id);
    CHECK(g.list_text() == "{\n  \"start_level\": \"" + id + "\",\n  \"levels\": [\n    {\n      \"id\": \"level\",\n      \"name\": \"Уровень 1\"\n    },\n    {\n      \"id\": \"" + id + "\",\n      \"name\": \"Пещера\"\n    }\n  ]\n}\n");
    std::string start;
    CHECK(start_level_folder(g.game, &start) == g.game / "levels" / id);
    CHECK(start == id);
    // Which level a folder is, however it is written.
    CHECK(level_of_folder(g.game, again, g.game / "levels" / id)->id == id);
    CHECK(level_of_folder(g.game, again, g.game / "levels" / ".." / "level")->id == "level");
    CHECK(level_of_folder(g.game, again, g.root / "elsewhere") == nullptr);
    // The old level's files: as they were.
    CHECK(g.text(g.game / "level" / "areas.json") == areas_before);
    CHECK(std::distance(fs::directory_iterator(g.game / "level"), fs::directory_iterator()) == 1);
}

TEST_CASE("levels: names, where case does not count, and ids that are not taken") {
    Game g;
    LevelList list = read_levels(g.game);
    std::string id, error;
    REQUIRE(add_level(g.game, list, "Пещера", id, &error));
    CHECK(level_name_problem(list, "пещера").find("уже есть") != std::string::npos);
    CHECK(level_name_problem(list, "ПЕЩЕРА").find("уже есть") != std::string::npos);
    CHECK(level_name_problem(list, "Пещера", id).empty()); // its own name
    CHECK(level_name_problem(list, "   ").find("пустое") != std::string::npos);
    CHECK(level_name_problem(list, std::string(61, 'a')).find("длиннее") != std::string::npos);
    CHECK(level_name_problem(list, std::string(60, 'a')).empty());
    CHECK(level_name_problem(list, "Ю\tг").find("управляющий") != std::string::npos);
    CHECK_FALSE(add_level(g.game, list, "уровень 1", id, &error));
    CHECK(error.find("уже есть") != std::string::npos);
    CHECK(list.levels.size() == 2);
    CHECK_FALSE(rename_level(g.game, list, "level", "ПЕЩЕРА", &error));
    CHECK(list.levels[0].name == "Уровень 1");
    CHECK_FALSE(rename_level(g.game, list, "lnothere", "Другой", &error));
    CHECK_FALSE(set_start_level(g.game, list, "lnothere", &error));
    CHECK(list.start == "level");

    // A new id is no level's and has no folder; one with a folder left behind is not given.
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string n = new_level_id(g.game, list);
        CHECK(valid_level_id(n));
        CHECK_FALSE(list.find(n));
        seen.insert(n);
    }
    CHECK(seen.size() > 190);
    CHECK(valid_level_id("level"));
    CHECK(valid_level_id("l3f9a0c21"));
    CHECK(valid_level_id("cave_2-b"));
    CHECK_FALSE(valid_level_id(""));
    CHECK_FALSE(valid_level_id("Cave"));
    CHECK_FALSE(valid_level_id("../x"));
    CHECK_FALSE(valid_level_id("a/b"));
    CHECK_FALSE(valid_level_id("a\\b"));
    CHECK_FALSE(valid_level_id("c:"));
    CHECK_FALSE(valid_level_id("пещера"));
    CHECK_FALSE(valid_level_id(std::string(41, 'a')));
}

TEST_CASE("levels: a list that cannot be used is told, the game starts with «level», the file is not written over") {
    Game g;
    const char* bad[] = {"{ не JSON", "[1, 2]", "{\"levels\": 5}", "{\"levels\": []}", "{\"levels\": [{\"name\": \"Без id\"}]}"};
    for (const char* text : bad) {
        CAPTURE(text);
        Game::write_text(g.game / kLevelsFile, text);
        LevelList list = read_levels(g.game);
        CHECK(list.broken);
        CHECK(list.problem.find("levels.json") != std::string::npos);
        REQUIRE(list.levels.size() == 1);
        CHECK(list.start_level().id == "level");
        std::vector<std::string> notes;
        CHECK(start_level_folder(g.game, nullptr, &notes) == g.game / "level");
        CHECK(has(notes, "начинается с уровня «level»"));
        std::string id, error;
        CHECK_FALSE(add_level(g.game, list, "Уровень 2", id, &error));
        CHECK(error.find("исправьте") != std::string::npos);
        CHECK_FALSE(rename_level(g.game, list, "level", "Другое", &error));
        CHECK_FALSE(set_start_level(g.game, list, "level", &error));
        CHECK(g.list_text() == text);
        std::error_code ec;
        CHECK_FALSE(fs::exists(g.game / "levels", ec));
    }
    // A folder where the file is: not readable, told.
    std::error_code ec;
    fs::remove(g.game / kLevelsFile, ec);
    fs::create_directories(g.game / kLevelsFile, ec);
    CHECK(read_levels(g.game).broken);
}

TEST_CASE("levels: entries that cannot be used are left out and told; a start not in the list is the first; the file is "
          "not written over") {
    Game g;
    const std::string text = "{\"start_level\": \"lgone\", \"levels\": ["
                             "{\"id\": \"Пещера\", \"name\": \"Плохой id\"},"
                             "{\"id\": \"cave\", \"name\": \"Пещера\"},"
                             "{\"id\": \"../x\", \"name\": \"Наружу\"},"
                             "{\"id\": \"cave\", \"name\": \"Ещё раз\"},"
                             "\"не объект\","
                             "{\"id\": \"level\"}],"
                             "\"later\": {\"key\": 1}}";
    Game::write_text(g.game / kLevelsFile, text);
    LevelList list = read_levels(g.game);
    CHECK_FALSE(list.broken);
    CHECK(list.locked);
    CHECK_FALSE(list.writable());
    REQUIRE(list.levels.size() == 2);
    CHECK(list.levels[0].id == "cave");
    CHECK(list.levels[0].name == "Пещера");
    CHECK(list.levels[1].id == "level");
    CHECK(list.levels[1].name == "level");
    CHECK(list.start == "cave");
    CHECK(has(list.notes, "«Пещера» — не буквы"));
    CHECK(has(list.notes, "«../x» — не буквы"));
    CHECK(has(list.notes, "«cave» уже есть"));
    CHECK(has(list.notes, "нет id"));
    CHECK(has(list.notes, "нет имени"));
    CHECK(has(list.notes, "стартового уровня «lgone» нет"));
    CHECK(has(list.notes, "поле «later» не прочитано"));
    CHECK(list.problem.find("levels.json: прочитано не всё") == 0);
    CHECK(list.problem.find("«later»") != std::string::npos);
    CHECK(list.problem.find("«lgone»") != std::string::npos);
    CHECK(list.problem.find("нет имени") == std::string::npos); // put right, nothing lost
    CHECK(level_folder(g.game, "cave") == g.game / "levels" / "cave");
    std::string id;
    std::vector<std::string> notes;
    CHECK(start_level_folder(g.game, &id, &notes) == g.game / "levels" / "cave");
    CHECK(id == "cave");
    CHECK(has(notes, "«lgone»"));
    // Nothing of the list changes, and the file stays as the author wrote it.
    const LevelList kept = list;
    std::string error, made;
    CHECK_FALSE(add_level(g.game, list, "Уровень 3", made, &error));
    CHECK(error.find("файл не переписывается") != std::string::npos);
    CHECK(made.empty());
    CHECK_FALSE(rename_level(g.game, list, "cave", "Шахта", &error));
    CHECK_FALSE(set_start_level(g.game, list, "level", &error));
    CHECK_FALSE(write_levels(g.game, list, &error));
    CHECK(error.find("прочитано не всё") != std::string::npos);
    CHECK(list.levels.size() == kept.levels.size());
    CHECK(list.find("cave")->name == "Пещера");
    CHECK(list.start == "cave");
    CHECK(g.list_text() == text);
    std::error_code ec;
    CHECK_FALSE(fs::exists(g.game / "levels", ec));
}

TEST_CASE("levels: a list read only in part is not written over; one read whole and put right is") {
    // Codex's case: a level whose id this Forge cannot use. Renaming the start level must not lose «Cave».
    Game g;
    const std::string text = "{\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\"},"
                             "{\"id\":\"Cave\",\"name\":\"Recoverable cave\"}]}";
    Game::write_text(g.game / kLevelsFile, text);
    LevelList list = read_levels(g.game);
    CHECK(list.locked);
    REQUIRE(list.levels.size() == 1);
    CHECK(list.levels[0].name == "Start");
    CHECK(list.problem.find("«Cave»") != std::string::npos);
    const LevelList kept = list;
    std::string error, made;
    CHECK_FALSE(rename_level(g.game, list, "level", "Renamed start", &error));
    CHECK(error.find("«Cave»") != std::string::npos);
    CHECK(error.find("файл не переписывается") != std::string::npos);
    CHECK_FALSE(add_level(g.game, list, "Уровень 2", made, &error));
    CHECK_FALSE(set_start_level(g.game, list, "level", &error));
    CHECK(list.levels.size() == 1);
    CHECK(list.levels[0].name == kept.levels[0].name);
    CHECK(list.start == kept.start);
    CHECK(list.locked);
    CHECK(g.list_text() == text);
    std::error_code ec;
    CHECK_FALSE(fs::exists(g.game / "levels", ec));

    // Each thing writing would lose locks the list: a field this Forge does not know (of the list, of a level), a
    // field written twice, a name that is no text, a start that is no id or no level of the list.
    const char* in_part[] = {
        "{\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\"}],\"transitions\":[]}",
        "{\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\",\"music\":\"a.ogg\"}]}",
        "{\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\",\"name\":\"Again\"}]}",
        "{\"start_level\":\"level\",\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\"}]}",
        "{\"start_level\":\"level\",\"levels\":[{\"id\":\"level\",\"name\":5}]}",
        "{\"start_level\":7,\"levels\":[{\"id\":\"level\",\"name\":\"Start\"}]}",
        "{\"start_level\":\"lgone\",\"levels\":[{\"id\":\"level\",\"name\":\"Start\"}]}",
    };
    const char* said[] = {"«transitions»", "«music»", "«name» записано не раз", "«start_level» записано не раз", "не строка",
                          "«start_level» — не id", "«lgone»"};
    for (usize k = 0; k < std::size(in_part); ++k) {
        CAPTURE(in_part[k]);
        Game::write_text(g.game / kLevelsFile, in_part[k]);
        LevelList l = read_levels(g.game);
        CHECK_FALSE(l.broken);
        CHECK(l.locked);
        CHECK(l.problem.find(said[k]) != std::string::npos);
        CHECK(l.start == "level");
        CHECK_FALSE(rename_level(g.game, l, "level", "Другое", &error));
        CHECK(g.list_text() == in_part[k]);
    }

    // Put right and nothing lost: a level with no name (named by its id), no start_level (the first), a name with
    // spaces around. Read whole: a change is written, and the file has it all.
    Game::write_text(g.game / kLevelsFile, "{\"levels\":[{\"id\":\"cave\",\"name\":\"  Пещера \"},{\"id\":\"level\"}]}");
    LevelList whole = read_levels(g.game);
    CHECK_FALSE(whole.locked);
    CHECK(whole.writable());
    CHECK(whole.problem.empty());
    CHECK(has(whole.notes, "нет имени"));
    CHECK(has(whole.notes, "стартовый уровень не указан"));
    CHECK(whole.start == "cave");
    REQUIRE(rename_level(g.game, whole, "level", "Деревня", &error));
    const LevelList again = read_levels(g.game);
    CHECK(again.notes.empty());
    REQUIRE(again.levels.size() == 2);
    CHECK(again.levels[0].name == "Пещера");
    CHECK(again.levels[1].name == "Деревня");
    CHECK(again.start == "cave");
}

TEST_CASE("levels: a change that cannot be written leaves the list, the file and the folders as they were") {
    Game g;
    LevelList list = read_levels(g.game);
    std::string first, error;
    REQUIRE(add_level(g.game, list, "Пещера", first, &error));
    const std::string before = g.list_text();
    const LevelList kept = list;
    // levels.json cannot be written over: a folder (not empty) stands where it goes.
    std::error_code ec;
    fs::remove(g.game / kLevelsFile, ec);
    fs::create_directories(g.game / kLevelsFile / "занято", ec);
    std::string id;
    CHECK_FALSE(add_level(g.game, list, "Уровень 3", id, &error));
    CHECK(error.find("не записался") != std::string::npos);
    CHECK(id.empty());
    CHECK(list.levels.size() == kept.levels.size());
    // Nothing of the new level is left: only the first one's folder.
    std::vector<std::string> folders;
    for (const auto& e : fs::directory_iterator(g.game / "levels")) folders.push_back(path_to_utf8(e.path().filename()));
    CHECK(folders == std::vector<std::string>{first});
    CHECK_FALSE(rename_level(g.game, list, first, "Шахта", &error));
    CHECK(list.find(first)->name == "Пещера");
    CHECK_FALSE(set_start_level(g.game, list, first, &error));
    CHECK(list.start == "level");
    fs::remove_all(g.game / kLevelsFile, ec);
    Game::write_text(g.game / kLevelsFile, before);
    CHECK(read_levels(g.game).levels.size() == 2);

    // The first new level of a game: its levels/ folder goes too.
    Game h("forge_tests_уровни другой игры");
    LevelList fresh = read_levels(h.game);
    fs::create_directories(h.game / kLevelsFile / "занято", ec);
    fresh = LevelList{};
    fresh.levels.push_back({"level", "Уровень 1"});
    fresh.start = "level";
    CHECK_FALSE(add_level(h.game, fresh, "Уровень 2", id, &error));
    CHECK_FALSE(fs::exists(h.game / "levels", ec));
}
