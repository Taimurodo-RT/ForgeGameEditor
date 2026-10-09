// The author's game as a folder of its own (step 14.1): its description, the catalog of templates, a new game
// made of one and a game found by its folder or its description.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/reflect.h"
#include "forge/editor/project.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

// What a property of the test module's kinds is bound to (a sound's name).
struct ProjTestBell {
    std::string sound;
};
FORGE_REFLECT_DECLARE(ProjTestBell)
FORGE_REFLECT(ProjTestBell, 1) { t.field("sound", &ProjTestBell::sound); }

using namespace forge;
using namespace forge::editor::project;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& file, std::string_view text) {
    REQUIRE(write_file_atomic(file, std::span(reinterpret_cast<const u8*>(text.data()), text.size())));
}
std::string text_of(const fs::path& file) {
    std::vector<u8> bytes;
    return read_file(file, bytes) ? std::string(bytes.begin(), bytes.end()) : std::string("<нет>");
}
// Every file under a folder and its bytes, by the path from the folder.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out[path_to_utf8(it->path().lexically_relative(dir)) + (it->is_directory() ? "/" : "")] =
            it->is_directory() ? std::string() : text_of(it->path());
    return out;
}
fs::path fresh(const char* name) {
    const fs::path dir = fs::temp_directory_path() / utf8_path(name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

// A module of two kinds ("thing" with a picture, "noisy" with a sound) and a template of it with an object of each,
// a picture, a sound, a conversation and «Ресурсы» of its own.
struct Fixture {
    fs::path root = fresh("forge_tests_проект");
    fs::path module_dir = root / "модуль";
    fs::path catalog = root / "каталог" / "templates.json";
    std::vector<Module> modules{{"test", "Проба", module_dir}};
    Template t;

    Fixture() {
        const std::string kinds = R"({"kinds": [
  {"id": "thing", "name": "Вещь", "props": []},
  {"id": "noisy", "name": "Шумное", "components": {"ProjTestBell": {}},
   "props": [{"id": "sound", "name": "Звук", "bind": "ProjTestBell.sound", "asset": "sound"}]}
]})";
        put(module_dir / "kinds.json", kinds);
        put(module_dir / "verbs.json", R"({"verbs": []})");
        put(module_dir / "ideas.json", R"({"ideas": []})");
        const fs::path game = root / "каталог" / utf8_path("Проба игры") / "game";
        put(game / "game.json", "{\n  \"title\": \"Проба\",\n  \"org\": \"Forge\",\n  \"autosave_minutes\": 5\n}\n");
        put(game / "kinds.json", kinds);
        put(game / "verbs.json", R"({"verbs": []})");
        put(game / "ideas.json", R"({"ideas": []})");
        put(game / "objects" / utf8_path("Сундук.object.json"),
            R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": "сундук.png"})");
        put(game / "objects" / utf8_path("Колокол.object.json"),
            R"({"id": "bell", "name": "Колокол", "kind": "noisy", "values": {"sound": "звон.wav"}})");
        put(game / "pictures" / utf8_path("сундук.png"), "png");
        put(game / "sounds" / utf8_path("звон.wav"), "wav");
        put(game / "dialogues" / utf8_path("кузнец.json"), R"({"lines": []})");
        put(game / "level" / "world.json", R"({"around": "empty"})");
        put(root / "каталог" / utf8_path("Проба игры") / "assets" / utf8_path("эскиз сундука.png"), "psd");
        put(root / "каталог" / "проба.png", "card");
        put(catalog, R"({"templates": [
  {"id": "probe", "name": "Проба", "about": "для тестов", "module": "test",
   "game": "Проба игры/game", "assets": "Проба игры/assets", "picture": "проба.png"}
]})");
        std::vector<Template> list;
        std::string error;
        REQUIRE(read_catalog(catalog, list, &error));
        REQUIRE(list.size() == 1);
        t = list[0];
        fs::create_directories(root / utf8_path("Мои игры"));
    }
    fs::path parent() const { return root / utf8_path("Мои игры"); }
};

} // namespace

TEST_CASE("project: a description is written and read back, a newer or a broken one is not opened") {
    Description d;
    d.module = "slice";
    d.from = "old-mine";
    const std::string json = description_json(d);
    CHECK(json.find("\"forge_project\": 1") != std::string::npos);
    Description back;
    std::string error;
    REQUIRE(read_description(json, back, &error));
    CHECK(back.format == kFormat);
    CHECK(back.module == "slice");
    CHECK(back.from == "old-mine");
    CHECK_FALSE(read_description(R"({"forge_project": 2, "module": "slice"})", back, &error));
    CHECK(error.find("более новой версией") != std::string::npos);
    CHECK_FALSE(read_description("not json", back, &error));
    CHECK_FALSE(read_description(R"({"module": "slice"})", back, &error));
    CHECK_FALSE(read_description(R"({"forge_project": 1})", back, &error));
    CHECK(error.find("модуль") != std::string::npos);
}

TEST_CASE("project: a game's folder keeps the letters and spaces of its title") {
    CHECK(folder_name("Игра А") == "Игра А");
    CHECK(folder_name("  Шахта:  часть 2. ") == "Шахта часть 2");
    CHECK(folder_name("a/../b\\c") == "a..bc");
    CHECK(folder_name("Что? <Да>|\"нет\"*") == "Что Данет");
    CHECK(folder_name("x\ty\n") == "x y");
    CHECK(folder_name("...") == "");
    CHECK(folder_name("  ") == "");
    CHECK(folder_name("con") == "con_1");
    CHECK(folder_name("COM1") == "COM1_1");
    CHECK(folder_name("Nul.игра") == "Nul_1.игра");
    CHECK(folder_name("aux") == "aux_1");
    CHECK(folder_name("Конь") == "Конь");
    std::string long_title;
    for (int i = 0; i < 80; ++i) long_title += "ж";
    const std::string cut = folder_name(long_title);
    CHECK(cut.size() == 100);
    CHECK(cut == long_title.substr(0, 100)); // 50 whole letters
}

TEST_CASE("project: where a new game goes and why it cannot") {
    const fs::path root = fresh("forge_tests_проект_куда");
    CHECK(target(root, "Игра А").problem.empty());
    CHECK(target(root, "Игра А").folder == root / utf8_path("Игра А"));
    CHECK(target(root, "  ").problem == "введите название игры");
    CHECK(target(root, "///").problem.find("ни одного знака") != std::string::npos);
    CHECK(target({}, "Игра").problem == "выберите, где создать игру");
    CHECK(target(utf8_path("Мои игры"), "Игра").problem.find("полный путь") != std::string::npos);
    CHECK(target(root / "нет такой", "Игра").problem.find("нет") != std::string::npos);
    put(root / "файл", "x");
    CHECK(target(root / "файл", "Игра").problem.find("не папка") != std::string::npos);
    fs::create_directories(root / utf8_path("Пустая"));
    CHECK(target(root, "Пустая").problem.empty());
    put(root / utf8_path("Занятая") / "чужое.txt", "чужое");
    CHECK(target(root, "Занятая").problem.find("уже есть, и в ней есть файлы") != std::string::npos);
    CHECK(target(root, "файл").problem.find("уже есть файл") != std::string::npos);
}

TEST_CASE("project: a game made of a template is a copy of its own; the template and another game stay as they are") {
    Fixture f;
    CHECK(template_problems(f.t, f.modules).empty());
    CHECK(f.t.name == "Проба");
    CHECK(f.t.module == "test");
    const auto template_before = tree(f.catalog.parent_path());

    fs::path a, b;
    std::string error;
    REQUIRE_MESSAGE(create(f.t, f.parent(), "Игра А", a, &error), error);
    CHECK(a == f.parent() / utf8_path("Игра А"));
    REQUIRE(create(f.t, f.parent(), "Игра Б", b, &error));
    // Only the two games in the folder: no side folder left.
    std::vector<std::string> there;
    for (const auto& e : fs::directory_iterator(f.parent())) there.push_back(path_to_utf8(e.path().filename()));
    std::sort(there.begin(), there.end());
    CHECK(there == std::vector<std::string>{"Игра А", "Игра Б"});

    // The template's game/ and assets/ byte for byte; game.json with the new title and the rest as it was.
    const auto source = tree(f.catalog.parent_path() / utf8_path("Проба игры"));
    auto made = tree(a);
    CHECK(made.at("project.forge") == description_json({kFormat, "test", "probe"}));
    CHECK(made.at("game/game.json") == "{\n  \"title\": \"Игра А\",\n  \"org\": \"Forge\",\n  \"autosave_minutes\": 5\n}\n");
    made.erase("project.forge");
    made.erase("game/game.json");
    auto copied = source;
    copied.erase("game/game.json");
    CHECK(made == copied);
    CHECK(game_title(a / "game") == "Игра А");

    // A changed in every way; B and the template keep every byte.
    const auto b_before = tree(b);
    put(a / "game" / "objects" / utf8_path("Сундук.object.json"), R"({"id": "chest", "name": "Сундук А", "kind": "thing"})");
    put(a / "game" / "pictures" / utf8_path("новая.png"), "new");
    fs::remove(a / "game" / "sounds" / utf8_path("звон.wav"));
    CHECK(tree(b) == b_before);
    CHECK(tree(f.catalog.parent_path()) == template_before);

    // The same name again: the folder has files now.
    fs::path again;
    CHECK_FALSE(create(f.t, f.parent(), "Игра А", again, &error));
    CHECK(error.find("уже есть, и в ней есть файлы") != std::string::npos);
    CHECK(again.empty());
    // An empty folder of that name makes way.
    fs::create_directories(f.parent() / utf8_path("Игра В"));
    CHECK(create(f.t, f.parent(), "Игра В", again, &error));
    CHECK(fs::exists(again / "project.forge"));
}

TEST_CASE("project: what keeps a template from making a game") {
    Fixture f;
    const fs::path game = f.t.game;
    auto problems = [&](const Template& t) {
        std::string all;
        for (const std::string& p : template_problems(t, f.modules)) all += p + "; ";
        return all;
    };
    Template t = f.t;
    t.module = "platformer";
    CHECK(problems(t).find("нужен модуль «platformer»") != std::string::npos);
    t = f.t;
    t.game = f.root / "нет";
    CHECK(problems(t).find("нет папки игры шаблона") != std::string::npos);
    t = f.t;
    t.picture = f.root / "нет.png";
    CHECK(problems(t).find("нет картинки шаблона") != std::string::npos);
    t = f.t;
    t.assets = f.root / "нет";
    CHECK(problems(t).find("нет папки «Ресурсов» шаблона") != std::string::npos);

    fs::rename(game / "pictures" / utf8_path("сундук.png"), f.root / "сундук.png");
    CHECK(problems(f.t).find("объект «Сундук»: нет картинки pictures/сундук.png") != std::string::npos);
    fs::rename(f.root / "сундук.png", game / "pictures" / utf8_path("сундук.png"));
    fs::rename(game / "sounds" / utf8_path("звон.wav"), f.root / "звон.wav");
    CHECK_MESSAGE(problems(f.t).find("объект «Колокол»: нет звука sounds/звон.wav") != std::string::npos, problems(f.t));
    fs::rename(f.root / "звон.wav", game / "sounds" / utf8_path("звон.wav"));
    put(game / "objects" / utf8_path("Дракон.object.json"), R"({"id": "dragon", "name": "Дракон", "kind": "dragon"})");
    CHECK(problems(f.t).find("объект «Дракон»: вида «dragon» нет в модуле") != std::string::npos);
    put(game / "objects" / utf8_path("Дракон.object.json"), "{ сломан");
    CHECK(problems(f.t).find("файл объекта objects/Дракон.object.json не читается") != std::string::npos);
    fs::remove(game / "objects" / utf8_path("Дракон.object.json"));
    CHECK_MESSAGE(problems(f.t).empty(), problems(f.t));
    put(game / "game.json", "{ сломан");
    CHECK(problems(f.t).find("game.json шаблона не читается") != std::string::npos);
    fs::path made;
    std::string error;
    CHECK_FALSE(create(f.t, f.parent(), "Игра", made, &error));
    CHECK(fs::is_empty(f.parent()));
    fs::remove(game / "game.json");
    CHECK(problems(f.t).find("в шаблоне нет game.json") != std::string::npos);

    // A path of the catalog out of its folder.
    put(f.catalog, R"({"templates": [{"id": "x", "name": "Икс", "module": "test", "game": "../../etc"},
                                    {"id": "y", "name": "Игрек", "module": "test", "game": "Проба игры/game", "picture": "/etc/passwd"},
                                    {"name": "без id"}]})");
    std::vector<Template> list;
    REQUIRE(read_catalog(f.catalog, list, &error));
    REQUIRE(list.size() == 2);
    CHECK(problems(list[0]).find("путь «../../etc» в каталоге ведёт из его папки") != std::string::npos);
    CHECK(problems(list[1]).find("путь «/etc/passwd» в каталоге ведёт из его папки") != std::string::npos);
    CHECK_FALSE(read_catalog(f.root / "нет.json", list, &error));
}

TEST_CASE("project: a copy that fails half way, or a folder taken while it is made, leaves nothing") {
    Fixture f;
    const auto before = tree(f.parent());
    const auto template_before = tree(f.catalog.parent_path());
    // Fails at each file in turn: the template's files, project.forge, game.json.
    usize files = 2;
    for (const fs::path& dir : {f.t.game, f.t.assets})
        for (const auto& [name, bytes] : tree(dir)) files += name.ends_with("/") ? 0 : 1;
    for (usize n = 1; n <= files; ++n) {
        usize written = 0;
        Hooks hooks;
        hooks.write = [&](const fs::path&) { return ++written != n; };
        fs::path made;
        std::string error;
        CHECK_FALSE(create(f.t, f.parent(), "Игра А", made, &error, hooks));
        CHECK(error.find("не записался файл ") == 0);
        CHECK(made.empty());
        CHECK(tree(f.parent()) == before);
    }
    // Someone puts a file into the game's folder just before the copy becomes it: theirs stays, nothing of ours.
    Hooks hooks;
    hooks.before_rename = [&](const fs::path& folder) { put(folder / "чужое.txt", "чужое"); };
    fs::path made;
    std::string error;
    CHECK_FALSE(create(f.t, f.parent(), "Игра А", made, &error, hooks));
    CHECK(error.find("занята") != std::string::npos);
    auto after = tree(f.parent());
    CHECK(after.size() == 2);
    CHECK(after.at("Игра А/чужое.txt") == "чужое");
    // An empty folder made meanwhile makes way.
    hooks.before_rename = [&](const fs::path& folder) { fs::create_directories(folder); };
    CHECK(create(f.t, f.parent(), "Игра Б", made, &error, hooks));
    CHECK(fs::exists(made / "game" / "game.json"));
    CHECK(tree(f.catalog.parent_path()) == template_before);
}

TEST_CASE("project: a game is found by its folder or its description; a folder from before opens as it did") {
    Fixture f;
    fs::path a;
    std::string error;
    REQUIRE(create(f.t, f.parent(), "Игра А", a, &error));
    Game g;
    REQUIRE(find(a, f.modules, g, &error) == Found::Game);
    CHECK(g.root == a);
    CHECK(g.game == a / "game");
    CHECK(g.described);
    CHECK(g.description.module == "test");
    CHECK(g.description.from == "probe");
    CHECK(g.title == "Игра А");
    REQUIRE(find(a / "project.forge", f.modules, g, &error) == Found::Game);
    CHECK(g.root == a);

    // From before step 14: game/game.json, no description.
    const fs::path old = f.root / "старый проект";
    put(old / "game" / "game.json", R"({"title": "Старая шахта"})");
    REQUIRE(find(old, f.modules, g, &error) == Found::Game);
    CHECK_FALSE(g.described);
    CHECK(g.description.module == "slice");
    CHECK(g.title == "Старая шахта");

    CHECK(find(f.root / "нет", f.modules, g, &error) == Found::NoGame);
    fs::create_directories(f.root / "пусто");
    CHECK(find(f.root / "пусто", f.modules, g, &error) == Found::NoGame);
    CHECK(error.find("нет игры Forge") != std::string::npos);
    CHECK(find(a / "game" / "game.json", f.modules, g, &error) == Found::Broken);
    CHECK(error.find("не описание игры Forge") != std::string::npos);
    CHECK(find(a, {}, g, &error) == Found::Broken);
    CHECK(error.find("нужен модуль «test»") != std::string::npos);
    put(a / "project.forge", R"({"forge_project": 7, "module": "test"})");
    CHECK(find(a, f.modules, g, &error) == Found::Broken);
    CHECK(error.find("более новой версией") != std::string::npos);
    put(a / "project.forge", "{");
    CHECK(find(a, f.modules, g, &error) == Found::Broken);
    put(a / "project.forge", description_json({kFormat, "test", "probe"}));
    fs::remove(a / "game" / "game.json");
    CHECK(find(a, f.modules, g, &error) == Found::Broken);
    CHECK(error == "в игре нет game/game.json");
}

TEST_CASE("project: opening refreshes only the module's files, and keeps the game's own when the module is away") {
    Fixture f;
    fs::path a;
    std::string error;
    REQUIRE(create(f.t, f.parent(), "Игра А", a, &error));
    put(f.module_dir / "verbs.json", R"({"verbs": ["новый"]})");
    put(a / "game" / "ideas.json", "мои");
    const auto before = tree(a);
    std::vector<std::string> refreshed;
    std::string note;
    CHECK(refresh_module_files(a / "game", f.modules[0], refreshed, &note));
    CHECK(refreshed == std::vector<std::string>{"verbs.json", "ideas.json"});
    CHECK(note.empty());
    CHECK(text_of(a / "game" / "verbs.json") == R"({"verbs": ["новый"]})");
    auto after = tree(a);
    after.erase("game/verbs.json");
    after.erase("game/ideas.json");
    auto kept = before;
    kept.erase("game/verbs.json");
    kept.erase("game/ideas.json");
    CHECK(after == kept);
    CHECK(refresh_module_files(a / "game", f.modules[0], refreshed, &note));
    CHECK(refreshed.empty());

    fs::remove_all(f.module_dir);
    const auto away = tree(a);
    CHECK_FALSE(refresh_module_files(a / "game", f.modules[0], refreshed, &note));
    CHECK(refreshed.empty());
    CHECK(note.find("модуля «Проба» нет на месте") != std::string::npos);
    CHECK(tree(a) == away);
}

TEST_CASE("project: the catalog's «Старая шахта» makes a game") {
    const fs::path games = utf8_path(FORGE_SOURCE_DIR) / "games";
    std::vector<Template> list;
    std::string error;
    REQUIRE(read_catalog(games / "templates.json", list, &error));
    const auto mine = std::find_if(list.begin(), list.end(), [](const Template& t) { return t.id == "old-mine"; });
    REQUIRE(mine != list.end());
    CHECK(mine->name == "Старая шахта");
    CHECK(mine->game == games / "slice");
    const std::vector<Module> modules{{"slice", "Старая шахта", games / "slice"}};
    std::string all;
    for (const std::string& p : template_problems(*mine, modules)) all += p + "; ";
    CHECK_MESSAGE(all.empty(), all);
    const fs::path parent = fresh("forge_tests_проект_шахта");
    fs::path made;
    REQUIRE_MESSAGE(create(*mine, parent, "Моя шахта", made, &error), error);
    Game g;
    REQUIRE(find(made, modules, g, &error) == Found::Game);
    CHECK(g.title == "Моя шахта");
    CHECK(g.description.from == "old-mine");
    CHECK(fs::exists(made / "game" / "logic.json"));
    CHECK(fs::exists(made / "game" / "dialogues"));
    // No «Ресурсы» in the template: the game gets an empty folder of its own for them.
    CHECK(fs::is_directory(made / "assets"));
    CHECK(fs::is_empty(made / "assets"));
    std::vector<std::string> refreshed;
    CHECK(refresh_module_files(g.game, modules[0], refreshed, &error));
    CHECK(refreshed.empty());
}
