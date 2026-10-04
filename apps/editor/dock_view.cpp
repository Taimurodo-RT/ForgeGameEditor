#include "dock_view.h"

#include "forge/core/file.h"
#include "forge/core/log.h"

#include <cmath>

namespace forge::editor_app {

namespace fs = std::filesystem;

namespace {

constexpr f32 kGap = 6;
constexpr f32 kHeader = 32;

Rml::Element* find(Rml::Context* context, const std::string& id) {
    if (!context) return nullptr;
    for (int i = 0; i < context->GetNumDocuments(); ++i)
        if (Rml::Element* e = context->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

void place_element(Rml::Element* e, const editor::DockRect* r) {
    if (!e) return;
    if (!r) {
        e->SetProperty("display", "none");
        return;
    }
    auto px = [](f32 v) { return std::to_string(static_cast<int>(std::lround(v))) + "px"; };
    e->SetProperty("display", "flex");
    e->SetProperty("left", px(r->x));
    e->SetProperty("top", px(r->y));
    e->SetProperty("width", px(r->w));
    e->SetProperty("height", px(r->h));
}

} // namespace

void DockView::register_types(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Tab>()) {
        s.RegisterMember("id", &Tab::id);
        s.RegisterMember("title", &Tab::title);
        s.RegisterMember("icon", &Tab::icon);
        s.RegisterMember("active", &Tab::active);
        s.RegisterMember("used", &Tab::used);
    }
    model.RegisterArray<std::vector<Tab>>();
    if (auto s = model.RegisterStruct<Frame>()) {
        s.RegisterMember("x", &Frame::x);
        s.RegisterMember("y", &Frame::y);
        s.RegisterMember("w", &Frame::w);
        s.RegisterMember("h", &Frame::h);
        s.RegisterMember("view", &Frame::view);
        s.RegisterMember("used", &Frame::used);
        s.RegisterMember("tabs", &Frame::tabs);
    }
    model.RegisterArray<std::vector<Frame>>();
    if (auto s = model.RegisterStruct<Gap>()) {
        s.RegisterMember("x", &Gap::x);
        s.RegisterMember("y", &Gap::y);
        s.RegisterMember("w", &Gap::w);
        s.RegisterMember("h", &Gap::h);
        s.RegisterMember("column", &Gap::column);
        s.RegisterMember("used", &Gap::used);
    }
    model.RegisterArray<std::vector<Gap>>();
}

void DockView::init(DockConfig config, const fs::path& settings, bool persist) {
    config_ = std::move(config);
    settings_ = settings;
    persist_ = persist && !settings.empty();
    ids_.clear();
    for (const auto& p : config_.panels) ids_.push_back(p.id);
    dock_.load(config_.default_layout, ids_);
    std::vector<u8> bytes;
    if (!settings_.empty() && read_file(settings_ / config_.file, bytes)) {
        if (!dock_.load(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ids_))
            FORGE_WARN("Раскладка панелей не прочиталась, взята обычная");
    }
}

void DockView::bind(Rml::DataModelConstructor& model) {
    const std::string& p = config_.prefix;
    model.Bind(p + "dock_frames", &m_frames_);
    model.Bind(p + "dock_gaps", &m_gaps_);
    model.Bind(p + "dock_drop", &m_drop_);
    model.Bind(p + "dock_drop_x", &m_drop_x_);
    model.Bind(p + "dock_drop_y", &m_drop_y_);
    model.Bind(p + "dock_drop_w", &m_drop_w_);
    model.Bind(p + "dock_drop_h", &m_drop_h_);
    model.BindEventCallback(p + "dock_grab", [this](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& a) {
        grab_panel_ = a.empty() ? std::string() : a[0].Get<Rml::String>();
        dragging_ = false;
        grab_x_ = ev.GetParameter<float>("mouse_x", mouse_x_);
        grab_y_ = ev.GetParameter<float>("mouse_y", mouse_y_);
    });
    model.BindEventCallback(p + "dock_split", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a) {
        grab_splitter_ = a.empty() ? -1 : a[0].Get<int>();
    });
    model.BindEventCallback(p + "dock_reset", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { reset(); });
}

const DockConfig::Panel* DockView::panel(std::string_view id) const {
    for (const auto& p : config_.panels)
        if (p.id == id) return &p;
    return nullptr;
}

bool DockView::update(Rml::Context* context) {
    Rml::Element* area = find(context, config_.area);
    if (!area || !area->IsVisible(true)) {
        view_ = {};
        return false;
    }
    const Rml::Vector2f at = area->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = area->GetBox().GetSize(Rml::BoxArea::Content);
    const bool moved = at.x != dock_x_ || at.y != dock_y_ || size.x != dock_w_ || size.y != dock_h_;
    dock_x_ = at.x;
    dock_y_ = at.y;
    dock_w_ = size.x;
    dock_h_ = size.y;
    if (moved || applied_version_ != dock_.version()) {
        dock_.layout({0, 0, dock_w_, dock_h_}, kGap, kHeader);
        applied_version_ = dock_.version();
        for (const auto& p : config_.panels) {
            editor::DockRect r;
            const bool shown = dock_.panel_rect(p.id, r);
            place_element(find(context, config_.pane_prefix + p.id), shown ? &r : nullptr);
        }
        view_ = dock_.view_rect();
        place_element(find(context, config_.view), &view_);
        m_frames_.clear();
        for (const auto& s : dock_.stacks()) {
            Frame f{s.frame.x, s.frame.y, s.frame.w, s.frame.h, s.view, true, {}};
            for (usize i = 0; i < s.panels.size(); ++i) {
                const DockConfig::Panel* info = panel(s.panels[i]);
                f.tabs.push_back({s.panels[i], info ? info->title : s.panels[i], info ? info->icon : "tab", i == s.active, true});
            }
            f.tabs.resize(config_.panels.size()); // one length, as the frames
            m_frames_.push_back(std::move(f));
        }
        m_gaps_.clear();
        for (const auto& g : dock_.splitters()) m_gaps_.push_back({g.rect.x, g.rect.y, g.rect.w, g.rect.h, g.column, true});
        // The lists keep one length (unused entries hidden): RmlUi complains
        // when a list with lists inside it gets shorter.
        const usize most = config_.panels.size() + 1;
        m_frames_.resize(most);
        for (Frame& f : m_frames_) f.tabs.resize(config_.panels.size());
        m_gaps_.resize(most);
        model_.DirtyVariable(config_.prefix + "dock_frames");
        model_.DirtyVariable(config_.prefix + "dock_gaps");
    }
    view_ = dock_.view_rect();
    return true;
}

bool DockView::mouse_move(f32 x, f32 y) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (grab_splitter_ >= 0) {
        dock_.drag_splitter(static_cast<usize>(grab_splitter_), x - dock_x_, y - dock_y_);
        return true;
    }
    if (grab_panel_.empty()) return false;
    if (!dragging_ && std::hypot(x - grab_x_, y - grab_y_) > 6) dragging_ = true;
    if (dragging_) {
        drop_ = dock_.drop_at(x - dock_x_, y - dock_y_, grab_panel_);
        const bool show = drop_.zone != editor::DockLayout::Zone::None;
        set(m_drop_, show, "dock_drop");
        set(m_drop_x_, drop_.preview.x, "dock_drop_x");
        set(m_drop_y_, drop_.preview.y, "dock_drop_y");
        set(m_drop_w_, drop_.preview.w, "dock_drop_w");
        set(m_drop_h_, drop_.preview.h, "dock_drop_h");
    }
    return true;
}

bool DockView::mouse_up() {
    bool used = false;
    if (grab_splitter_ >= 0) {
        grab_splitter_ = -1;
        save();
        used = true;
    }
    if (!grab_panel_.empty()) {
        if (dragging_) {
            if (dock_.move(grab_panel_, drop_)) save();
        } else {
            dock_.activate(grab_panel_);
            save();
        }
        grab_panel_.clear();
        dragging_ = false;
        drop_ = {};
        set(m_drop_, false, "dock_drop");
        used = true;
    }
    return used;
}

void DockView::reset() {
    dock_.load(config_.default_layout, ids_);
    save();
}

void DockView::save() {
    if (!persist_) return;
    std::error_code ec;
    fs::create_directories(settings_, ec);
    const std::string text = dock_.save();
    write_file_atomic(settings_ / config_.file, {reinterpret_cast<const u8*>(text.data()), text.size()});
}

} // namespace forge::editor_app
