#include "forge/game/screens.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/core/time.h"
#include "forge/ui/html.h"

#include <RmlUi/Core.h>
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace forge::game {

namespace fs = std::filesystem;

namespace {

ScreenRole role_of(const Rml::String& word) {
    if (word == "playing") return ScreenRole::Playing;
    if (word == "menu") return ScreenRole::Menu;
    return ScreenRole::Command;
}

// The variable names a text reads: {inv.coins} -> inv.coins ("{{" is a brace).
void text_variables(std::string_view text, std::vector<std::string>& out) {
    for (usize i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        const usize end = text.find('}', i);
        if (end == std::string_view::npos) return;
        out.emplace_back(text.substr(i + 1, end - i - 1));
        i = end;
    }
}

Expr parse_expr(const std::string& source, const std::string& page, const char* what) {
    std::string error;
    Expr e = Expr::parse(source, &error);
    if (!error.empty()) FORGE_WARN("экран %s: %s «%s» не читается: %s", page.c_str(), what, source.c_str(), error.c_str());
    return e;
}

} // namespace

std::vector<ScreenAction> parse_actions(std::string_view json) {
    std::vector<ScreenAction> out;
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (yyjson_is_arr(root)) {
        usize i, n;
        yyjson_val* v;
        yyjson_arr_foreach(root, i, n, v) {
            yyjson_val* what = yyjson_arr_get(v, 0);
            yyjson_val* target = yyjson_arr_get(v, 1);
            if (!yyjson_is_str(what)) continue;
            out.push_back({yyjson_get_str(what), yyjson_is_str(target) ? yyjson_get_str(target) : ""});
        }
    }
    if (doc) yyjson_doc_free(doc);
    return out;
}

// A layer the game's data changes.
struct Bound {
    Rml::Element* element = nullptr;
    std::string text;           // forge-text: the text with {variables}
    bool has_show = false;
    Expr show;                  // forge-show-if
    bool has_bar = false;
    Expr value, max;            // forge-bar-*
    std::string from = "left";
    bool has_picture = false;
    Expr picture;               // forge-picture: a path in the game folder
    // What was put on the element last, so unchanged values are not set again.
    std::string shown_text;
    int shown = -1;  // -1: not yet
    f64 fraction = -1;
    std::string shown_picture = "\x01"; // not yet
};

// One element of a list: what a cell shows of it.
struct Row {
    std::string id;
    f64 count = 0;
    const ScreenItem* item = nullptr; // items: what the game says about it
    std::string title, text;          // quests
    bool done = false;
    bool operator==(const Row& o) const {
        return id == o.id && count == o.count && item == o.item && title == o.title && text == o.text && done == o.done;
    }
};

// A list's elements, made again when the game's variables change.
struct Rows {
    u64 seen = ~0ull;
    const void* source_seen = nullptr;
    u64 changed = 1; // bumped when the elements differ from before
    std::vector<Row> rows;
};

// A layer repeating its cell for every element of a list (forge-list). Only
// the cells in sight are made: they are moved and given other elements as
// the list scrolls.
struct ListView {
    Rml::Element* box = nullptr;     // scrolls
    Rml::Element* content = nullptr; // as tall as every cell together; the cells sit in it
    Rml::ElementPtr cell;            // the cell everything is cloned from
    Rml::Element* template_element = nullptr; // the cell on the page, until make_lists() takes it out
    std::string source;
    f32 gap = 8, cx = 0, cy = 0, cw = 100, ch = 40;
    std::vector<Rml::Element*> empties; // shown while the list is empty
    struct Cell {
        Rml::Element* element = nullptr;
        std::vector<Bound> bound;
        Vars vars;      // item.* over the game's
        i64 row = -1; // the element it shows (-1: none)
        bool shown = false; // in sight (else display: none)
        u64 seen = ~0ull;
        f32 left = -1, top = -1;
    };
    std::vector<std::unique_ptr<Cell>> cells;
    int empty_shown = -1;
    f32 height = -1;
    u64 rows_seen = 0;
};

struct GameScreens::Page {
    std::string name;
    fs::path path;
    fs::file_time_type time{};
    Rml::ElementDocument* doc = nullptr;
    Rml::Element* root = nullptr;
    ScreenRole role = ScreenRole::Command;
    std::string fit = "expand";
    f32 width = 1920, height = 1080;
    std::string bars;
    bool pauses = false, esc = true;
    bool shown = false;  // a Command page that was shown
    bool visible = false; // on screen now
    u64 order = 0;        // when it was shown (Esc closes the newest)
    int fit_w = -1, fit_h = -1;
    std::string fit_transform; // the scale fit() gives the root
    f32 design_w = 1920, design_h = 1080; // the root's size in the page's pixels
    // How it comes and goes (forge-appear): the movement under way.
    std::string appear;
    f32 appear_time = 0.25f;
    // Its sound: the music while it is up, its buttons' sound.
    std::string music, button_sound;
    u64 move_start = 0;   // 0: none
    bool leaving = false; // going away: hidden when the movement ends
    std::vector<Bound> bound;
    std::vector<std::unique_ptr<ListView>> lists;
};

struct GameScreens::Impl : Rml::EventListener {
    GameScreens* self = nullptr;
    Rml::Context* context = nullptr;
    fs::path dir;
    bool watch = false;
    u64 next_poll = 0;
    u64 shows = 0;
    std::vector<std::unique_ptr<Page>> pages;
    std::string music; // what on_music was told last
    // The lists' data.
    std::vector<ScreenItem> items;
    std::unordered_map<std::string, usize> item_index;
    const QuestBook* quests = nullptr;
    Rows item_rows, quest_rows;
    f64 lists_ms = 0;

    void ProcessEvent(Rml::Event& event) override { self->click(event.GetTargetElement()); }

    Page* find(std::string_view name) {
        for (auto& p : pages)
            if (p->name == name) return p.get();
        return nullptr;
    }
    Page* of(Rml::ElementDocument* doc) {
        for (auto& p : pages)
            if (p->doc == doc) return p.get();
        return nullptr;
    }

    // A layer's own link to the data (forge-text, -show-if, -bar-*, -picture)
    // goes into out; false when it has none.
    bool bind_layer(const std::string& page, Rml::Element* e, std::vector<Bound>& out) {
        const auto attr = [&](const char* name) { return e->GetAttribute<Rml::String>(name, ""); };
        Bound b;
        b.element = e;
        bool any = false;
        if (e->HasAttribute("forge-text")) {
            b.text = attr("forge-text");
            any = true;
        }
        if (e->HasAttribute("forge-show-if")) {
            b.show = parse_expr(attr("forge-show-if"), page, "условие");
            b.has_show = true;
            any = true;
        }
        if (e->HasAttribute("forge-bar-value")) {
            b.value = parse_expr(attr("forge-bar-value"), page, "значение полоски");
            b.max = parse_expr(attr("forge-bar-max"), page, "наибольшее значение полоски");
            b.from = attr("forge-bar-from");
            b.has_bar = true;
            any = true;
        }
        if (e->HasAttribute("forge-picture")) {
            b.picture = parse_expr(attr("forge-picture"), page, "картинка");
            b.has_picture = true;
            any = true;
        }
        return any ? (out.push_back(std::move(b)), true) : false;
    }

    void scan(Page& p, Rml::Element* e) {
        const auto attr = [&](const char* name) { return e->GetAttribute<Rml::String>(name, ""); };
        if (e->HasAttribute("forge-screen") && !p.root) {
            p.root = e;
            p.role = role_of(attr("forge-screen"));
            p.fit = attr("forge-fit");
            std::sscanf(attr("forge-size").c_str(), "%f %f", &p.width, &p.height);
            p.width = std::max(p.width, 1.0f);
            p.height = std::max(p.height, 1.0f);
            p.bars = attr("forge-bars");
            p.pauses = attr("forge-pauses") == "1";
            p.esc = attr("forge-esc") != "0";
            p.appear = attr("forge-appear");
            if (p.appear == "none") p.appear.clear();
            p.appear_time = std::clamp(static_cast<f32>(std::atof(attr("forge-appear-time").c_str())), 0.0f, 10.0f);
            if (p.appear_time <= 0) p.appear_time = 0.25f;
            p.music = attr("forge-music");
            p.button_sound = attr("forge-button-sound");
        }
        bind_layer(p.name, e, p.bound);
        // The cell is the list's first element (a page written with line
        // breaks has text between them); the elements after it show when
        // the list is empty.
        Rml::Element* cell = nullptr;
        if (e->HasAttribute("forge-list"))
            for (int i = 0; i < e->GetNumChildren() && !cell; ++i)
                if (e->GetChild(i)->GetTagName() != "#text") cell = e->GetChild(i);
        if (cell) {
            auto list = std::make_unique<ListView>();
            list->box = e;
            list->source = attr("forge-list");
            list->gap = std::max(static_cast<f32>(std::atof(attr("forge-list-gap").c_str())), 0.0f);
            std::sscanf(attr("forge-cell").c_str(), "%f %f %f %f", &list->cx, &list->cy, &list->cw, &list->ch);
            list->cw = std::max(list->cw, 1.0f);
            list->ch = std::max(list->ch, 1.0f);
            list->template_element = cell;
            for (int i = 0; i < e->GetNumChildren(); ++i)
                if (e->GetChild(i) != cell && e->GetChild(i)->GetTagName() != "#text") list->empties.push_back(e->GetChild(i));
            p.lists.push_back(std::move(list));
        }
        // The cell is not on the page: its copies are.
        for (int i = 0; i < e->GetNumChildren(); ++i)
            if (e->GetChild(i) != cell) scan(p, e->GetChild(i));
    }

    // The cells come out of the page and an empty box takes their place;
    // update() fills it.
    void make_lists(Page& p) {
        for (auto& l : p.lists) {
            l->cell = l->box->RemoveChild(l->template_element);
            l->template_element = nullptr;
            Rml::ElementPtr content = p.doc->CreateElement("div");
            content->SetProperty("display", "block");
            content->SetProperty("position", "relative");
            content->SetProperty("margin", "0px");
            content->SetProperty("padding", "0px");
            content->SetProperty("width", "100%");
            content->SetProperty("height", "0px");
            content->SetProperty("flex", "none");
            content->SetProperty("pointer-events", "none");
            l->content = l->box->InsertBefore(std::move(content), l->box->GetFirstChild());
        }
    }

    // The elements of a list now.
    const Rows& rows(const std::string& source, const Vars& v) {
        if (source == "quests") {
            Rows& r = quest_rows;
            if (r.seen == v.version() && r.source_seen == quests) return r;
            r.seen = v.version();
            r.source_seen = quests;
            std::vector<Row> next;
            if (quests)
                for (const JournalEntry& j : quests->journal(v)) {
                    Row row;
                    row.id = j.quest->id;
                    row.title = j.quest->title;
                    row.text = j.text;
                    row.done = j.state == QuestState::Done;
                    next.push_back(std::move(row));
                }
            if (next != r.rows) {
                r.rows = std::move(next);
                ++r.changed;
            }
            return r;
        }
        // items: every inv.<id> above 0, the game's known things first in its order
        Rows& r = item_rows;
        if (r.seen == v.version() && r.source_seen == items.data()) return r;
        r.seen = v.version();
        r.source_seen = items.data();
        std::vector<Row> known(items.size()), others;
        constexpr std::string_view prefix = "inv.";
        const auto& all = v.all();
        for (auto it = all.lower_bound(prefix); it != all.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
            if (it->second.is_text()) continue;
            const f64 count = it->second.number();
            if (count <= 0) continue;
            Row row;
            row.id = it->first.substr(prefix.size());
            row.count = count;
            auto k = item_index.find(row.id);
            if (k != item_index.end()) {
                row.item = &items[k->second];
                known[k->second] = std::move(row);
            } else {
                others.push_back(std::move(row));
            }
        }
        std::vector<Row> next;
        next.reserve(known.size() + others.size());
        for (Row& row : known)
            if (!row.id.empty()) next.push_back(std::move(row));
        for (Row& row : others) next.push_back(std::move(row));
        if (next != r.rows) {
            r.rows = std::move(next);
            ++r.changed;
        }
        return r;
    }

    static void fill(ListView::Cell& c, const Row& row, i64 index, const std::string& source) {
        c.vars.clear();
        c.vars.set("item.id", row.id);
        c.vars.set("item.index", static_cast<f64>(index + 1));
        if (source == "quests") {
            c.vars.set("item.title", row.title);
            c.vars.set("item.name", row.title);
            c.vars.set("item.text", row.text);
            c.vars.set("item.done", row.done);
        } else {
            c.vars.set("item.name", row.item && !row.item->name.empty() ? row.item->name : row.id);
            c.vars.set("item.count", row.count);
            c.vars.set("item.icon", row.item ? row.item->picture : std::string());
            c.vars.set("item.about", row.item ? row.item->about : std::string());
        }
    }

    // A list's cells for where it is scrolled now.
    void update_list(Page& p, ListView& l, const Vars& v, const CallFn& call) {
        const Rows& r = rows(l.source, v);
        const i64 n = static_cast<i64>(r.rows.size());
        const int empty = n == 0 ? 1 : 0;
        if (empty != l.empty_shown) {
            l.empty_shown = empty;
            for (Rml::Element* e : l.empties) {
                if (empty) e->RemoveProperty("visibility");
                else e->SetProperty("visibility", "hidden");
            }
        }
        const f32 W = l.box->GetClientWidth(), H = l.box->GetClientHeight();
        const f32 step_x = l.cw + l.gap, step_y = l.ch + l.gap;
        const i64 cols = std::max<i64>(1, static_cast<i64>((W - l.cx + l.gap) / step_x));
        const i64 lines = (n + cols - 1) / cols;
        const f32 height = n ? l.cy * 2 + static_cast<f32>(lines) * step_y - l.gap : 0;
        if (height != l.height) {
            l.height = height;
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.2fpx", static_cast<double>(height));
            l.content->SetProperty("height", buf);
        }
        const f32 scroll = l.box->GetScrollTop();
        i64 first = std::max<i64>(0, static_cast<i64>(std::floor((scroll - l.cy) / step_y)));
        i64 last = std::min<i64>(lines - 1, static_cast<i64>(std::floor((scroll + H - l.cy) / step_y)));
        if (H <= 0) last = std::min<i64>(lines - 1, first); // not laid out yet: one line
        const i64 want = last >= first ? (last - first + 1) * cols : 0;
        while (static_cast<i64>(l.cells.size()) < want) {
            auto c = std::make_unique<ListView::Cell>();
            Rml::ElementPtr clone = l.cell->Clone();
            clone->SetAttribute("forge-item", "");
            clone->SetProperty("position", "absolute");
            clone->SetProperty("margin", "0px");
            clone->SetProperty("pointer-events", "auto");
            clone->SetProperty("display", "none"); // shown when it gets an element
            c->element = l.content->AppendChild(std::move(clone));
            auto visit = [&](auto&& rec, Rml::Element* e) -> void {
                bind_layer(p.name, e, c->bound);
                for (int i = 0; i < e->GetNumChildren(); ++i) rec(rec, e->GetChild(i));
            };
            visit(visit, c->element);
            l.cells.push_back(std::move(c));
        }
        const bool data_changed = l.rows_seen != r.changed;
        l.rows_seen = r.changed;
        for (i64 i = 0; i < static_cast<i64>(l.cells.size()); ++i) {
            ListView::Cell& c = *l.cells[static_cast<usize>(i)];
            const i64 row = i < want ? first * cols + i : -1;
            if (row < 0 || row >= n) {
                c.row = -1;
                if (c.shown) {
                    c.shown = false;
                    c.element->SetProperty("display", "none");
                }
                continue;
            }
            bool refill = row != c.row || data_changed || c.seen != v.version();
            if (!c.shown) {
                // Back in sight: shown, then its layers' conditions apply again
                // (the cell's own show-if too, which also sets display).
                c.shown = true;
                c.element->RemoveProperty("display");
                for (Bound& b : c.bound) b.shown = -1;
                refill = true;
            }
            const f32 left = l.cx + static_cast<f32>(row % cols) * step_x, top = l.cy + static_cast<f32>(row / cols) * step_y;
            char buf[48];
            if (left != c.left) {
                c.left = left;
                std::snprintf(buf, sizeof(buf), "%.2fpx", static_cast<double>(left));
                c.element->SetProperty("left", buf);
            }
            if (top != c.top) {
                c.top = top;
                std::snprintf(buf, sizeof(buf), "%.2fpx", static_cast<double>(top));
                c.element->SetProperty("top", buf);
            }
            if (refill) {
                c.row = row;
                c.seen = v.version();
                c.vars.set_parent(&v);
                fill(c, r.rows[static_cast<usize>(row)], row, l.source);
                for (Bound& b : c.bound) apply(b, c.vars, call);
            }
        }
    }

    bool open(Page& p, const std::string& html, const std::string& address) {
        const std::string rml = ui::html_to_rml(html, "/web/html.rcss");
        // The page's own address: its pictures are found next to it (../pictures/...).
        std::string url = address;
        std::replace(url.begin(), url.end(), '\\', '/');
        std::replace(url.begin(), url.end(), ':', '|');
        p.doc = context->LoadDocumentFromMemory(rml, url);
        if (!p.doc) {
            FORGE_ERROR("экран %s не построился", p.name.c_str());
            return false;
        }
        p.doc->AddEventListener(Rml::EventId::Click, this);
        p.root = nullptr;
        p.bound.clear();
        p.fit_w = p.fit_h = -1;
        p.visible = false;
        p.lists.clear();
        scan(p, p.doc);
        make_lists(p);
        restack();
        return true;
    }

    // Under the game's menus and dialogue box: the screens over the world
    // first, then the windows in the order they were opened.
    void restack() {
        std::vector<Page*> order;
        for (auto& p : pages)
            if (p->doc) order.push_back(p.get());
        auto rank = [](const Page* p) { return p->role == ScreenRole::Command ? 1 : 0; };
        std::stable_sort(order.begin(), order.end(), [&](const Page* a, const Page* b) {
            return rank(a) != rank(b) ? rank(a) > rank(b) : a->order > b->order;
        });
        for (Page* p : order) p->doc->PushToBack(); // the top one first: each next goes under it
    }

    void close(Page& p) {
        if (!p.doc) return;
        p.doc->RemoveEventListener(Rml::EventId::Click, this);
        context->UnloadDocument(p.doc);
        p.doc = nullptr;
        p.root = nullptr;
        p.bound.clear();
        p.lists.clear();
    }

    bool load_file(Page& p) {
        std::vector<u8> bytes;
        if (!read_file(p.path, bytes)) return false;
        std::error_code ec;
        p.time = fs::last_write_time(p.path, ec);
        return open(p, std::string(bytes.begin(), bytes.end()), path_to_utf8(p.path));
    }

    // Pages saved again, new and removed ones.
    void poll() {
        const u64 now = time_now_ns();
        if (now < next_poll) return;
        next_poll = now + 500'000'000;
        std::error_code ec;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".html") continue;
            const std::string name = path_to_utf8(e.path().stem());
            if (name == "components") continue;
            Page* p = find(name);
            if (!p) {
                auto page = std::make_unique<Page>();
                page->name = name;
                page->path = e.path();
                if (load_file(*page)) pages.push_back(std::move(page));
                continue;
            }
            if (fs::last_write_time(e.path(), ec) == p->time) continue;
            close(*p);
            load_file(*p);
        }
        std::erase_if(pages, [&](const std::unique_ptr<Page>& p) {
            if (p->path.empty() || fs::exists(p->path, ec)) return false; // pages from memory have no file
            close(*p);
            return true;
        });
    }

    void fit(Page& p, int w, int h) {
        if (!p.root || (p.fit_w == w && p.fit_h == h)) return;
        p.fit_w = w;
        p.fit_h = h;
        const ScreenFit f = fit_screen(p.fit, p.width, p.height, static_cast<f32>(w), static_cast<f32>(h));
        apply_screen_fit(p.root, f, p.fit == "fit" ? p.bars : std::string());
        p.design_w = f.width;
        p.design_h = f.height;
        p.fit_transform = screen_fit_transform(f);
        place(p, p.move_start ? -1.0f : 1.0f);
    }

    // The root's transform and opacity at a point of its coming (0..1, 1:
    // where it stays; -1: wherever its movement is now).
    void place(Page& p, f32 t) {
        if (!p.root) return;
        if (t < 0) t = progress(p);
        if (p.appear.empty() || t >= 1) {
            p.root->SetProperty("transform", p.fit_transform);
            p.root->RemoveProperty("opacity");
            return;
        }
        t = std::clamp(t, 0.0f, 1.0f);
        const f32 e = p.leaving ? t * t * t : 1 - (1 - t) * (1 - t) * (1 - t); // in: slows down; out: speeds up
        const f32 away = 1 - e;
        f32 dx = 0, dy = 0, z = 1;
        if (p.appear == "rise") dy = 48 * away;
        else if (p.appear == "drop") dy = -48 * away;
        else if (p.appear == "left") dx = -80 * away;
        else if (p.appear == "right") dx = 80 * away;
        else if (p.appear == "zoom") {
            z = 0.9f + 0.1f * e;
            dx = p.design_w * (1 - z) * 0.5f;
            dy = p.design_h * (1 - z) * 0.5f;
        }
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s translate(%.2fpx, %.2fpx) scale(%.4f)", p.fit_transform.c_str(), static_cast<double>(dx),
                      static_cast<double>(dy), static_cast<double>(z));
        p.root->SetProperty("transform", buf);
        p.root->SetProperty("opacity", std::to_string(e));
    }
    f32 progress(const Page& p) const {
        if (!p.move_start) return 1;
        const f64 t = static_cast<f64>(time_now_ns() - p.move_start) * 1e-9 / std::max(p.appear_time, 0.01f);
        const f32 f = static_cast<f32>(std::min(t, 1.0));
        return p.leaving ? 1 - f : f;
    }

    // Puts the data on one layer (only what changed).
    void apply(Bound& b, const Vars& vars, const CallFn& call) {
        if (b.has_show) {
            const int on = b.show.test(vars, call) ? 1 : 0;
            if (on != b.shown) {
                b.shown = on;
                if (on) b.element->RemoveProperty("display");
                else b.element->SetProperty("display", "none");
            }
        }
        if (!b.text.empty()) {
            std::string text = substitute(b.text, vars);
            if (text != b.shown_text) {
                b.element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
                b.shown_text = std::move(text);
            }
        }
        if (b.has_picture) {
            std::string path = b.picture.eval(vars, call).text();
            if (path == "0") path.clear();
            if (path != b.shown_picture) {
                if (path.empty()) b.element->SetProperty("background-image", "none");
                else {
                    std::string url = "url(\"../" + path + "\")";
                    b.element->SetProperty("background-image", url);
                }
                b.shown_picture = std::move(path);
            }
        }
        if (b.has_bar) {
            const f64 max = b.max.eval(vars, call).number();
            f64 t = max > 0 ? b.value.eval(vars, call).number() / max : 0;
            t = std::clamp(t, 0.0, 1.0);
            t = std::round(t * 1000.0) / 1000.0;
            if (t == b.fraction) return;
            b.fraction = t;
            if (t >= 1) {
                b.element->RemoveProperty("mask-image");
                return;
            }
            const char* to = b.from == "right" ? "to left" : b.from == "bottom" ? "to top" : b.from == "top" ? "to bottom" : "to right";
            char buf[160];
            std::snprintf(buf, sizeof(buf), "linear-gradient(%s, #000 %.1f%%, #0000 %.1f%%)", to, t * 100.0, t * 100.0);
            b.element->SetProperty("mask-image", buf);
        }
    }

    void bind(Page& p, const Vars& vars, const CallFn& call) {
        for (Bound& b : p.bound) apply(b, vars, call);
        if (p.lists.empty()) return;
        const u64 t0 = time_now_ns();
        for (auto& l : p.lists) update_list(p, *l, vars, call);
        lists_ms += static_cast<f64>(time_now_ns() - t0) * 1e-6;
    }

    // The music of the screens up now: the newest window shown by command
    // that has music, else a menu's, else a screen's over the world (by name,
    // so it does not depend on the order the files were read in).
    std::string wanted_music() const {
        const Page* top = nullptr;
        auto rank = [](const Page& p) { return p.role == ScreenRole::Command ? 2 : p.role == ScreenRole::Menu ? 1 : 0; };
        for (const auto& page : pages) {
            const Page& p = *page;
            if (!p.doc || !p.visible || p.music.empty()) continue;
            if (!top || rank(p) > rank(*top) ||
                (rank(p) == rank(*top) && (p.role == ScreenRole::Command ? p.order > top->order : p.name < top->name)))
                top = &p;
        }
        return top ? top->music : std::string();
    }

    // The cell an element is in, if any.
    ListView::Cell* cell_of(Rml::Element* e) {
        for (; e; e = e->GetParentNode()) {
            if (!e->HasAttribute("forge-item")) continue;
            for (auto& p : pages)
                for (auto& l : p->lists)
                    for (auto& c : l->cells)
                        if (c->element == e) return c->row >= 0 ? c.get() : nullptr;
            return nullptr;
        }
        return nullptr;
    }
};

ScreenFit fit_screen(std::string_view mode, f32 page_w, f32 page_h, f32 view_w, f32 view_h) {
    page_w = std::max(page_w, 1.0f);
    page_h = std::max(page_h, 1.0f);
    view_w = std::max(view_w, 1.0f);
    view_h = std::max(view_h, 1.0f);
    const f32 sx = view_w / page_w, sy = view_h / page_h, s = std::min(sx, sy);
    ScreenFit f;
    if (mode == "stretch") {
        f.sx = sx;
        f.sy = sy;
        f.width = page_w;
        f.height = page_h;
    } else if (mode == "fit") {
        f.sx = f.sy = s;
        f.left = (view_w - page_w * s) * 0.5f;
        f.top = (view_h - page_h * s) * 0.5f;
        f.width = page_w;
        f.height = page_h;
    } else { // expand: the smaller side's scale, the rest grows
        f.sx = f.sy = s;
        f.width = view_w / s;
        f.height = view_h / s;
    }
    return f;
}

std::string screen_fit_transform(const ScreenFit& f) {
    char buf[96];
    if (f.sx == f.sy) std::snprintf(buf, sizeof(buf), "scale(%.5f)", static_cast<double>(f.sx));
    else std::snprintf(buf, sizeof(buf), "scale(%.5f, %.5f)", static_cast<double>(f.sx), static_cast<double>(f.sy));
    return buf;
}

void apply_screen_fit(Rml::Element* root, const ScreenFit& f, const std::string& bars) {
    if (!root) return;
    char buf[48];
    auto px = [&](f32 v) {
        std::snprintf(buf, sizeof(buf), "%.3fpx", static_cast<double>(v));
        return std::string(buf);
    };
    root->SetProperty("transform-origin", "0px 0px 0px");
    root->SetProperty("width", px(f.width));
    root->SetProperty("height", px(f.height));
    root->SetProperty("left", px(f.left));
    root->SetProperty("top", px(f.top));
    root->SetProperty("transform", screen_fit_transform(f));
    if (!bars.empty())
        if (Rml::ElementDocument* doc = root->GetOwnerDocument()) doc->SetProperty("background-color", bars);
}

GameScreens::GameScreens() : impl_(std::make_unique<Impl>()) { impl_->self = this; }
GameScreens::~GameScreens() { unload(); }

void GameScreens::load(Rml::Context* context, const fs::path& game_dir, bool watch) {
    unload();
    impl_->context = context;
    impl_->dir = game_dir / "ui";
    impl_->watch = watch;
    impl_->next_poll = 0;
    impl_->poll();
    if (!impl_->pages.empty()) FORGE_INFO("экраны игры: %zu", impl_->pages.size());
}

bool GameScreens::load_page(Rml::Context* context, const std::string& name, const std::string& html, const std::string& url) {
    impl_->context = context;
    Page* p = impl_->find(name);
    if (p) impl_->close(*p);
    else {
        impl_->pages.push_back(std::make_unique<Page>());
        p = impl_->pages.back().get();
        p->name = name;
    }
    return impl_->open(*p, html, url);
}

void GameScreens::unload() {
    for (auto& p : impl_->pages) impl_->close(*p);
    impl_->pages.clear();
}

void GameScreens::remove(std::string_view name) {
    std::erase_if(impl_->pages, [&](const std::unique_ptr<Page>& p) {
        if (p->name != name) return false;
        impl_->close(*p);
        return true;
    });
}

void GameScreens::update(const Vars& vars, bool playing, bool menu, int width, int height, const CallFn& call) {
    if (impl_->watch && !impl_->dir.empty()) impl_->poll();
    impl_->lists_ms = 0;
    bool restacked = false;
    for (auto& page : impl_->pages) {
        Page& p = *page;
        if (!p.doc) continue;
        // A window opened from the menu is over the menu (the game starting closes it: `Shell::begin`).
        const bool visible = p.role == ScreenRole::Menu ? menu : p.role == ScreenRole::Playing ? playing : p.shown;
        if (visible) {
            impl_->fit(p, width, height);
            impl_->bind(p, vars, call);
        }
        if (visible != p.visible) {
            p.visible = visible;
            const bool moves = !p.appear.empty() && p.root;
            if (visible) {
                if (!p.doc->IsVisible()) {
                    // A menu or a window that stops the game takes the keyboard (Tab goes to its buttons); a
                    // screen over the running game leaves it to the game.
                    const bool keys = p.role == ScreenRole::Menu || (p.role == ScreenRole::Command && p.pauses);
                    p.doc->Show(Rml::ModalFlag::None, keys ? Rml::FocusFlag::Document : Rml::FocusFlag::None);
                    restacked = true;
                }
                // Comes in (from wherever it was going away).
                const f32 from = p.move_start ? impl_->progress(p) : 0.0f;
                p.leaving = false;
                p.move_start = moves ? time_now_ns() - static_cast<u64>(from * p.appear_time * 1e9) : 0;
                if (moves) impl_->place(p, from);
            } else if (moves) {
                const f32 from = p.move_start ? impl_->progress(p) : 1.0f;
                p.leaving = true;
                p.move_start = time_now_ns() - static_cast<u64>((1 - from) * p.appear_time * 1e9);
            } else {
                p.doc->Hide();
                restacked = true;
            }
        }
        if (p.move_start) {
            const f32 t = impl_->progress(p);
            const bool done = p.leaving ? t <= 0 : t >= 1;
            if (done) {
                p.move_start = 0;
                if (p.leaving) {
                    p.leaving = false;
                    p.doc->Hide();
                    restacked = true;
                }
                impl_->place(p, 1);
            } else {
                impl_->place(p, t);
            }
        }
    }
    if (restacked) impl_->restack();
    if (std::string m = impl_->wanted_music(); m != impl_->music) {
        impl_->music = std::move(m);
        if (on_music) on_music(impl_->music);
    }
}

const std::string& GameScreens::music() const { return impl_->music; }

void GameScreens::stop_music() {
    impl_->music.clear();
    if (on_music) on_music({});
}

bool GameScreens::has_focus(Rml::Context* context) const {
    Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
    if (!focus || !impl_->of(focus->GetOwnerDocument())) return false;
    for (Rml::Element* e = focus; e; e = e->GetParentNode())
        if (e->HasAttribute("forge-click")) return true;
    return false;
}

bool GameScreens::show(std::string_view name, bool on) {
    Page* p = impl_->find(name);
    if (!p) {
        FORGE_WARN("нет экрана «%.*s»", static_cast<int>(name.size()), name.data());
        return false;
    }
    if (on && !p->shown) p->order = ++impl_->shows;
    p->shown = on;
    return true;
}

bool GameScreens::toggle(std::string_view name) { return show(name, !shown(name)); }

bool GameScreens::shown(std::string_view name) const {
    const Page* p = impl_->find(name);
    return p && (p->shown || (p->role != ScreenRole::Command && p->visible));
}

bool GameScreens::exists(std::string_view name) const { return impl_->find(name) != nullptr; }

void GameScreens::hide_commands() {
    for (auto& p : impl_->pages) p->shown = false;
}

bool GameScreens::close_top() {
    Page* top = nullptr;
    for (auto& p : impl_->pages)
        if (p->role == ScreenRole::Command && p->shown && p->esc && (!top || p->order > top->order)) top = p.get();
    if (!top) return false;
    top->shown = false;
    return true;
}

bool GameScreens::pauses() const {
    for (auto& p : impl_->pages)
        if (p->role == ScreenRole::Command && p->shown && p->pauses) return true;
    return false;
}

bool GameScreens::has_menu() const {
    for (auto& p : impl_->pages)
        if (p->role == ScreenRole::Menu && p->doc) return true;
    return false;
}

std::vector<std::string> GameScreens::names() const {
    std::vector<std::string> out;
    for (auto& p : impl_->pages) out.push_back(p->name);
    return out;
}

Rml::ElementDocument* GameScreens::document(std::string_view name) const {
    const Page* p = impl_->find(name);
    return p ? p->doc : nullptr;
}

bool GameScreens::run_page_action(const ScreenAction& a, const std::string& page) {
    if (a.what == "show") return show(a.target, true), true;
    if (a.what == "hide") return show(a.target.empty() ? page : a.target, false), true;
    if (a.what == "toggle") return toggle(a.target), true;
    if (a.what == "close") return show(page, false), true;
    return false;
}

bool GameScreens::click(Rml::Element* element) {
    for (Rml::Element* e = element; e; e = e->GetParentNode()) {
        if (!e->HasAttribute("forge-click")) continue;
        for (Rml::Element* up = e; up; up = up->GetParentNode())
            if (up->HasAttribute("forge-disabled")) return true; // off: no actions, no sound
        // The mouse leaves the focus on what it pressed, often a label inside: the button itself takes it, so
        // Enter and Space press it again (only a button the keyboard may press, `tab-index: auto`).
        if (e->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto && e->GetContext() &&
            e->GetContext()->GetFocusElement() != e)
            e->Focus();
        Page* p = impl_->of(e->GetOwnerDocument());
        const std::string page = p ? p->name : std::string();
        // Its sound once, as it does what it does.
        std::string sound = e->HasAttribute("forge-click-sound") ? e->GetAttribute<Rml::String>("forge-click-sound", "")
                            : p                                  ? p->button_sound
                                                                 : std::string();
        if (sound == "none") sound.clear();
        if (!sound.empty() && on_sound) on_sound(sound);
        // In a list's cell, {item.id} and the like are the element it shows now.
        const ListView::Cell* cell = impl_->cell_of(e);
        for (ScreenAction a : parse_actions(e->GetAttribute<Rml::String>("forge-click", ""))) {
            if (cell) a.target = substitute(a.target, cell->vars);
            if ((actions_to_caller || !run_page_action(a, page)) && on_action) on_action(a, page);
        }
        return true;
    }
    return false;
}

std::vector<std::string> GameScreens::variables(std::string_view name) const {
    std::vector<std::string> out;
    const Page* p = impl_->find(name);
    if (!p || !p->doc) return out;
    auto visit = [&](auto&& self, Rml::Element* e) -> void {
        const auto attr = [&](const char* a) { return e->GetAttribute<Rml::String>(a, ""); };
        text_variables(attr("forge-text"), out);
        for (const char* a : {"forge-show-if", "forge-bar-value", "forge-bar-max"}) {
            if (!e->HasAttribute(a)) continue;
            std::string error;
            Expr x = Expr::parse(attr(a), &error);
            x.collect(nullptr, &out);
        }
        for (int i = 0; i < e->GetNumChildren(); ++i) self(self, e->GetChild(i));
    };
    visit(visit, p->doc);
    for (const auto& l : p->lists)
        if (l->cell) visit(visit, l->cell.get());
    // Variables changed by clicks («Изменить данные»).
    auto clicks = [&](auto&& self, Rml::Element* e) -> void {
        if (e->HasAttribute("forge-click"))
            for (const ScreenAction& a : parse_actions(e->GetAttribute<Rml::String>("forge-click", "")))
                if (a.what == "change") {
                    std::string error;
                    Expr::parse_actions(a.target, &error).collect(nullptr, &out);
                }
        for (int i = 0; i < e->GetNumChildren(); ++i) self(self, e->GetChild(i));
    };
    clicks(clicks, p->doc);
    for (const auto& l : p->lists)
        if (l->cell) clicks(clicks, l->cell.get());
    // Cells read their element (item.*), not the game's variables; a list
    // reads the variables its elements come from.
    std::erase_if(out, [](const std::string& v) { return v.rfind("item.", 0) == 0; });
    for (const auto& l : p->lists) {
        if (l->source == "quests" && impl_->quests) {
            for (const Quest& q : impl_->quests->quests())
                if (!q.var.empty()) out.push_back(q.var);
        } else if (l->source != "quests") {
            for (const ScreenItem& it : impl_->items) out.push_back("inv." + it.id);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void GameScreens::set_items(std::vector<ScreenItem> items) {
    impl_->items = std::move(items);
    impl_->item_index.clear();
    for (usize i = 0; i < impl_->items.size(); ++i) impl_->item_index.emplace(impl_->items[i].id, i);
    impl_->item_rows.seen = ~0ull;
    impl_->item_rows.source_seen = nullptr;
}

void GameScreens::set_quests(const QuestBook* quests) {
    impl_->quests = quests;
    impl_->quest_rows.seen = ~0ull;
}

usize GameScreens::list_cells(std::string_view name) const {
    const Page* p = impl_->find(name);
    usize n = 0;
    if (p)
        for (const auto& l : p->lists) n += l->cells.size();
    return n;
}

f64 GameScreens::lists_ms() const { return impl_->lists_ms; }

} // namespace forge::game
