// The author's game as a folder of its own (step 14.1): its description, the catalog of templates, a new game
// made of one and a game found by its folder or its description.

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/reflect.h"
#include "forge/editor/project.h"
#include "forge/editor/ui_design.h"
#include "forge/assets/image.h"
#include "forge/game/shell.h"
#include "forge/level/levels.h"
#include "forge/level/own_tiles.h"

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

// A module of two kinds ("thing" with a picture, "noisy" with a sound) and a verb, and a template of it with an
// object of each, a picture, a sound, a conversation and «Ресурсы» of its own.
struct Fixture {
    fs::path root = fresh("forge_tests_проект");
    fs::path module_dir = root / utf8_path("модуль");
    fs::path catalog = root / utf8_path("каталог") / "templates.json";
    std::vector<Module> modules{{"test", "Проба", module_dir}};
    Template t;

    Fixture() {
        const std::string kinds = R"({"kinds": [
  {"id": "thing", "name": "Вещь", "props": []},
  {"id": "noisy", "name": "Шумное", "components": {"ProjTestBell": {}},
   "props": [{"id": "sound", "name": "Звук", "bind": "ProjTestBell.sound", "asset": "sound"}]}
]})";
        const std::string verbs = R"({"verbs": [{"id": "open", "name": "открывает", "plural": "открывают", "case": "acc", "do": "open"}]})";
        put(module_dir / "kinds.json", kinds);
        put(module_dir / "verbs.json", verbs);
        put(module_dir / "ideas.json", R"({"ideas": []})");
        const fs::path game = root / utf8_path("каталог") / utf8_path("Проба игры") / "game";
        put(game / "game.json", "{\n  \"title\": \"Проба\",\n  \"org\": \"Forge\",\n  \"autosave_minutes\": 5\n}\n");
        put(game / "kinds.json", kinds);
        put(game / "verbs.json", verbs);
        put(game / "ideas.json", R"({"ideas": []})");
        put(game / "objects" / utf8_path("Сундук.object.json"),
            R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": "сундук.png"})");
        put(game / "objects" / utf8_path("Колокол.object.json"),
            R"({"id": "bell", "name": "Колокол", "kind": "noisy", "values": {"sound": "звон.wav"}})");
        put(game / "pictures" / utf8_path("сундук.png"), "png");
        put(game / "sounds" / utf8_path("звон.wav"), "wav");
        put(game / "dialogues" / utf8_path("кузнец.json"), R"({"id": "кузнец", "nodes": [{"id": "привет", "text": "Здравствуй"}]})");
        put(game / "level" / "world.json", R"({"around": "empty"})");
        put(root / utf8_path("каталог") / utf8_path("Проба игры") / "assets" / utf8_path("эскиз сундука.png"), "psd");
        put(root / utf8_path("каталог") / utf8_path("проба.png"), "card");
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
    CHECK(target(root / utf8_path("нет такой"), "Игра").problem.find("нет") != std::string::npos);
    put(root / utf8_path("файл"), "x");
    CHECK(target(root / utf8_path("файл"), "Игра").problem.find("не папка") != std::string::npos);
    fs::create_directories(root / utf8_path("Пустая"));
    CHECK(target(root, "Пустая").problem.empty());
    put(root / utf8_path("Занятая") / utf8_path("чужое.txt"), "чужое");
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
    REQUIRE_MESSAGE(create(f.t, f.modules, f.parent(), "Игра А", a, &error), error);
    CHECK(a == f.parent() / utf8_path("Игра А"));
    REQUIRE(create(f.t, f.modules, f.parent(), "Игра Б", b, &error));
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
    CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра А", again, &error));
    CHECK(error.find("уже есть, и в ней есть файлы") != std::string::npos);
    CHECK(again.empty());
    // An empty folder of that name makes way.
    fs::create_directories(f.parent() / utf8_path("Игра В"));
    CHECK(create(f.t, f.modules, f.parent(), "Игра В", again, &error));
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
    t.game = f.root / utf8_path("нет");
    CHECK(problems(t).find("нет папки игры шаблона") != std::string::npos);
    t = f.t;
    t.picture = f.root / utf8_path("нет.png");
    CHECK(problems(t).find("нет картинки шаблона") != std::string::npos);
    t = f.t;
    t.assets = f.root / utf8_path("нет");
    CHECK(problems(t).find("нет папки «Ресурсов» шаблона") != std::string::npos);

    fs::rename(game / "pictures" / utf8_path("сундук.png"), f.root / utf8_path("сундук.png"));
    CHECK(problems(f.t).find("объект «Сундук»: нет картинки pictures/сундук.png") != std::string::npos);
    fs::rename(f.root / utf8_path("сундук.png"), game / "pictures" / utf8_path("сундук.png"));
    fs::rename(game / "sounds" / utf8_path("звон.wav"), f.root / utf8_path("звон.wav"));
    CHECK_MESSAGE(problems(f.t).find("объект «Колокол»: нет звука sounds/звон.wav") != std::string::npos, problems(f.t));
    fs::rename(f.root / utf8_path("звон.wav"), game / "sounds" / utf8_path("звон.wav"));
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
    CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра", made, &error));
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
    CHECK_FALSE(read_catalog(f.root / utf8_path("нет.json"), list, &error));
}

TEST_CASE("project: a template whose object names a file of another place makes no game") {
    Fixture f;
    const fs::path game = f.t.game;
    auto problems = [&] {
        std::string all;
        for (const std::string& p : template_problems(f.t, f.modules)) all += p + "; ";
        return all;
    };
    const auto before = tree(f.parent());
    auto refused = [&](const std::string& expected) {
        const std::string all = problems();
        CHECK_MESSAGE(all.find(expected) != std::string::npos, all);
        fs::path made;
        std::string error;
        CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра", made, &error));
        CHECK_MESSAGE(error.find("из шаблона «Проба» игру не создать: ") == 0, error);
        CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
        CHECK(made.empty());
        CHECK(tree(f.parent()) == before);
    };
    // A picture there is, by its full path: the game would need a file outside it.
    const fs::path outside = f.root / utf8_path("чужая папка") / utf8_path("чужой сундук.png");
    put(outside, "png");
    const fs::path chest = game / "objects" / utf8_path("Сундук.object.json");
    const std::string full = path_to_utf8(outside);
    std::string json = R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": ")";
    for (char c : full) json += c == '\\' ? std::string("\\\\") : std::string(1, c);
    put(chest, json + "\"}");
    refused("объект «Сундук»: картинка «" + full + "» вне папки игры (нужен файл в pictures/)");
    // Out of pictures/ by "..", by a Windows separator or drive.
    put(game / utf8_path("сундук.png"), "png");
    for (const char* name : {"../сундук.png", "../../чужая папка/чужой сундук.png", "..\\\\сундук.png", "C:сундук.png"}) {
        CAPTURE(name);
        put(chest, std::string(R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": ")") + name + "\"}");
        refused("объект «Сундук»: картинка «");
        CHECK(problems().find("вне папки игры") != std::string::npos);
    }
    fs::remove(game / utf8_path("сундук.png"));
    // A sound by its full path.
    const fs::path ring = f.root / utf8_path("чужая папка") / utf8_path("звон.wav");
    put(ring, "wav");
    const fs::path bell = game / "objects" / utf8_path("Колокол.object.json");
    put(chest, R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": "сундук.png"})");
    json = R"({"id": "bell", "name": "Колокол", "kind": "noisy", "values": {"sound": ")";
    for (char c : path_to_utf8(ring)) json += c == '\\' ? std::string("\\\\") : std::string(1, c);
    put(bell, json + "\"}}");
    refused("объект «Колокол»: звук «" + path_to_utf8(ring) + "» вне папки игры (нужен файл в sounds/)");
    put(bell, R"({"id": "bell", "name": "Колокол", "kind": "noisy", "values": {"sound": "../../звон.wav"}})");
    refused("объект «Колокол»: звук «../../звон.wav» вне папки игры");

    // Letters and spaces of any kind in the game's own files are fine, and the copy has them.
    put(game / "pictures" / utf8_path("старый сундук (1).png"), "png");
    put(game / "sounds" / utf8_path("звон колокола.wav"), "wav");
    put(chest, R"({"id": "chest", "name": "Сундук", "kind": "thing", "picture": "старый сундук (1).png"})");
    put(bell, R"({"id": "bell", "name": "Колокол", "kind": "noisy", "values": {"sound": "звон колокола.wav"}})");
    CHECK_MESSAGE(problems().empty(), problems());
    fs::path made;
    std::string error;
    REQUIRE_MESSAGE(create(f.t, f.modules, f.parent(), "Игра с пробелом", made, &error), error);
    CHECK(text_of(made / "game" / "pictures" / utf8_path("старый сундук (1).png")) == "png");
    CHECK(text_of(made / "game" / "sounds" / utf8_path("звон колокола.wav")) == "wav");
    CHECK(game_problems(made / "game", &f.modules[0]).empty());
}

TEST_CASE("project: a template's conversations, quests, links and screens are checked as the game reads them") {
    Fixture f;
    const fs::path game = f.t.game;
    auto problems = [&] {
        std::string all;
        for (const std::string& p : template_problems(f.t, f.modules)) all += p + "; ";
        return all;
    };
    auto has = [&](const std::string& expected) {
        const std::string all = problems();
        CHECK_MESSAGE(all.find(expected) != std::string::npos, all);
        fs::path made;
        std::string error;
        CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра", made, &error));
        CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
        CHECK(fs::is_empty(f.parent()));
    };
    REQUIRE_MESSAGE(problems().empty(), problems());

    // A conversation the game cannot load: not JSON, no nodes.
    const fs::path talk = game / "dialogues" / utf8_path("кузнец.json");
    const std::string good_talk = text_of(talk);
    put(talk, "{ сломан");
    has("разговор dialogues/кузнец.json: ");
    put(talk, R"({"id": "кузнец", "lines": []})");
    has("разговор dialogues/кузнец.json: ");
    put(talk, good_talk);

    // Quests that cannot be read.
    put(game / "quests.json", "{ сломан");
    has("задания quests.json: ");
    put(game / "quests.json", R"([{"id": "сундук", "title": "Сундук", "var": "quest.chest", "stages": [{"at": 1, "name": "ищет", "text": "Найти сундук."}]}])");
    CHECK_MESSAGE(problems().empty(), problems());

    // Links: the verb must be the module's.
    put(game / "logic.json", R"({"links": [{"id": 1, "a": "bell", "verb": "open", "b": "chest"}]})");
    CHECK_MESSAGE(problems().empty(), problems());
    put(game / "logic.json", R"({"links": [{"id": 7, "a": "bell", "verb": "летает", "b": "chest"}]})");
    has("связь №7: глагола «летает» нет в модуле");
    put(game / "logic.json", "{ сломан");
    has("связи logic.json не читаются");
    fs::remove(game / "logic.json");

    // A screen: its page, its pictures and sounds are the game's own.
    editor::design::Screen screen;
    screen.title = "Меню";
    editor::design::Paint picture;
    picture.kind = editor::design::PaintKind::Image;
    picture.image = "pictures/фон меню.png";
    screen.root.fills.push_back(picture);
    screen.music = "музыка.ogg";
    const fs::path ui = game / "ui";
    put(ui / utf8_path("меню.json"), editor::design::save_screen(screen));
    has("экран «меню»: нет страницы ui/меню.html, её показывает игра");
    put(ui / utf8_path("меню.html"), "<rml></rml>");
    has("экран «меню»: картинка: нет файла pictures/фон меню.png");
    put(game / "pictures" / utf8_path("фон меню.png"), "png");
    has("экран «меню»: звук: нет файла sounds/музыка.ogg");
    put(game / "sounds" / utf8_path("музыка.ogg"), "ogg");
    CHECK_MESSAGE(problems().empty(), problems());
    screen.root.fills[0].image = "../../проба.png";
    put(ui / utf8_path("меню.json"), editor::design::save_screen(screen));
    has("экран «меню»: картинка «../../проба.png» вне папки игры");
    screen.root.fills[0].image = "pictures/фон меню.png";
    screen.music = path_to_utf8(game / "sounds" / utf8_path("музыка.ogg"));
    put(ui / utf8_path("меню.json"), editor::design::save_screen(screen));
    has("экран «меню»: звук «" + screen.music + "» вне папки игры (нужен файл в sounds/)");
    put(ui / utf8_path("меню.json"), "{ сломан");
    has("экран «меню»: ui/меню.json не читается");
}

TEST_CASE("project: the template is read again when the game is made, and the copy is checked before it gets its name") {
    Fixture f;
    REQUIRE(template_problems(f.t, f.modules).empty());
    // The window checked the template; then its picture went away.
    const fs::path picture = f.t.game / "pictures" / utf8_path("сундук.png");
    fs::rename(picture, f.root / utf8_path("сундук.png"));
    const auto template_before = tree(f.catalog.parent_path());
    fs::path made;
    std::string error;
    CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра А", made, &error));
    CHECK(error == "из шаблона «Проба» игру не создать: объект «Сундук»: нет картинки pictures/сундук.png");
    CHECK(made.empty());
    CHECK(fs::is_empty(f.parent()));
    fs::rename(f.root / utf8_path("сундук.png"), picture);

    // A file that is not in the copy (gone while the template was copied): the copy never becomes the game.
    Hooks hooks;
    hooks.write = [&](const fs::path& file) {
        if (file.filename() == "project.forge") fs::remove(file.parent_path() / "game" / "pictures" / utf8_path("сундук.png"));
        return true;
    };
    CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра А", made, &error, hooks));
    CHECK(error == "в копии шаблона не всё, что нужно игре: объект «Сундук»: нет картинки pictures/сундук.png");
    CHECK(made.empty());
    CHECK(fs::is_empty(f.parent()));
    CHECK(tree(f.catalog.parent_path()) != template_before); // the picture is back
    CHECK(fs::exists(picture));
    REQUIRE_MESSAGE(create(f.t, f.modules, f.parent(), "Игра А", made, &error), error);
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
        CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра А", made, &error, hooks));
        CHECK(error.find("не записался файл ") == 0);
        CHECK(made.empty());
        CHECK(tree(f.parent()) == before);
    }
    // Someone puts a file into the game's folder just before the copy becomes it: theirs stays, nothing of ours.
    Hooks hooks;
    hooks.before_rename = [&](const fs::path& folder) { put(folder / utf8_path("чужое.txt"), "чужое"); };
    fs::path made;
    std::string error;
    CHECK_FALSE(create(f.t, f.modules, f.parent(), "Игра А", made, &error, hooks));
    CHECK(error.find("занята") != std::string::npos);
    auto after = tree(f.parent());
    CHECK(after.size() == 2);
    CHECK(after.at("Игра А/чужое.txt") == "чужое");
    // An empty folder made meanwhile makes way.
    hooks.before_rename = [&](const fs::path& folder) { fs::create_directories(folder); };
    CHECK(create(f.t, f.modules, f.parent(), "Игра Б", made, &error, hooks));
    CHECK(fs::exists(made / "game" / "game.json"));
    CHECK(tree(f.catalog.parent_path()) == template_before);
}

TEST_CASE("project: a game is found by its folder or its description; a folder from before opens as it did") {
    Fixture f;
    fs::path a;
    std::string error;
    REQUIRE(create(f.t, f.modules, f.parent(), "Игра А", a, &error));
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
    const fs::path old = f.root / utf8_path("старый проект");
    put(old / "game" / "game.json", R"({"title": "Старая шахта"})");
    REQUIRE(find(old, f.modules, g, &error) == Found::Game);
    CHECK_FALSE(g.described);
    CHECK(g.description.module == "slice");
    CHECK(g.title == "Старая шахта");

    CHECK(find(f.root / utf8_path("нет"), f.modules, g, &error) == Found::NoGame);
    fs::create_directories(f.root / utf8_path("пусто"));
    CHECK(find(f.root / utf8_path("пусто"), f.modules, g, &error) == Found::NoGame);
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
    REQUIRE(create(f.t, f.modules, f.parent(), "Игра А", a, &error));
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
    REQUIRE_MESSAGE(create(*mine, modules, parent, "Моя шахта", made, &error), error);
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

// The template «Платформер» (step 14.2d): games/platformer as the catalog's second card makes it.
TEST_CASE("project: the catalog's «Платформер» makes a game with its «Ресурсы» and the module's files") {
    const fs::path games = utf8_path(FORGE_SOURCE_DIR) / "games";
    std::vector<Template> list;
    std::string error;
    REQUIRE(read_catalog(games / "templates.json", list, &error));
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == "old-mine"); // the cards in this order: «Новая игра» shows «Платформер» second
    const Template& t = list[1];
    CHECK(t.id == "platformer");
    CHECK(t.name == "Платформер");
    CHECK(t.module == "slice");
    CHECK(t.game == games / "platformer");
    CHECK(t.assets == games / "templates" / "platformer" / "assets");
    CHECK(t.picture == games / "templates" / "platformer.png");
    CHECK(t.bad.empty());
    assets::CookedTexture card;
    std::vector<u8> bytes;
    REQUIRE(read_file(t.picture, bytes));
    REQUIRE(assets::decode_image(bytes, card));
    CHECK(card.width == 480);
    CHECK(card.height == 270);
    const std::vector<Module> modules{{"slice", "Старая шахта", games / "slice"}};
    std::string all;
    for (const std::string& p : template_problems(t, modules)) all += p + "; ";
    CHECK_MESSAGE(all.empty(), all);
    const fs::path parent = fresh("forge_tests_проект_платформер");
    fs::path made;
    REQUIRE_MESSAGE(create(t, modules, parent, "Мой платформер", made, &error), error);
    Game g;
    REQUIRE(find(made, modules, g, &error) == Found::Game);
    CHECK(g.title == "Мой платформер");
    CHECK(g.description.from == "platformer");
    // Its «Ресурсы»: the template's pictures with their .meta, byte for byte.
    for (const char* name : {"герой.png", "жук.png", "еж.png", "монета.png", "шипы.png", "ряд_шипов.png", "флаг.png", "указатель.png",
                             "сердце.png"})
        for (const std::string& file : {std::string(name), std::string(name) + ".meta"})
            CHECK_MESSAGE(text_of(made / "assets" / utf8_path("картинки") / utf8_path(file)) ==
                              text_of(t.assets / utf8_path("картинки") / utf8_path(file)),
                          file);
    // The module's files are the module's: nothing to refresh.
    for (const char* file : {"kinds.json", "verbs.json", "ideas.json"})
        CHECK_MESSAGE(text_of(t.game / file) == text_of(games / "slice" / file), file);
    std::vector<std::string> refreshed;
    CHECK(refresh_module_files(g.game, modules[0], refreshed, &error));
    CHECK(refreshed.empty());
    // Its view: 32 pixels a tile, as the pictures are drawn.
    CHECK(game::read_game_info(t.game).zoom == 32);
    CHECK(game::read_game_info(games / "slice").zoom == 0); // a game without it: the game's own
}

TEST_CASE("project: the levels of «Платформер» have the tiles of its tiles.png, solid ones opaque on top") {
    const fs::path games = utf8_path(FORGE_SOURCE_DIR) / "games";
    const fs::path game = games / "platformer";
    const level::LevelList levels = level::read_levels(game);
    REQUIRE(levels.from_file);
    REQUIRE(levels.writable());
    REQUIRE(levels.levels.size() == 3);
    CHECK(levels.start == "level");
    std::string error;
    // The template's tiles.png (the brief's contract): 16 pictures of 32 px in a row, ids 256..271.
    assets::CookedTexture atlas;
    std::vector<u8> bytes;
    REQUIRE(read_file(games / "templates" / "platformer" / "tiles.png", bytes));
    REQUIRE(assets::decode_image(bytes, atlas));
    REQUIRE(atlas.width == 512);
    REQUIRE(atlas.height == 32);
    for (const level::LevelEntry& e : levels.levels) {
        CAPTURE(e.id);
        const fs::path folder = level::level_folder(game, e.id);
        level::LevelTiles tiles;
        bool found = false;
        REQUIRE_MESSAGE(level::load_tiles(folder, tiles, 3, 2, &found, &error), error);
        REQUIRE(found);
        CHECK(tiles.px == 32);
        REQUIRE(tiles.tiles.size() == 16);
        for (usize i = 0; i < tiles.tiles.size(); ++i) {
            const level::OwnTile& o = tiles.tiles[i];
            CAPTURE(o.name);
            CHECK(o.id == level::kFirstOwnTile + i);
            CHECK(o.solid == (i < 10)); // 256..265: ground, bridge and stone
            CHECK(o.layer == (o.solid ? 1u : 0u)); // solid on «Блоки», the rest on «Стены»
            const u8* p = tiles.picture(i);
            bool same = true;
            for (u32 y = 0; y < 32; ++y)
                same = same && std::equal(p + y * 32 * 4, p + (y + 1) * 32 * 4, &atlas.rgba8[(static_cast<usize>(y) * 512 + i * 32) * 4]);
            CHECK_MESSAGE(same, "its picture is cell ", i, " of the template's tiles.png");
            if (!o.solid) continue;
            bool top = true;
            for (u32 x = 0; x < 32; ++x) top = top && p[x * 4 + 3] == 255;
            CHECK_MESSAGE(top, "the top row of a solid tile is opaque: the hero stands on what is seen");
        }
        level::LevelWorld world;
        REQUIRE(level::load_world(folder, world, &found, &error));
        CHECK(world.empty_around);
    }
}
