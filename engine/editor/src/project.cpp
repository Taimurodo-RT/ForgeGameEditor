#include "forge/editor/project.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/editor/ui_design.h"
#include "forge/game/dialogue.h"
#include "forge/game/quests.h"
#include "forge/logic/logic.h"
#include "forge/objects/library.h"

#include <yyjson.h>

#include <algorithm>
#include <span>

namespace forge::editor::project {

namespace fs = std::filesystem;

namespace {

std::string str(yyjson_val* o, const char* key) {
    const char* s = yyjson_get_str(yyjson_obj_get(o, key));
    return s ? s : "";
}

bool read_text(const fs::path& file, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool write_text(const fs::path& file, std::string_view text) {
    return write_file_atomic(file, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
}

// A path of the catalog: from its folder, never out of it.
bool inside(std::string_view text, const fs::path& base, fs::path& out) {
    const fs::path p = utf8_path(text);
    if (text.empty() || p.has_root_name() || p.has_root_directory()) return false;
    for (const fs::path& part : p)
        if (part == "..") return false;
    out = base / p;
    return true;
}

// A file a game's data names, from one of its folders: no full path, no "..", nothing a Windows path would
// read as another place (\\, :). Spaces and Cyrillic are fine.
bool inside_name(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos) return false;
    fs::path unused;
    return inside(name, {}, unused);
}

std::string shown(const fs::path& p) { return path_to_utf8(p); }

// Problems in one line: the first three, and how many more.
std::string listed(const std::vector<std::string>& problems) {
    std::string out;
    for (usize i = 0; i < problems.size() && i < 3; ++i) out += (i ? "; " : "") + problems[i];
    if (problems.size() > 3) out += "; и ещё " + std::to_string(problems.size() - 3);
    return out;
}

// A file name with no letters of its own but a dot ("." and ".."), or a
// Windows device's ("con", "COM1.txt"): the base before the first dot.
bool device_name(std::string_view name) {
    std::string base(name.substr(0, name.find('.')));
    for (char& c : base)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    static constexpr std::string_view kDevices[] = {"con", "prn", "aux", "nul"};
    if (std::find(std::begin(kDevices), std::end(kDevices), base) != std::end(kDevices)) return true;
    if (base.size() < 4 || !(base.starts_with("com") || base.starts_with("lpt"))) return false;
    const std::string_view n = std::string_view(base).substr(3);
    return (n.size() == 1 && n[0] >= '0' && n[0] <= '9') || n == "\xC2\xB9" || n == "\xC2\xB2" || n == "\xC2\xB3"; // COM¹
}

bool empty_folder(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::is_empty(dir, ec) && !ec;
}

// The copy of a folder, file by file, so a failing one is named. hooks.write
// may fail a file (tests).
bool copy_tree(const fs::path& from, const fs::path& to, const fs::path& shown_as, const Hooks& hooks, std::string& error) {
    std::error_code ec;
    if (!fs::create_directories(to, ec) && ec) {
        error = "не создалась папка " + shown(shown_as);
        return false;
    }
    fs::recursive_directory_iterator it(from, ec), end;
    if (ec) {
        error = "не читается папка шаблона " + shown(from);
        return false;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "не читается папка шаблона " + shown(from);
            return false;
        }
        const fs::path rel = it->path().lexically_relative(from);
        const fs::path dst = to / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(dst, ec);
            if (ec) {
                error = "не создалась папка " + shown(shown_as / rel);
                FORGE_WARN("Новая игра: %s: %s", shown(dst).c_str(), ec.message().c_str());
                return false;
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const bool allowed = !hooks.write || hooks.write(dst);
        if (allowed) fs::copy_file(it->path(), dst, fs::copy_options::none, ec);
        if (!allowed || ec) {
            error = "не записался файл " + shown(shown_as / rel);
            if (ec) FORGE_WARN("Новая игра: %s: %s", shown(dst).c_str(), ec.message().c_str());
            return false;
        }
    }
    return true;
}

// A template with no «Ресурсы»: the game gets an empty folder for its own.
bool make_folder(const fs::path& dir, const fs::path& shown_as, std::string& error) {
    std::error_code ec;
    if (fs::create_directories(dir, ec) || !ec) return true;
    error = "не создалась папка " + shown(shown_as);
    return false;
}

bool write_file(const fs::path& file, std::string_view text, const fs::path& shown_as, const Hooks& hooks, std::string& error) {
    if ((hooks.write && !hooks.write(file)) || !write_text(file, text)) {
        error = "не записался файл " + shown(shown_as);
        return false;
    }
    return true;
}

// game.json with another title, the rest as it was.
bool retitle(const std::string& json, std::string_view title, std::string& out) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_mut_doc* mut = yyjson_doc_mut_copy(doc, nullptr);
    yyjson_doc_free(doc);
    yyjson_mut_val* root = yyjson_mut_doc_get_root(mut);
    yyjson_mut_val* value = yyjson_mut_strncpy(mut, title.data(), title.size());
    if (yyjson_mut_obj_get(root, "title")) yyjson_mut_obj_replace(root, yyjson_mut_str(mut, "title"), value);
    else yyjson_mut_obj_insert(root, yyjson_mut_str(mut, "title"), value, 0);
    usize len = 0;
    char* text = yyjson_mut_write(mut, YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, &len);
    yyjson_mut_doc_free(mut);
    if (!text) return false;
    out.assign(text, len);
    free(text);
    return true;
}

} // namespace

bool read_description(std::string_view json, Description& out, std::string* error) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    yyjson_val* format = root ? yyjson_obj_get(root, "forge_project") : nullptr;
    if (!yyjson_is_obj(root) || !yyjson_is_int(format)) {
        yyjson_doc_free(doc);
        if (error) *error = "это не описание игры Forge";
        return false;
    }
    out = {};
    out.format = static_cast<int>(yyjson_get_int(format));
    out.module = str(root, "module");
    out.from = str(root, "template");
    yyjson_doc_free(doc);
    if (out.format < 1 || out.format > kFormat) {
        if (error) *error = "игра сделана более новой версией Forge (формат описания " + std::to_string(out.format) + ")";
        return false;
    }
    if (out.module.empty()) {
        if (error) *error = "в описании игры не назван модуль";
        return false;
    }
    return true;
}

std::string description_json(const Description& d) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "forge_project", d.format);
    yyjson_mut_obj_add_strncpy(doc, root, "module", d.module.data(), d.module.size());
    if (!d.from.empty()) yyjson_mut_obj_add_strncpy(doc, root, "template", d.from.data(), d.from.size());
    usize len = 0;
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, &len);
    yyjson_mut_doc_free(doc);
    std::string out = text ? std::string(text, len) : std::string();
    free(text);
    return out;
}

const Module* find_module(const std::vector<Module>& modules, std::string_view id) {
    for (const Module& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

bool read_catalog(const fs::path& file, std::vector<Template>& out, std::string* error) {
    out.clear();
    std::string text;
    if (!read_text(file, text)) {
        if (error) *error = "нет каталога шаблонов " + shown(file);
        return false;
    }
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    yyjson_val* list = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "templates") : nullptr;
    if (!yyjson_is_arr(list)) {
        yyjson_doc_free(doc);
        if (error) *error = "каталог шаблонов " + shown(file) + " не читается";
        return false;
    }
    const fs::path base = file.parent_path();
    yyjson_arr_iter it = yyjson_arr_iter_with(list);
    for (yyjson_val* v; (v = yyjson_arr_iter_next(&it));) {
        Template t;
        t.id = str(v, "id");
        t.name = str(v, "name");
        t.about = str(v, "about");
        t.module = str(v, "module");
        if (t.id.empty() || t.name.empty()) continue;
        // A path out of the catalog's folder is not taken: the template cannot be used then.
        const std::string game = str(v, "game"), assets = str(v, "assets"), picture = str(v, "picture");
        for (const std::string* p : {&game, &assets, &picture})
            if (!p->empty() && t.bad.empty() && !inside(*p, base, p == &game ? t.game : p == &assets ? t.assets : t.picture))
                t.bad = "путь «" + *p + "» в каталоге ведёт из его папки";
        out.push_back(std::move(t));
    }
    yyjson_doc_free(doc);
    return true;
}

std::vector<std::string> template_problems(const Template& t, const std::vector<Module>& modules) {
    std::vector<std::string> out;
    std::error_code ec;
    const Module* module = find_module(modules, t.module);
    if (!t.bad.empty()) out.push_back(t.bad);
    if (!module) out.push_back("нужен модуль «" + t.module + "», его нет в этой сборке Forge");
    if (t.game.empty() || !fs::is_directory(t.game, ec)) {
        out.push_back("нет папки игры шаблона" + (t.game.empty() ? std::string() : " " + shown(t.game)));
        return out;
    }
    std::string json;
    if (!read_text(t.game / "game.json", json)) out.push_back("в шаблоне нет game.json");
    else if (yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0)) yyjson_doc_free(doc);
    else out.push_back("game.json шаблона не читается");
    if (!t.picture.empty() && !fs::is_regular_file(t.picture, ec)) out.push_back("нет картинки шаблона");
    if (!t.assets.empty() && !fs::is_directory(t.assets, ec)) out.push_back("нет папки «Ресурсов» шаблона");
    for (std::string& p : game_problems(t.game, module)) out.push_back(std::move(p));
    return out;
}

std::vector<std::string> game_problems(const fs::path& game, const Module* module) {
    std::vector<std::string> out;
    std::error_code ec;
    // A file a game's data names, as the game finds it: inside the game's folder, or the game depends on a file
    // of another place, and a copy of the game is not a game of its own.
    auto named = [&](const std::string& what, const std::string& name, const fs::path& folder, const std::string& shown_folder) {
        if (!inside_name(name)) {
            out.push_back(what + " «" + name + "» вне папки игры (нужен файл в " +
                          (shown_folder.empty() ? std::string("папке игры, например pictures/") : shown_folder) + ")");
            return;
        }
        if (!fs::is_regular_file(folder / utf8_path(name), ec)) out.push_back(what + ": нет файла " + shown_folder + name);
    };

    // Objects as the module sees them: kinds, pictures, sounds.
    const fs::path objects = game / "objects";
    if (fs::is_directory(objects, ec)) {
        const fs::path kinds = module && fs::is_regular_file(module->files / "kinds.json", ec) ? module->files / "kinds.json"
                                                                                               : game / "kinds.json";
        objects::Library lib;
        lib.set_pictures_folder(game / "pictures");
        lib.set_sounds_folder(game / "sounds");
        std::string error;
        if (!lib.load(kinds, objects, &error)) {
            out.push_back("виды объектов не читаются: " + error);
        } else {
            for (fs::directory_iterator it(objects, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = path_to_utf8(it->path().filename());
                if (!it->is_regular_file(ec) || !name.ends_with(".object.json")) continue;
                const bool read = std::any_of(lib.templates().begin(), lib.templates().end(),
                                              [&](const objects::Template& o) { return o.file == it->path(); });
                if (!read) out.push_back("файл объекта objects/" + name + " не читается");
            }
            for (const objects::Template& o : lib.templates()) {
                if (module && !lib.kind(o.kind)) out.push_back("объект «" + o.name + "»: вида «" + o.kind + "» нет в модуле");
                if (!o.picture.empty()) {
                    if (!inside_name(o.picture))
                        out.push_back("объект «" + o.name + "»: картинка «" + o.picture + "» вне папки игры (нужен файл в pictures/)");
                    else if (!fs::is_regular_file(lib.picture_file(o), ec))
                        out.push_back("объект «" + o.name + "»: нет картинки pictures/" + o.picture);
                }
                for (const objects::PropDef* p : lib.props_of(o)) {
                    const std::string* value = p->asset == "sound" ? o.value(p->id) : nullptr;
                    if (!value) continue;
                    yyjson_doc* doc = yyjson_read(value->data(), value->size(), 0);
                    const char* sound = doc ? yyjson_get_str(yyjson_doc_get_root(doc)) : nullptr;
                    const std::string name = sound ? sound : "";
                    yyjson_doc_free(doc);
                    if (name.empty()) continue;
                    if (!inside_name(name))
                        out.push_back("объект «" + o.name + "»: звук «" + name + "» вне папки игры (нужен файл в sounds/)");
                    else if (!fs::is_regular_file(lib.sound_file(name), ec))
                        out.push_back("объект «" + o.name + "»: нет звука sounds/" + name);
                }
            }
        }
    }

    // Conversations and quests, as the game reads them.
    std::string text;
    std::vector<fs::path> talks;
    for (fs::directory_iterator it(game / "dialogues", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json") talks.push_back(it->path());
    std::sort(talks.begin(), talks.end());
    for (const fs::path& file : talks) {
        const std::string shown_as = "разговор dialogues/" + path_to_utf8(file.filename());
        if (!read_text(file, text)) {
            out.push_back(shown_as + " не читается");
            continue;
        }
        forge::game::Dialogue talk;
        forge::game::DialogueReport report;
        talk.load(text, report);
        for (const std::string& e : report.errors) out.push_back(shown_as + ": " + e);
    }
    if (fs::exists(game / "quests.json", ec)) {
        std::vector<std::string> errors;
        forge::game::QuestBook quests;
        if (!read_text(game / "quests.json", text)) out.push_back("задания quests.json не читаются");
        else quests.load(text, errors);
        for (const std::string& e : errors) out.push_back("задания quests.json: " + e);
    }

    // Links («Логика») and the verbs they use: the module's (a game gets them on opening), else its own.
    if (fs::exists(game / "logic.json", ec)) {
        logic::Logic links;
        logic::Verbs verbs;
        const fs::path verbs_file = module && fs::is_regular_file(module->files / "verbs.json", ec) ? module->files / "verbs.json"
                                                                                                   : game / "verbs.json";
        std::string error;
        if (!links.load(game / "logic.json", &error)) out.push_back("связи logic.json не читаются: " + error);
        else if (!verbs.load(verbs_file, &error)) out.push_back("глаголы verbs.json не читаются: " + error);
        else
            for (const logic::Link& l : links.links)
                if (!verbs.find(l.verb)) out.push_back("связь №" + std::to_string(l.id) + ": глагола «" + l.verb + "» нет в модуле");
    }

    // Screens («Интерфейс»): the game shows each one's page; its pictures and sounds are the game's.
    std::vector<fs::path> screens;
    for (fs::directory_iterator it(game / "ui", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json") screens.push_back(it->path());
    std::sort(screens.begin(), screens.end());
    for (const fs::path& file : screens) {
        const std::string name = path_to_utf8(file.stem());
        const std::string shown_as = "экран «" + name + "»";
        design::Screen screen;
        std::string error;
        if (!read_text(file, text) || !design::load_screen(text, screen, &error)) {
            out.push_back(shown_as + ": ui/" + path_to_utf8(file.filename()) + " не читается" + (error.empty() ? "" : ": " + error));
            continue;
        }
        if (!screen.library && !fs::is_regular_file(design::screen_file(game / "ui", name, ".html"), ec))
            out.push_back(shown_as + ": нет страницы ui/" + name + ".html, её показывает игра");
        std::vector<std::string> pictures, sounds;
        auto walk = [&](auto&& self, const design::Node& n) -> void {
            for (const design::Paint& f : n.fills)
                if (!f.image.empty()) pictures.push_back(f.image);
            if (!n.frame.image.empty()) pictures.push_back(n.frame.image);
            if (!n.mask.image.empty()) pictures.push_back(n.mask.image);
            if (!n.click_sound.empty() && n.click_sound != "none") sounds.push_back(n.click_sound);
            for (const design::Node& c : n.children) self(self, c);
        };
        walk(walk, screen.root);
        for (const std::string* s : {&screen.music, &screen.button_sound})
            if (!s->empty() && *s != "none") sounds.push_back(*s);
        std::sort(pictures.begin(), pictures.end());
        pictures.erase(std::unique(pictures.begin(), pictures.end()), pictures.end());
        std::sort(sounds.begin(), sounds.end());
        sounds.erase(std::unique(sounds.begin(), sounds.end()), sounds.end());
        for (const std::string& p : pictures) named(shown_as + ": картинка", p, game, "");
        for (const std::string& s : sounds) named(shown_as + ": звук", s, game / "sounds", "sounds/");
    }
    return out;
}

std::string folder_name(std::string_view title) {
    std::string out;
    bool space = false;
    for (char ch : title) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (c < 0x20 || c == 0x7F || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) continue;
        if (space && !out.empty()) out += ' ';
        space = false;
        out += ch;
    }
    auto trim = [&] {
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    };
    trim();
    if (out.size() > 100) {
        usize cut = 100;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut; // not in the middle of a letter
        out.resize(cut);
        trim();
    }
    // A device's name, «nul.игра» too: "_1" after the part before the first dot.
    if (!out.empty() && device_name(out)) out.insert(std::min(out.find('.'), out.size()), "_1");
    return out;
}

Target target(const fs::path& parent, std::string_view title) {
    Target t;
    std::error_code ec;
    const std::string name = folder_name(title);
    if (title.find_first_not_of(" \t") == std::string_view::npos) t.problem = "введите название игры";
    else if (name.empty()) t.problem = "в названии нет ни одного знака, который годится для имени папки";
    else if (parent.empty()) t.problem = "выберите, где создать игру";
    else if (!parent.is_absolute()) t.problem = "нужен полный путь к папке, где создать игру";
    else if (!fs::exists(parent, ec)) t.problem = "папки " + shown(parent) + " нет";
    else if (!fs::is_directory(parent, ec)) t.problem = shown(parent) + " — это не папка";
    if (!name.empty() && !parent.empty()) t.folder = parent / utf8_path(name);
    if (!t.problem.empty()) return t;
    const fs::file_status st = fs::status(t.folder, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) t.problem = "папку " + shown(t.folder) + " не удаётся прочитать";
    else if (!fs::exists(st)) {}
    else if (!fs::is_directory(st)) t.problem = "там уже есть файл «" + name + "»";
    else if (!empty_folder(t.folder)) t.problem = "папка «" + name + "» уже есть, и в ней есть файлы";
    return t;
}

bool create(const Template& t, const std::vector<Module>& modules, const fs::path& parent, std::string_view title, fs::path& made,
            std::string* error, const Hooks& hooks) {
    std::error_code ec;
    made.clear();
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    const Target to = target(parent, title);
    if (!to.problem.empty()) return fail(to.problem);
    // The template as it is now, not as the window saw it: its files may have changed since.
    const std::vector<std::string> problems = template_problems(t, modules);
    if (!problems.empty()) return fail("из шаблона «" + t.name + "» игру не создать: " + listed(problems));
    std::string game_json;
    if (!read_text(t.game / "game.json", game_json)) return fail("game.json шаблона не читается");
    // The side folder: one no one has.
    const std::string name = path_to_utf8(to.folder.filename());
    fs::path side;
    for (int n = 1; n < 1000 && side.empty(); ++n) {
        const fs::path p = parent / utf8_path("." + name + ".создаётся-" + std::to_string(n));
        if (!fs::exists(p, ec) && !ec && fs::create_directory(p, ec) && !ec) side = p;
    }
    if (side.empty()) return fail("не создаётся папка в " + shown(parent));
    std::string why, titled;
    if (!retitle(game_json, title, titled)) why = "game.json шаблона не читается";
    const bool copied = why.empty() && copy_tree(t.game, side / "game", "game", hooks, why) &&
                        (t.assets.empty() ? make_folder(side / "assets", "assets", why)
                                          : copy_tree(t.assets, side / "assets", "assets", hooks, why)) &&
                        write_file(side / utf8_path(kDescription), description_json({kFormat, t.module, t.id}),
                                   utf8_path(kDescription), hooks, why) &&
                        write_file(side / "game" / "game.json", titled, "game/game.json", hooks, why);
    // The copy itself, before it gets its name: what the game needs is in it (a file that went away while it
    // was copied is not).
    if (copied) {
        const std::vector<std::string> left = game_problems(side / "game", find_module(modules, t.module));
        if (!left.empty()) why = "в копии шаблона не всё, что нужно игре: " + listed(left);
    }
    if (copied && why.empty() && hooks.before_rename) hooks.before_rename(to.folder);
    if (copied && why.empty()) {
        // An empty folder makes way (remove takes only an empty one); one with anything in it is never written over.
        const bool there = fs::exists(to.folder, ec);
        if (there && (!empty_folder(to.folder) || !fs::remove(to.folder, ec) || ec))
            why = "папка «" + name + "» занята: в ней появились файлы";
        else {
            fs::rename(side, to.folder, ec);
            if (ec) {
                why = "папка " + shown(side) + " не переименовалась в «" + name + "»";
                FORGE_WARN("Новая игра: %s", ec.message().c_str());
            }
        }
        if (why.empty()) {
            made = to.folder;
            return true;
        }
    }
    fs::remove_all(side, ec);
    return fail(why);
}

Found find(const fs::path& path, const std::vector<Module>& modules, Game& out, std::string* error) {
    std::error_code ec;
    out = {};
    auto broken = [&](std::string why) {
        if (error) *error = std::move(why);
        return Found::Broken;
    };
    fs::path root = path;
    if (fs::is_regular_file(path, ec)) {
        if (path.filename() != utf8_path(kDescription))
            return broken(shown(path) + " — не описание игры Forge (нужна папка игры или её project.forge)");
        root = path.parent_path();
    }
    if (!fs::is_directory(root, ec)) {
        if (error) *error = "папки " + shown(root) + " нет";
        return Found::NoGame;
    }
    out.root = root;
    out.game = root / "game";
    const fs::path file = root / utf8_path(kDescription);
    if (fs::exists(file, ec)) {
        std::string text, why;
        if (!read_text(file, text)) return broken(shown(file) + " не читается");
        if (!read_description(text, out.description, &why)) return broken(shown(file) + ": " + why);
        if (!find_module(modules, out.description.module))
            return broken("игре нужен модуль «" + out.description.module + "», его нет в этой сборке Forge");
        if (!fs::is_regular_file(out.game / "game.json", ec)) return broken("в игре нет game/game.json");
        out.described = true;
    } else if (fs::is_regular_file(out.game / "game.json", ec)) {
        out.description.module = "slice"; // every game before step 14 is one of «Старая шахта»
    } else {
        if (error) *error = "в папке " + shown(root) + " нет игры Forge (нет project.forge)";
        return Found::NoGame;
    }
    out.title = game_title(out.game);
    return Found::Game;
}

bool refresh_module_files(const fs::path& game, const Module& module, std::vector<std::string>& refreshed, std::string* note) {
    refreshed.clear();
    std::error_code ec;
    bool all = true;
    for (const char* name : kModuleFiles) {
        const fs::path from = module.files / name, mine = game / name;
        std::vector<u8> a, b;
        if (!read_file(from, a)) {
            all = false;
            continue;
        }
        if (read_file(mine, b) && a == b) continue;
        if (write_file_atomic(mine, a)) refreshed.push_back(name);
        else if (note) *note = "не обновился " + std::string(name) + " из модуля";
    }
    if (!all && note && note->empty())
        *note = "модуля «" + module.name + "» нет на месте (" + shown(module.files) + "): у игры остаются свои копии его файлов";
    return all;
}

std::string game_title(const fs::path& game_dir) {
    std::string text;
    if (!read_text(game_dir / "game.json", text)) return {};
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    const std::string title = doc ? str(yyjson_doc_get_root(doc), "title") : std::string();
    yyjson_doc_free(doc);
    return title;
}

} // namespace forge::editor::project
