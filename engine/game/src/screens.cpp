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
    // What was put on the element last, so unchanged values are not set again.
    std::string shown_text;
    int shown = -1;  // -1: not yet
    f64 fraction = -1;
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
    u64 move_start = 0;   // 0: none
    bool leaving = false; // going away: hidden when the movement ends
    std::vector<Bound> bound;
};

struct GameScreens::Impl : Rml::EventListener {
    GameScreens* self = nullptr;
    Rml::Context* context = nullptr;
    fs::path dir;
    bool watch = false;
    u64 next_poll = 0;
    u64 shows = 0;
    std::vector<std::unique_ptr<Page>> pages;

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
        }
        Bound b;
        b.element = e;
        bool any = false;
        if (e->HasAttribute("forge-text")) {
            b.text = attr("forge-text");
            any = true;
        }
        if (e->HasAttribute("forge-show-if")) {
            b.show = parse_expr(attr("forge-show-if"), p.name, "условие");
            b.has_show = true;
            any = true;
        }
        if (e->HasAttribute("forge-bar-value")) {
            b.value = parse_expr(attr("forge-bar-value"), p.name, "значение полоски");
            b.max = parse_expr(attr("forge-bar-max"), p.name, "наибольшее значение полоски");
            b.from = attr("forge-bar-from");
            b.has_bar = true;
            any = true;
        }
        if (any) p.bound.push_back(std::move(b));
        for (int i = 0; i < e->GetNumChildren(); ++i) scan(p, e->GetChild(i));
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
        scan(p, p.doc);
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
            if (fs::exists(p->path, ec)) return false;
            close(*p);
            return true;
        });
    }

    void fit(Page& p, int w, int h) {
        if (!p.root || (p.fit_w == w && p.fit_h == h)) return;
        p.fit_w = w;
        p.fit_h = h;
        const f32 W = static_cast<f32>(std::max(w, 1)), H = static_cast<f32>(std::max(h, 1));
        const f32 sx = W / p.width, sy = H / p.height, s = std::min(sx, sy);
        char buf[96];
        auto px = [&](f32 v) {
            std::snprintf(buf, sizeof(buf), "%.3fpx", static_cast<double>(v));
            return std::string(buf);
        };
        Rml::Element* r = p.root;
        r->SetProperty("transform-origin", "0px 0px 0px");
        if (p.fit == "stretch") {
            r->SetProperty("width", px(p.width));
            r->SetProperty("height", px(p.height));
            p.design_w = p.width;
            p.design_h = p.height;
            std::snprintf(buf, sizeof(buf), "scale(%.5f, %.5f)", static_cast<double>(sx), static_cast<double>(sy));
        } else if (p.fit == "fit") {
            r->SetProperty("width", px(p.width));
            r->SetProperty("height", px(p.height));
            r->SetProperty("left", px((W - p.width * s) * 0.5f));
            r->SetProperty("top", px((H - p.height * s) * 0.5f));
            p.design_w = p.width;
            p.design_h = p.height;
            std::snprintf(buf, sizeof(buf), "scale(%.5f)", static_cast<double>(s));
            if (!p.bars.empty())
                if (Rml::Element* body = p.doc) body->SetProperty("background-color", p.bars);
        } else { // expand: the smaller side's scale, the rest grows
            r->SetProperty("width", px(W / s));
            r->SetProperty("height", px(H / s));
            p.design_w = W / s;
            p.design_h = H / s;
            std::snprintf(buf, sizeof(buf), "scale(%.5f)", static_cast<double>(s));
        }
        p.fit_transform = buf;
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

    void bind(Page& p, const Vars& vars, const CallFn& call) {
        for (Bound& b : p.bound) {
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
            if (b.has_bar) {
                const f64 max = b.max.eval(vars, call).number();
                f64 t = max > 0 ? b.value.eval(vars, call).number() / max : 0;
                t = std::clamp(t, 0.0, 1.0);
                t = std::round(t * 1000.0) / 1000.0;
                if (t == b.fraction) continue;
                b.fraction = t;
                if (t >= 1) {
                    b.element->RemoveProperty("mask-image");
                    continue;
                }
                const char* to = b.from == "right" ? "to left" : b.from == "bottom" ? "to top" : b.from == "top" ? "to bottom" : "to right";
                char buf[160];
                std::snprintf(buf, sizeof(buf), "linear-gradient(%s, #000 %.1f%%, #0000 %.1f%%)", to, t * 100.0, t * 100.0);
                b.element->SetProperty("mask-image", buf);
            }
        }
    }
};

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
    bool restacked = false;
    for (auto& page : impl_->pages) {
        Page& p = *page;
        if (!p.doc) continue;
        const bool visible = p.role == ScreenRole::Menu ? menu : p.role == ScreenRole::Playing ? playing : p.shown && !menu;
        if (visible) {
            impl_->fit(p, width, height);
            impl_->bind(p, vars, call);
        }
        if (visible != p.visible) {
            p.visible = visible;
            const bool moves = !p.appear.empty() && p.root;
            if (visible) {
                if (!p.doc->IsVisible()) {
                    p.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
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
        Page* p = impl_->of(e->GetOwnerDocument());
        const std::string page = p ? p->name : std::string();
        for (const ScreenAction& a : parse_actions(e->GetAttribute<Rml::String>("forge-click", "")))
            if ((actions_to_caller || !run_page_action(a, page)) && on_action) on_action(a, page);
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
    for (const char* sheet : {"forge-click"}) (void)sheet;
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
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace forge::game
