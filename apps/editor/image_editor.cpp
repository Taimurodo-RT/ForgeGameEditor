#include "image_editor.h"

#include "asset_library.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/editor/sources.h"
#include "forge/ui/tokens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace forge::editor_app {

namespace fs = std::filesystem;
using assets::CookedTexture;
using assets::PixelRect;
using assets::Rgba;

// Drawing on pictures up to this many pixels; a bigger one only turns, changes size, takes effects and saves.
constexpr u64 kDrawPixels = 4096ull * 4096ull;
// No picture made bigger than this by a transformation.
constexpr u64 kMaxPixels = 8192ull * 8192ull;
constexpr u32 kZooms[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};

struct ImageEditor::Picture {
    Guid id;                  // of «Ресурсы» (none yet: a file «Сохранить как» just made, until «Ресурсы» sees it)
    std::string rel;          // its path there
    fs::path file;
    std::string ext;          // ".png"
    CookedTexture image;      // the working copy
    std::string disk_hash;    // of the file as read or written last
    editor::Document doc;     // the history wants one
    editor::UndoStack history{doc};
    u32 own_frames = 1;       // the tab's number of frames (no template draws the picture)
    int area = -1;            // -1: the whole picture
    u32 zoom = 0;             // 0: fit when first drawn
    f64 cx = 0, cy = 0;       // the canvas's middle, in the picture's pixels
    bool outside = false;     // the file changed outside while there are unsaved changes
    bool missing = false;     // the file is gone
    bool kept = false;        // «Оставить мои»: the file is another program's, no state of the history is on disk
    bool view_only = false;   // a picture of the game that «Ресурсы» has not: shown, never changed or written
    std::string game_file;    // shown only: its path in the game ("pictures/интерфейс/рамка_дерево.png")
    std::string copy_of;      // shown only: the picture of «Ресурсы» the file became a copy of since («картинки/x.png»)

    // What Ctrl+S would change on disk: the history away from its saved state, or the file kept off.
    bool unsaved() const { return kept || history.dirty(); }
};

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string extension_of(const fs::path& file) { return lower(path_to_utf8(file.extension())); }

std::string format_of(const std::string& ext) {
    if (ext == ".jpg" || ext == ".jpeg") return "JPEG";
    std::string s = ext.empty() ? std::string("?") : ext.substr(1);
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s == "WEBP" ? "WebP" : s;
}

std::string name_of(const std::string& rel) {
    const usize slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

std::string folder_of(const std::string& rel) {
    const usize slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

// A name as the alphabet orders it: Ё with Е.
std::string alphabet_key(std::string s) {
    for (usize at = 0; (at = s.find("Ё", at)) != std::string::npos;) s.replace(at, 2, "Е");
    for (usize at = 0; (at = s.find("ё", at)) != std::string::npos;) s.replace(at, 2, "е");
    return lower(s);
}

std::string hex_of(Rgba c) {
    char buf[16];
    if (c.a == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

std::string css_of(Rgba c) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "rgba(%u, %u, %u, %u)", c.r, c.g, c.b, c.a);
    return buf;
}

// A token of the theme ("#rrggbb" or "#rrggbbaa").
bool parse_hex(std::string s, Rgba& out) {
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    u8 v[4] = {0, 0, 0, 255};
    for (usize i = 0; i < s.size() / 2; ++i) {
        int n = 0;
        for (usize k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            n = n * 16 + d;
        }
        v[i] = static_cast<u8>(n);
    }
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

Rgba rgba_of(editor::design::Color c) { return {c.r, c.g, c.b, c.a}; }
editor::design::Color color_of(Rgba c) { return {c.r, c.g, c.b, c.a}; }

// The kinds of pictures the tab reads (decode_image).
bool picture_extension(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".gif" || ext == ".psd" ||
           ext == ".webp";
}

std::string count_words(u32 n, const char* one, const char* few, const char* many) {
    const u32 d = n % 10, h = n % 100;
    return std::to_string(n) + " " + (d == 1 && h != 11 ? one : d >= 2 && d <= 4 && (h < 12 || h > 14) ? few : many);
}

std::string size_words(const CookedTexture& t) { return std::to_string(t.width) + " × " + std::to_string(t.height); }

i64 floor_div(i64 a, i64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// One step of a picture's history: its pixels (and size, and the tab's number of frames) before and after.
class PixelStep final : public editor::Command {
public:
    struct Side {
        u32 w = 0, h = 0, frames = 1;
        PixelRect rect;
        std::vector<u8> pixels;
    };
    PixelStep(ImageEditor::Picture& p, std::function<void()> touched, Side before, Side after, std::string label)
        : p_(p), touched_(std::move(touched)), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { put(after_); }
    void revert(editor::Document&) override { put(before_); }
    std::string label() const override { return label_; }

private:
    void put(const Side& s);
    ImageEditor::Picture& p_;
    std::function<void()> touched_;
    Side before_, after_;
    std::string label_;
};

} // namespace

void PixelStep::put(const Side& s) {
    CookedTexture& img = p_.image;
    if (img.width != s.w || img.height != s.h) {
        img.width = s.w;
        img.height = s.h;
        img.rgba8.assign(static_cast<usize>(s.w) * s.h * 4, 0);
    }
    assets::paste_rect(img, s.rect, s.pixels);
    p_.own_frames = s.frames;
    if (touched_) touched_();
}

ImageEditor::ImageEditor(AssetLibrary& assets) : assets_(assets) {
    write_file = [](const fs::path& path, std::span<const u8> bytes) { return write_file_atomic(path, bytes); };
}

ImageEditor::~ImageEditor() = default;

bool ImageEditor::init(ui::Ui& ui) {
    ui_ = &ui;
    struct E {
        const char* id;
        const char* label;
    };
    for (const E& e : {E{"light", "Яркость и контраст"}, E{"hue", "Тон, насыщенность, светлота"}, E{"invert", "Инверсия"},
                       E{"gray", "Оттенки серого"}, E{"outline", "Контур 1 px"}, E{"alpha", "Порог альфы"}, E{"reduce", "Меньше цветов"},
                       E{"replace", "Замена цвета"}})
        m_effects_.push_back({e.id, e.label, std::string(e.id) == effect_});
    return true;
}

void ImageEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<ListRow>()) {
        s.RegisterMember("rel", &ListRow::rel);
        s.RegisterMember("name", &ListRow::name);
        s.RegisterMember("folder", &ListRow::folder);
        s.RegisterMember("header", &ListRow::header);
        s.RegisterMember("selected", &ListRow::selected);
        s.RegisterMember("unsaved", &ListRow::unsaved);
        s.RegisterMember("game", &ListRow::game);
        s.RegisterMember("index", &ListRow::index);
    }
    model.RegisterArray<std::vector<ListRow>>();
    if (auto s = model.RegisterStruct<Swatch>()) {
        s.RegisterMember("css", &Swatch::css);
        s.RegisterMember("hex", &Swatch::hex);
    }
    model.RegisterArray<std::vector<Swatch>>();
    if (auto s = model.RegisterStruct<AreaChip>()) {
        s.RegisterMember("label", &AreaChip::label);
        s.RegisterMember("frame", &AreaChip::frame);
        s.RegisterMember("selected", &AreaChip::selected);
    }
    model.RegisterArray<std::vector<AreaChip>>();
    if (auto s = model.RegisterStruct<EffectChip>()) {
        s.RegisterMember("id", &EffectChip::id);
        s.RegisterMember("label", &EffectChip::label);
        s.RegisterMember("selected", &EffectChip::selected);
    }
    model.RegisterArray<std::vector<EffectChip>>();
    if (auto s = model.RegisterStruct<Param>()) {
        s.RegisterMember("label", &Param::label);
        s.RegisterMember("min", &Param::min);
        s.RegisterMember("max", &Param::max);
        s.RegisterMember("value", &Param::value);
    }
    model.RegisterArray<std::vector<Param>>();
    model.Bind("im_list", &m_list_);
    model.Bind("im_colors", &m_colors_);
    model.Bind("im_areas", &m_areas_);
    model.Bind("im_effects", &m_effects_);
    model.Bind("im_params", &m_params_);
    model.Bind("im_has", &m_has_);
    model.Bind("im_name", &m_name_);
    model.Bind("im_info", &m_info_);
    model.Bind("im_unsaved", &m_unsaved_);
    model.Bind("im_status", &m_status_);
    model.Bind("im_canvas", &m_canvas_);
    model.Bind("im_canvas_w", &m_canvas_w_);
    model.Bind("im_canvas_h", &m_canvas_h_);
    model.Bind("im_tool", &m_tool_);
    model.Bind("im_size", &m_size_);
    model.Bind("im_c1", &m_c1_);
    model.Bind("im_c2", &m_c2_);
    model.Bind("im_c1_hex", &m_c1_hex_);
    model.Bind("im_c2_hex", &m_c2_hex_);
    model.Bind("im_color_note", &m_color_note_);
    model.Bind("im_view", &m_view_);
    model.Bind("im_view_text", &m_view_text_);
    model.Bind("im_view_copy", &m_view_copy_);
    model.Bind("im_frames", &m_frames_);
    model.Bind("im_frames_own", &m_frames_own_);
    model.Bind("im_frames_from", &m_frames_from_);
    model.Bind("im_note", &m_note_);
    model.Bind("im_warning", &m_warning_);
    model.Bind("im_outside", &m_outside_shown_);
    model.Bind("im_outside_text", &m_outside_);
    model.Bind("im_effect", &m_effect_);
    model.Bind("im_trying", &m_trying_);
    model.Bind("im_zoom", &m_zoom_);
    model.Bind("im_rw", &m_rw_);
    model.Bind("im_rh", &m_rh_);
    model.Bind("im_smooth", &m_smooth_);
    model.Bind("im_cw", &m_cw_);
    model.Bind("im_ch", &m_ch_);
    model.Bind("im_anchor", &m_anchor_);
    model.Bind("im_gw", &m_gw_);
    model.Bind("im_gh", &m_gh_);
    model.Bind("im_gc", &m_gc_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i, int fallback = -1) { return i < a.size() ? a[i].Get<int>(fallback) : fallback; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto number = [](const Rml::String& s) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s.c_str(), &end, 10);
        return end && end != s.c_str() && v <= 1000000 ? static_cast<u32>(v) : 0u;
    };
    on("im_open", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i < 0 || i >= static_cast<int>(m_list_.size()) || m_list_[static_cast<usize>(i)].header) return;
        const ListRow& r = m_list_[static_cast<usize>(i)];
        if (r.game) open_game(r.rel);
        else open(assets_.abs(r.rel));
    });
    // Shown only, and a copy of «Ресурсы» since: its picture there, opened as itself.
    on("im_open_copy", [this](Rml::Event&, const Rml::VariantList&) {
        if (current_ && current_->view_only && !current_->copy_of.empty()) open(assets_.abs(current_->copy_of));
    });
    on("im_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { set_tool(arg_str(a, 0)); });
    on("im_size", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { set_size(static_cast<u32>(std::max(1, arg_int(a, 0, 1)))); });
    on("im_swap", [this](Rml::Event&, const Rml::VariantList&) { swap_colors(); });
    // A colour's swatch: the picker beside it.
    on("im_color", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        ev.StopPropagation();
        Rml::Element* swatch = ev.GetCurrentElement();
        if (!swatch) return;
        if (Rml::Context* c = swatch->GetContext()) context_ = c;
        const Rml::Vector2f at = swatch->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = swatch->GetBox().GetSize(Rml::BoxArea::Border);
        open_color(arg_int(a, 0) == 1, at.x, at.y, size.x, size.y);
    });
    on("im_pick", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i < 0 || i >= static_cast<int>(colors_.size())) return;
        const bool right = ev.GetParameter<int>("button", 0) == 1;
        (right ? second_ : first_) = colors_[static_cast<usize>(i)];
        (right ? second_theme_ : first_theme_).clear();
        rebuild_panels();
    });
    on("im_area", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { set_area(arg_int(a, 0, -1)); });
    on("im_frames_less", [this](Rml::Event&, const Rml::VariantList&) { set_frames(frames() > 1 ? frames() - 1 : 1); });
    on("im_frames_more", [this](Rml::Event&, const Rml::VariantList&) { set_frames(frames() + 1); });
    on("im_turn", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string t = arg_str(a, 0);
        turn(t == "cw" ? assets::Turn::Cw : t == "ccw" ? assets::Turn::Ccw : t == "half" ? assets::Turn::Half
             : t == "fliph" ? assets::Turn::FlipH : assets::Turn::FlipV);
    });
    on("im_smooth", [this](Rml::Event&, const Rml::VariantList&) {
        m_smooth_ = !m_smooth_;
        dirty("im_smooth");
    });
    on("im_resize", [this, number](Rml::Event&, const Rml::VariantList&) {
        const u32 w = number(m_rw_), h = number(m_rh_);
        if (!w || !h) say("Ширина и высота — целые числа от 1");
        else resize(w, h, m_smooth_);
    });
    on("im_percent", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (!current_) return;
        const u32 n = std::max(1u, frames()), pct = static_cast<u32>(std::max(1, arg_int(a, 0, 100)));
        const u32 fw = current_->image.width / n, fh = current_->image.height;
        resize(std::max(1u, fw * pct / 100), std::max(1u, fh * pct / 100), m_smooth_);
    });
    on("im_anchor", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        m_anchor_ = std::clamp(arg_int(a, 0, 4), 0, 8);
        dirty("im_anchor");
    });
    on("im_canvas", [this, number](Rml::Event&, const Rml::VariantList&) {
        const u32 w = number(m_cw_), h = number(m_ch_);
        if (!w || !h) say("Ширина и высота холста — целые числа от 1");
        else canvas(w, h, static_cast<assets::Anchor>(m_anchor_));
    });
    on("im_trim", [this](Rml::Event&, const Rml::VariantList&) { trim(); });
    on("im_grid", [this, number](Rml::Event&, const Rml::VariantList&) {
        const u32 w = number(m_gw_), h = number(m_gh_), c = number(m_gc_);
        if (!w || !h || !c) say("Кадр и столбцы — целые числа от 1");
        else grid_to_strip(w, h, c);
    });
    on("im_effect", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        effect_ = arg_str(a, 0);
        params_[0] = effect_ == "alpha" ? 128 : effect_ == "reduce" ? 8 : effect_ == "hue" ? 120 : 0;
        params_[1] = params_[2] = 0;
        effect_from_panel();
    });
    on("im_param", [this, arg_int](Rml::Event& ev, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i < 0 || i > 2) return;
        const Rml::String v = ev.GetParameter<Rml::String>("value", "");
        params_[i] = std::atoi(v.c_str());
        effect_from_panel();
    });
    on("im_apply", [this](Rml::Event&, const Rml::VariantList&) { apply_effect(); });
    on("im_cancel", [this](Rml::Event&, const Rml::VariantList&) { cancel_effect(); });
    on("im_save", [this](Rml::Event&, const Rml::VariantList&) { save(); });
    on("im_save_as", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { save_as(arg_str(a, 0)); });
    on("im_save_as_jpg", [this](Rml::Event&, const Rml::VariantList&) { save_as(".jpg", true); });
    on("im_take_disk", [this](Rml::Event&, const Rml::VariantList&) { take_disk(); });
    on("im_keep_mine", [this](Rml::Event&, const Rml::VariantList&) { keep_mine(); });
    on("im_zoom_in", [this](Rml::Event&, const Rml::VariantList&) {
        for (u32 z : kZooms)
            if (z > zoom()) {
                set_zoom(z);
                break;
            }
    });
    on("im_zoom_out", [this](Rml::Event&, const Rml::VariantList&) {
        u32 to = 1;
        for (u32 z : kZooms)
            if (z < zoom()) to = z;
        set_zoom(to);
    });
    on("im_one", [this](Rml::Event&, const Rml::VariantList&) { set_zoom(1); });
    on("im_fit", [this](Rml::Event&, const Rml::VariantList&) { fit(); });
}

// --- the picture open ------------------------------------------------------------------------------------------

ImageEditor::Picture* ImageEditor::find(const Guid& id) const {
    for (const auto& p : pictures_)
        if (p->id == id) return p.get();
    return nullptr;
}

std::filesystem::path ImageEditor::file() const { return current_ ? current_->file : fs::path(); }
std::string ImageEditor::rel() const { return current_ ? current_->rel : std::string(); }
Guid ImageEditor::asset_id() const { return current_ ? current_->id : Guid{}; }

const CookedTexture& ImageEditor::image() const {
    static const CookedTexture none;
    return current_ ? current_->image : none;
}

const CookedTexture& ImageEditor::shown() const { return trying_ ? tried_ : image(); }

bool ImageEditor::dirty() const { return current_ && current_->unsaved(); }

std::vector<std::string> ImageEditor::unsaved() const {
    std::vector<std::string> out;
    for (const auto& p : pictures_)
        if (p->unsaved()) out.push_back("Изображения: «" + p->rel + "» не сохранена (при закрытии правки теряются)");
    return out;
}

editor::UndoStack& ImageEditor::history() { return current_ ? current_->history : empty_history_; }

bool ImageEditor::read_disk(Picture& p, std::vector<u8>& bytes, CookedTexture& image, std::string& why) const {
    if (!read_file(p.file, bytes)) {
        why = "файл не читается: " + path_to_utf8(p.file);
        return false;
    }
    if (!assets::decode_image(bytes, image, &why)) {
        why = "это не картинка: " + why;
        return false;
    }
    return true;
}

bool ImageEditor::open(const fs::path& file) {
    const assets::AssetRecord* r = assets_.record_of(file);
    std::error_code ec;
    if (!r && !fs::is_regular_file(file, ec)) {
        say("Нет такой картинки в «Ресурсах»: " + path_to_utf8(file));
        return false;
    }
    // A file of the game's folder is not of «Ресурсы»: it is shown only.
    if (!r && !game_dir.empty()) {
        const fs::path in_game = fs::weakly_canonical(file, ec).lexically_relative(fs::weakly_canonical(game_dir, ec));
        if (!in_game.empty() && *in_game.begin() != "..") return open_game(path_to_utf8(in_game));
    }
    if (!r) {
        const fs::path in_assets = fs::weakly_canonical(file, ec).lexically_relative(fs::weakly_canonical(assets_.root(), ec));
        if (in_assets.empty() || *in_assets.begin() == "..") {
            say("Не картинка «Ресурсов» и не картинка игры: " + path_to_utf8(file));
            return false;
        }
    }
    if (r && r->type != "image") {
        say("«" + r->path + "» — не картинка");
        return false;
    }
    cancel_effect();
    if (stroking_) release();
    Picture* p = r ? find(r->id) : nullptr;
    if (!p)
        for (const auto& q : pictures_)
            if (fs::equivalent(q->file, file, ec)) p = q.get();
    if (!p) {
        auto made = std::make_unique<Picture>();
        made->file = file;
        made->rel = r ? r->path : path_to_utf8(file.filename());
        if (r) made->id = r->id;
        made->ext = extension_of(file);
        std::vector<u8> bytes;
        std::string why;
        if (!read_disk(*made, bytes, made->image, why)) {
            say("«" + made->rel + "» не открывается: " + why);
            return false;
        }
        made->disk_hash = editor::sources::hash_of(bytes);
        made->history.clear();
        p = made.get();
        pictures_.push_back(std::move(made));
        FORGE_INFO("Изображения: открыта «%s» (%s, %s)", p->rel.c_str(), size_words(p->image).c_str(), format_of(p->ext).c_str());
    }
    current_ = p;
    have_last_ = false;
    warning_.clear();
    jpg_asked_.clear();
    note_.clear();
    if (frames() > 1 && p->area >= static_cast<int>(frames())) p->area = -1;
    colors_ = assets::picture_colors(p->image);
    list_version_ = ~0ull;
    touched();
    return true;
}

bool ImageEditor::open_game(const std::string& game_file_in) {
    std::string rel = game_file_in;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (game_dir.empty() || rel.rfind("pictures/", 0) != 0 || rel.find("/../") != std::string::npos || !picture_extension(extension_of(utf8_path(rel)))) {
        say("Не картинка игры: «" + rel + "» (картинки игры — в её папке pictures)");
        return false;
    }
    const fs::path file = game_dir / utf8_path(rel);
    cancel_effect();
    if (stroking_) release();
    Picture* p = nullptr;
    for (const auto& q : pictures_)
        if (q->view_only && q->game_file == rel) p = q.get();
    if (!p) {
        auto made = std::make_unique<Picture>();
        made->view_only = true;
        made->game_file = rel;
        made->file = file;
        made->rel = rel;
        made->ext = extension_of(file);
        std::vector<u8> bytes;
        std::string why;
        if (!read_disk(*made, bytes, made->image, why)) {
            say("«" + rel + "» не открывается: " + why);
            return false;
        }
        made->disk_hash = editor::sources::hash_of(bytes);
        made->copy_of = copy_from ? copy_from(rel) : std::string();
        p = made.get();
        pictures_.push_back(std::move(made));
        FORGE_INFO("Изображения: показана картинка игры «%s» (%s, %s) — только просмотр", rel.c_str(), size_words(p->image).c_str(),
                   format_of(p->ext).c_str());
    }
    current_ = p;
    have_last_ = false;
    warning_.clear();
    jpg_asked_.clear();
    note_.clear();
    if (frames() > 1 && p->area >= static_cast<int>(frames())) p->area = -1;
    colors_ = assets::picture_colors(p->image);
    list_version_ = ~0ull;
    touched();
    return true;
}

bool ImageEditor::view_only() const { return current_ && current_->view_only; }
std::string ImageEditor::game_file() const { return current_ && current_->view_only ? current_->game_file : std::string(); }
std::string ImageEditor::copy_of() const { return current_ && current_->view_only ? current_->copy_of : std::string(); }
bool ImageEditor::missing() const { return current_ && current_->missing; }

std::string ImageEditor::view_reason(const Picture& p) const {
    if (!p.copy_of.empty())
        return "«" + p.game_file + "» — копия «" + p.copy_of + "» из «Ресурсов». Правьте «" + p.copy_of + "»: копия в игре обновится сама";
    return "«" + p.game_file + "» есть только в игре, в «Ресурсах» его нет. Вкладка пишет картинки «Ресурсов», а игра получает их "
           "копии через sources.json; файл, записанный прямо в игру, не был бы ничьей копией и разошёлся бы с ними";
}

bool ImageEditor::editable(const std::string& what) {
    if (!current_) return false;
    if (!current_->view_only) return true;
    say(what + " — только просмотр: " + view_reason(*current_));
    return false;
}

std::vector<std::string> ImageEditor::game_pictures() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path root = game_dir / "pictures";
    if (game_dir.empty() || !fs::is_directory(root, ec)) return out;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !picture_extension(extension_of(it->path()))) continue;
        const std::string rel = "pictures/" + path_to_utf8(it->path().lexically_relative(root));
        if (copy_from && !copy_from(rel).empty()) continue; // a copy of «Ресурсы»: its picture there is the one to open
        out.push_back(rel);
    }
    return out;
}

// --- history ---------------------------------------------------------------------------------------------------

bool ImageEditor::commit(Picture& p, const CookedTexture& before, PixelRect changed, std::string label, u32 frames_before) {
    if (p.view_only) {
        p.image = before; // (nothing changes it: the actions refuse first)
        return false;
    }
    PixelStep::Side b, a;
    b.w = before.width;
    b.h = before.height;
    a.w = p.image.width;
    a.h = p.image.height;
    b.frames = frames_before ? frames_before : p.own_frames;
    a.frames = p.own_frames;
    if (b.w != a.w || b.h != a.h || b.frames != a.frames) {
        b.rect = assets::whole_rect(before);
        b.pixels = before.rgba8;
        a.rect = assets::whole_rect(p.image);
        a.pixels = p.image.rgba8;
    } else {
        if (changed.empty()) return false;
        b.rect = a.rect = changed;
        b.pixels = assets::copy_rect(before, changed);
        a.pixels = assets::copy_rect(p.image, changed);
        if (b.pixels == a.pixels) return false;
    }
    p.history.execute(std::make_unique<PixelStep>(p, [this] { touched(); }, std::move(b), std::move(a), std::move(label)));
    p.history.seal();
    touched();
    return true;
}

void ImageEditor::undo() {
    if (!current_) return;
    // Ctrl+Z while the picker is open puts its colour back (as «Отмена»): the picture's history is not touched.
    if (picking()) {
        cp_->close(false);
        return;
    }
    if (!editable("Отменять нечего")) return;
    cancel_effect();
    if (stroking_) release();
    if (current_->history.undo()) FORGE_INFO("Отменено");
    have_last_ = false;
}

void ImageEditor::redo() {
    if (!current_) return;
    if (picking()) {
        cp_->close(true);
        return;
    }
    if (!editable("Повторять нечего")) return;
    cancel_effect();
    if (current_->history.redo()) FORGE_INFO("Повторено");
    have_last_ = false;
}

void ImageEditor::touched() {
    canvas_wanted_ = true;
    if (current_ && current_->area >= 0 && assets::frame_rect(current_->image, frames(), static_cast<u32>(current_->area)).empty())
        current_->area = -1;
    rebuild_panels();
}

std::string ImageEditor::status() const {
    if (!current_) return "Изображения: выберите картинку слева или «Открыть в редакторе изображений» в «Ресурсах»";
    std::string s = current_->view_only ? "Изображения: игра «" + current_->game_file + "» (только просмотр) · " + size_words(current_->image) + " · " +
                                              format_of(current_->ext)
                                        : "Изображения: «" + current_->rel + "» · " + size_words(current_->image) + " · " + format_of(current_->ext);
    if (frames() > 1) s += " · " + count_words(frames(), "кадр", "кадра", "кадров");
    if (current_->unsaved()) s += " · не сохранена";
    if (current_->outside) s += " · файл изменён снаружи";
    if (current_->missing) s += current_->view_only ? " · файла больше нет в игре" : " · файла нет";
    if (!current_->copy_of.empty()) s += " · теперь копия «" + current_->copy_of + "»";
    return s;
}

void ImageEditor::say(std::string text) {
    note_ = std::move(text);
    if (!note_.empty()) FORGE_INFO("Изображения: %s", note_.c_str());
    if (m_note_ != note_) {
        m_note_ = note_;
        dirty("im_note");
    }
}

// --- the pixel editor -------------------------------------------------------------------------------------------

void ImageEditor::set_tool(const std::string& tool) {
    if (tool != "pencil" && tool != "eraser" && tool != "fill" && tool != "picker") return;
    tool_ = tool;
    rebuild_panels();
}

void ImageEditor::set_size(u32 px) {
    size_ = std::clamp(px, 1u, 4u);
    rebuild_panels();
}

bool ImageEditor::set_color(const std::string& hex, bool secondary) {
    const std::optional<editor::design::Color> c = editor::design::parse_hex_input(hex);
    if (!c) {
        say("«" + hex + "» — не цвет: " + editor::design::hex_problem(hex));
        return false;
    }
    (secondary ? second_ : first_) = rgba_of(*c);
    (secondary ? second_theme_ : first_theme_).clear();
    rebuild_panels();
    return true;
}

void ImageEditor::swap_colors() {
    if (picking()) return; // the picker has the keys
    std::swap(first_, second_);
    std::swap(first_theme_, second_theme_);
    rebuild_panels();
}

// --- the colour picker (13.4) on the brush's colours ------------------------------------------------------------------

bool ImageEditor::open_color(bool secondary, f32 x, f32 y, f32 w, f32 h) {
    if (!cp_) return false;
    if (picking()) cp_->close(true);
    if (stroking_) release();
    picking_second_ = secondary;
    picking_was_ = secondary ? second_ : first_;
    picking_was_theme_ = secondary ? second_theme_ : first_theme_;
    ColorPicker::Request r;
    r.field = secondary ? "image.right" : "image.left";
    r.start = color_of(picking_was_);
    r.linkable = false; // the picture keeps RGBA: a game colour is copied
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return cp_->open(*this, r, context_);
}

void ImageEditor::picker_show(editor::design::Color color, const std::string& link) {
    (void)link; // never followed: the brush keeps the colour's RGBA
    Rgba& c = picking_second_ ? second_ : first_;
    std::string& theme = picking_second_ ? second_theme_ : first_theme_;
    c = rgba_of(color);
    theme.clear();
    if (cp_ && !cp_->theme_key().empty() && cp_->game_colors)
        for (const editor::design::NamedColor& g : cp_->game_colors())
            if (g.key == cp_->theme_key() && g.color == color) theme = g.name;
    rebuild_panels();
}

void ImageEditor::picker_closed(bool keep, bool live) {
    (void)live;
    if (!keep) {
        (picking_second_ ? second_ : first_) = picking_was_;
        (picking_second_ ? second_theme_ : first_theme_) = picking_was_theme_;
    }
    rebuild_panels();
}

bool ImageEditor::picker_sample(f32 mx, f32 my) {
    if (!current_ || canvas_w_ == 0 || mx < view_x_ || my < view_y_ || mx >= view_x_ + static_cast<f32>(canvas_w_) ||
        my >= view_y_ + static_cast<f32>(canvas_h_))
        return false;
    // Not where the picker itself stands over the canvas.
    if (context_)
        for (int i = 0; i < context_->GetNumDocuments(); ++i)
            if (Rml::Element* p = context_->GetDocument(i)->GetElementById("ue-picker"); p && p->IsVisible(true)) {
                const Rml::Vector2f at = p->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f size = p->GetBox().GetSize(Rml::BoxArea::Border);
                if (mx >= at.x && my >= at.y && mx < at.x + size.x && my < at.y + size.y) return false;
            }
    i32 x = -1, y = -1;
    const CookedTexture& img = shown();
    if (!to_pixel(mx, my, x, y) || x < 0 || y < 0 || static_cast<u32>(x) >= img.width || static_cast<u32>(y) >= img.height) return false;
    const Rgba c = assets::pixel_at(img, static_cast<u32>(x), static_cast<u32>(y));
    cp_->take(color_of(c), "Взят пиксель " + std::to_string(x) + ", " + std::to_string(y) + " картинки: " + hex_of(c) +
                               (c.a == 0 ? " (прозрачный)" : c.a < 255 ? " (с прозрачностью)" : "") + ".");
    return true;
}

std::string ImageEditor::picker_title() const { return picking_second_ ? "Цвет правой кнопки" : "Цвет левой кнопки"; }

std::string ImageEditor::picker_note() const {
    std::string note = "Цвет кисти. Картинка хранит пиксели RGBA: цвет игры копируется в кисть как есть, и если потом поменять его в "
                       "«Компонентах», уже нарисованные пиксели не перекрасятся. Рисунок, файл и Ctrl+Z от выбора цвета не меняются.";
    const std::string& theme = picking_second_ ? second_theme_ : first_theme_;
    if (!theme.empty()) note = "Скопирован цвет игры «" + theme + "». " + note;
    return note;
}

std::string ImageEditor::picker_dropper_hint(bool live) const {
    (void)live;
    return "Пипетка: щёлкните по картинке на холсте — возьмётся цвет её пикселя, с прозрачностью. Щелчок мимо картинки, правая кнопка "
           "или Esc — убрать пипетку.";
}

u32 ImageEditor::frames() const {
    if (!current_) return 1;
    if (current_->view_only && users_of_file)
        for (const User& u : users_of_file(current_->game_file))
            if (u.frames > 0) return u.frames;
    if (users_of && !current_->id.is_null())
        for (const User& u : users_of(current_->id))
            if (u.frames > 0) return u.frames;
    return std::max(1u, current_->own_frames);
}

std::string ImageEditor::frames_from() const {
    if (!current_) return {};
    if ((users_of && !current_->id.is_null()) || (current_->view_only && users_of_file)) {
        const std::vector<User> users = current_->view_only ? users_of_file(current_->game_file) : users_of(current_->id);
        if (!users.empty()) {
            std::string s = "«Кадров в картинке» шаблона «" + users.front().name + "»";
            for (usize i = 1; i < users.size(); ++i)
                if (users[i].frames != users.front().frames)
                    s += "; у «" + users[i].name + "» — " + std::to_string(users[i].frames);
            return s;
        }
    }
    return "своё число вкладки: картинку не рисует ни один шаблон";
}

bool ImageEditor::set_frames(u32 n) {
    if (!current_ || n < 1) return false;
    if (const u32 limit = frame_limit ? frame_limit() : 64; n > limit) {
        say("Кадров не больше " + std::to_string(limit) + ": это предел игры (поле «Предел кадров» в «Пиксельных»)");
        return false;
    }
    if ((current_->view_only && users_of_file && !users_of_file(current_->game_file).empty()) ||
        (users_of && !current_->id.is_null() && !users_of(current_->id).empty())) {
        say("Число кадров задаёт «Кадров в картинке» шаблона в «Объектах»: " + frames_from());
        return false;
    }
    current_->own_frames = n;
    if (current_->image.width % n != 0) say("Ширина " + std::to_string(current_->image.width) + " не делится на " + std::to_string(n) + ": кадры не выделить");
    else say("");
    current_->area = -1;
    touched();
    return true;
}

int ImageEditor::area() const { return current_ ? current_->area : -1; }

PixelRect ImageEditor::area_rect() const {
    if (!current_) return {};
    if (current_->area >= 0) {
        const PixelRect r = assets::frame_rect(current_->image, frames(), static_cast<u32>(current_->area));
        if (!r.empty()) return r;
    }
    return assets::whole_rect(current_->image);
}

bool ImageEditor::set_area(int frame) {
    if (!current_) return false;
    if (frame >= 0 && assets::frame_rect(current_->image, frames(), static_cast<u32>(frame)).empty()) {
        say("Кадра " + std::to_string(frame + 1) + " нет: ширина " + std::to_string(current_->image.width) + ", кадров " + std::to_string(frames()));
        return false;
    }
    cancel_effect();
    current_->area = frame < 0 ? -1 : frame;
    have_last_ = false;
    touched();
    return true;
}

void ImageEditor::press(i32 x, i32 y, bool secondary, bool shift, bool alt) {
    Picture* p = current_;
    if (!p || stroking_) return;
    const std::string tool = alt ? std::string("picker") : tool_;
    if (tool == "picker") {
        const CookedTexture& img = shown();
        if (x < 0 || y < 0 || static_cast<u32>(x) >= img.width || static_cast<u32>(y) >= img.height) return;
        (secondary ? second_ : first_) = assets::pixel_at(img, static_cast<u32>(x), static_cast<u32>(y));
        (secondary ? second_theme_ : first_theme_).clear();
        rebuild_panels();
        return;
    }
    if (!editable(tool == "fill" ? "Заливка" : tool == "eraser" ? "Ластик" : "Карандаш")) return;
    if (trying_) {
        say("Сначала «Применить» или «Отмена» эффекта");
        return;
    }
    if (static_cast<u64>(p->image.width) * p->image.height > kDrawPixels) {
        say("Рисовать можно на картинке до 4096 × 4096; у этой " + size_words(p->image) + " — для неё трансформации, эффекты и формат");
        return;
    }
    const PixelRect area = area_rect();
    const Rgba colour = tool == "eraser" ? Rgba{0, 0, 0, 0} : secondary ? second_ : first_;
    if (tool == "fill") {
        if (x < 0 || y < 0 || !area.contains(x, y)) return;
        const CookedTexture before = p->image;
        const PixelRect changed = assets::fill(p->image, static_cast<u32>(x), static_cast<u32>(y), colour, area);
        commit(*p, before, changed, "Заливка " + hex_of(colour) + (p->area >= 0 ? ": кадр " + std::to_string(p->area + 1) : std::string()));
        last_x_ = x;
        last_y_ = y;
        have_last_ = true;
        return;
    }
    stroking_ = true;
    stroke_second_ = secondary;
    stroke_before_ = p->image;
    stroke_label_ = (tool == "eraser" ? std::string("Ластик") : "Карандаш " + hex_of(colour)) +
                    (p->area >= 0 ? ": кадр " + std::to_string(p->area + 1) : std::string());
    stroke_rect_ = shift && have_last_ ? assets::paint_line(p->image, last_x_, last_y_, x, y, size_, colour, area)
                                       : assets::paint(p->image, x, y, size_, colour, area);
    last_x_ = x;
    last_y_ = y;
    have_last_ = true;
    canvas_wanted_ = true;
}

void ImageEditor::drag(i32 x, i32 y) {
    if (!stroking_ || !current_ || (x == last_x_ && y == last_y_)) return;
    const Rgba colour = tool_ == "eraser" ? Rgba{0, 0, 0, 0} : stroke_second_ ? second_ : first_;
    stroke_rect_ = assets::united(stroke_rect_, assets::paint_line(current_->image, last_x_, last_y_, x, y, size_, colour, area_rect()));
    last_x_ = x;
    last_y_ = y;
    canvas_wanted_ = true;
}

void ImageEditor::release() {
    if (!stroking_) return;
    stroking_ = false;
    if (current_) commit(*current_, stroke_before_, stroke_rect_, stroke_label_);
    stroke_before_ = {};
    stroke_rect_ = {};
}

// --- transformations ---------------------------------------------------------------------------------------------

bool ImageEditor::turn(assets::Turn t) {
    Picture* p = current_;
    if (!p || !editable(t == assets::Turn::FlipH || t == assets::Turn::FlipV ? "Отразить" : "Повернуть")) return false;
    cancel_effect();
    const char* words = t == assets::Turn::Cw ? "Повернуть вправо" : t == assets::Turn::Ccw ? "Повернуть влево" : t == assets::Turn::Half ? "Повернуть на 180°"
                        : t == assets::Turn::FlipH ? "Отразить по горизонтали" : "Отразить по вертикали";
    const CookedTexture before = p->image;
    const u32 n = frames();
    std::string why;
    if (p->area >= 0) {
        if (!assets::turn_area(p->image, area_rect(), t, &why)) {
            say(why);
            return false;
        }
        say("");
        return commit(*p, before, area_rect(), std::string(words) + ": кадр " + std::to_string(p->area + 1));
    }
    if (n > 1 && p->image.width % n == 0) {
        // Each frame by itself: the strip stays a strip, its frames in their places.
        for (u32 i = 0; i < n; ++i)
            if (!assets::turn_area(p->image, assets::frame_rect(p->image, n, i), t, &why)) {
                p->image = before;
                say("Кадры " + std::to_string(p->image.width / n) + " × " + std::to_string(p->image.height) +
                    " не квадратные: на 90° поворачивается только картинка из одного кадра, а кадры полосы — на 180° и отражением");
                return false;
            }
        say("");
        return commit(*p, before, assets::whole_rect(p->image), std::string(words) + ": " + count_words(n, "кадр", "кадра", "кадров"));
    }
    p->image = assets::turned(before, t);
    say("");
    return commit(*p, before, assets::whole_rect(p->image), std::string(words) + ": вся картинка");
}

namespace {
// Why a change of size is not for one frame of a strip.
const char* kWholeOnly = "Размер меняется у всей картинки: кадр полосы не может стать другого размера, чем соседние. Выберите «Вся картинка»";
} // namespace

bool ImageEditor::resize(u32 width, u32 height, bool smooth) {
    Picture* p = current_;
    if (!p || !editable("Размер")) return false;
    cancel_effect();
    if (p->area >= 0) {
        say(kWholeOnly);
        return false;
    }
    const u32 n = frames() > 1 && p->image.width % frames() == 0 ? frames() : 1;
    if (static_cast<u64>(width) * n * height > kMaxPixels || width * n > 16384 || height > 16384) {
        say("Слишком большая: до 8192 × 8192 пикселей всего");
        return false;
    }
    const CookedTexture before = p->image;
    CookedTexture out;
    std::string why;
    if (!assets::each_frame(before, n, [&](const CookedTexture& f) { return assets::resized(f, width, height, smooth); }, out, &why)) {
        say(why);
        return false;
    }
    p->image = std::move(out);
    say("");
    return commit(*p, before, {}, "Размер " + std::to_string(width) + " × " + std::to_string(height) + (n > 1 ? " у каждого кадра" : "") +
                                     (smooth ? ", сглаживание" : ", ближайший пиксель"));
}

bool ImageEditor::canvas(u32 width, u32 height, assets::Anchor anchor) {
    Picture* p = current_;
    if (!p || !editable("Холст")) return false;
    cancel_effect();
    if (p->area >= 0) {
        say(kWholeOnly);
        return false;
    }
    const u32 n = frames() > 1 && p->image.width % frames() == 0 ? frames() : 1;
    if (static_cast<u64>(width) * n * height > kMaxPixels || width * n > 16384 || height > 16384) {
        say("Слишком большая: до 8192 × 8192 пикселей всего");
        return false;
    }
    const CookedTexture before = p->image;
    CookedTexture out;
    std::string why;
    if (!assets::each_frame(before, n, [&](const CookedTexture& f) { return assets::with_canvas(f, width, height, anchor); }, out, &why)) {
        say(why);
        return false;
    }
    p->image = std::move(out);
    say("");
    return commit(*p, before, {}, "Холст " + std::to_string(width) + " × " + std::to_string(height) + (n > 1 ? " у каждого кадра" : ""));
}

bool ImageEditor::trim() {
    Picture* p = current_;
    if (!p || !editable("Обрезать")) return false;
    cancel_effect();
    if (p->area >= 0) {
        say(kWholeOnly);
        return false;
    }
    const u32 n = frames() > 1 && p->image.width % frames() == 0 ? frames() : 1;
    const u32 fw = p->image.width / n;
    // One rectangle for every frame (relative to its frame): the frames keep one size and the figure its place.
    PixelRect keep;
    for (u32 i = 0; i < n; ++i) {
        PixelRect b = assets::opaque_bounds(p->image, assets::frame_rect(p->image, n, i));
        if (b.empty()) continue;
        b.x -= i * fw;
        keep = assets::united(keep, b);
    }
    if (keep.empty()) {
        say("Картинка вся прозрачная: обрезать нечего");
        return false;
    }
    if (keep.w == fw && keep.h == p->image.height) {
        say("Прозрачных краёв нет: обрезать нечего");
        return false;
    }
    const CookedTexture before = p->image;
    CookedTexture out;
    std::string why;
    if (!assets::each_frame(before, n, [&](const CookedTexture& f) { return assets::cropped(f, keep); }, out, &why)) {
        say(why);
        return false;
    }
    p->image = std::move(out);
    say("");
    return commit(*p, before, {}, "Обрезать по непрозрачному: " + std::to_string(keep.w) + " × " + std::to_string(keep.h));
}

bool ImageEditor::grid_to_strip(u32 frame_w, u32 frame_h, u32 columns) {
    Picture* p = current_;
    if (!p || !editable("Сетка кадров")) return false;
    cancel_effect();
    if (p->area >= 0) {
        say("«Сетка кадров» делает полосу из всей картинки: выберите «Вся картинка»");
        return false;
    }
    const CookedTexture before = p->image;
    CookedTexture out;
    u32 n = 0;
    std::string why;
    if (!assets::sheet_to_strip(before, frame_w, frame_h, columns, out, &n, &why)) {
        say(why);
        return false;
    }
    const u32 frames_before = p->own_frames;
    p->image = std::move(out);
    p->own_frames = n;
    p->area = -1;
    std::string said = "Полоса из " + count_words(n, "кадра", "кадров", "кадров") + " " + std::to_string(frame_w) + " × " + std::to_string(frame_h) +
                       ", по порядку: ряды листа сверху вниз, в ряду слева направо";
    const std::vector<User> users = users_of && !p->id.is_null() ? users_of(p->id) : std::vector<User>{};
    if (users.empty())
        said += ". Шаблону с этой картинкой поставьте «Кадров в картинке» " + std::to_string(n) + " в «Объектах»";
    else if (users.front().frames != n)
        said += ". Шаблон «" + users.front().name + "» читает её как " + count_words(users.front().frames, "кадр", "кадра", "кадров") +
                ": поставьте «Кадров в картинке» " + std::to_string(n) + " в «Объектах»";
    say(said);
    return commit(*p, before, {}, "Сетка кадров " + std::to_string(frame_w) + " × " + std::to_string(frame_h) + " в полосу: " +
                                     count_words(n, "кадр", "кадра", "кадров"), frames_before);
}

// --- effects -------------------------------------------------------------------------------------------------------

void ImageEditor::effect_from_panel() {
    assets::Effect e;
    if (effect_ == "light") e.kind = assets::EffectKind::BrightnessContrast;
    else if (effect_ == "hue") e.kind = assets::EffectKind::HueSaturation;
    else if (effect_ == "invert") e.kind = assets::EffectKind::Invert;
    else if (effect_ == "gray") e.kind = assets::EffectKind::Grayscale;
    else if (effect_ == "outline") e.kind = assets::EffectKind::Outline;
    else if (effect_ == "alpha") e.kind = assets::EffectKind::AlphaThreshold;
    else if (effect_ == "reduce") e.kind = assets::EffectKind::ReduceColors;
    else if (effect_ == "replace") e.kind = assets::EffectKind::ReplaceColor;
    else return;
    e.a = params_[0];
    e.b = params_[1];
    e.c = params_[2];
    e.color = first_;
    e.to = second_;
    try_effect(e);
}

bool ImageEditor::try_effect(const assets::Effect& effect) {
    if (!current_ || !editable("Эффект")) return false;
    if (stroking_) release();
    tried_ = current_->image;
    tried_changed_ = assets::apply_effect(tried_, area_rect(), effect);
    tried_effect_ = effect;
    trying_ = true;
    canvas_wanted_ = true;
    rebuild_panels();
    return true;
}

bool ImageEditor::apply_effect() {
    Picture* p = current_;
    if (!p || !editable("Применить эффект") || !trying_) return false;
    static const char* names[] = {"Яркость и контраст", "Тон, насыщенность, светлота", "Инверсия", "Оттенки серого", "Контур",
                                  "Порог альфы", "Меньше цветов", "Замена цвета"};
    std::string label = names[static_cast<int>(tried_effect_.kind)];
    const assets::Effect& e = tried_effect_;
    switch (e.kind) {
    case assets::EffectKind::BrightnessContrast: label += " " + std::to_string(e.a) + ", " + std::to_string(e.b); break;
    case assets::EffectKind::HueSaturation:
        label = "Тон " + std::string(e.a > 0 ? "+" : "") + std::to_string(e.a) + ", насыщенность " + std::to_string(e.b) + ", светлота " + std::to_string(e.c);
        break;
    case assets::EffectKind::AlphaThreshold: label += " " + std::to_string(e.a); break;
    case assets::EffectKind::ReduceColors: label += ": " + std::to_string(e.a); break;
    case assets::EffectKind::Outline: label += " " + hex_of(e.color); break;
    case assets::EffectKind::ReplaceColor: label += " " + hex_of(e.color) + " → " + hex_of(e.to); break;
    default: break;
    }
    if (p->area >= 0) label += ": кадр " + std::to_string(p->area + 1);
    const CookedTexture before = p->image;
    p->image = std::move(tried_);
    tried_ = {};
    trying_ = false;
    if (!commit(*p, before, tried_changed_, label)) {
        say("Эффект ничего не изменил");
        touched();
        return false;
    }
    say("");
    return true;
}

void ImageEditor::cancel_effect() {
    if (!trying_) return;
    trying_ = false;
    tried_ = {};
    canvas_wanted_ = true;
    rebuild_panels();
}

// --- saving ----------------------------------------------------------------------------------------------------------

bool ImageEditor::write_picture(Picture& p, const fs::path& to, const std::string& extension, std::string& why) {
    std::vector<u8> bytes;
    if (!assets::encode_image(p.image, extension, bytes)) {
        why = "картинка не кодируется в " + format_of(extension);
        return false;
    }
    if (!write_file || !write_file(to, bytes)) {
        why = "файл не записан: " + path_to_utf8(to);
        return false;
    }
    p.disk_hash = editor::sources::hash_of(bytes);
    return true;
}

bool ImageEditor::save() {
    Picture* p = current_;
    if (!p) return false;
    if (picking()) cp_->close(true); // the colour as picked; the picture's file does not depend on it
    if (!editable("Не сохранено")) return false;
    if (stroking_) release();
    if (p->missing) {
        say("Файла «" + p->rel + "» больше нет в «Ресурсах»: «Сохранить как…» запишет картинку новым файлом");
        return false;
    }
    std::vector<u8> disk;
    const bool read = read_file(p->file, disk);
    // Nothing changed here since the file was read or written (Ctrl+Z back to that state too): writing it again would
    // encode it again — a JPEG loses each time — and have the game's copies looked at for nothing. A file another
    // program changed since is taken, as F5 takes it.
    if (!p->unsaved()) {
        if (read && editor::sources::hash_of(disk) != p->disk_hash) {
            files_changed();
            return true;
        }
        say("Нечего сохранять: «" + p->rel + "» как на диске, файл не перезаписан");
        return true;
    }
    if (!assets::can_encode_image(p->ext)) {
        say(format_of(p->ext) + " вкладка читает, но не пишет: «Сохранить как…» PNG, WebP, TGA, BMP или JPG");
        return false;
    }
    if (read && editor::sources::hash_of(disk) != p->disk_hash) {
        p->outside = true;
        say("Файл «" + p->rel + "» изменён другой программой после того, как открыт здесь. Не записан: выберите «Взять с диска» или «Оставить мои», "
            "или «Сохранить как…»");
        rebuild_panels();
        return false;
    }
    std::string why;
    if (!write_picture(*p, p->file, p->ext, why)) {
        say("Не сохранено: " + why + ". Файл, его копии в игре и правки здесь — как были");
        FORGE_ERROR("Изображения: «%s» не сохранена: %s", p->rel.c_str(), why.c_str());
        return false;
    }
    p->history.mark_saved();
    p->outside = p->kept = false;
    say("Сохранено: «" + p->rel + "» (" + size_words(p->image) + ", " + format_of(p->ext) + ")");
    list_version_ = ~0ull;
    rebuild_panels();
    if (written) written();
    return true;
}

bool ImageEditor::save_all(std::string& error) {
    Picture* was = current_;
    bool ok = true;
    for (const auto& p : pictures_) {
        if (p->view_only || !p->unsaved()) continue;
        current_ = p.get();
        if (!save()) {
            if (ok) error = "картинка «" + p->rel + "» не записана: " + note_;
            ok = false;
        }
    }
    current_ = was;
    touched();
    return ok;
}

bool ImageEditor::save_as(const std::string& extension_in, bool confirm) {
    Picture* p = current_;
    const std::string extension = lower(extension_in);
    if (!p || !editable("Не сохранено как " + format_of(extension)) || !assets::can_encode_image(extension)) return false;
    if (stroking_) release();
    cancel_effect();
    const bool jpg = extension == ".jpg" || extension == ".jpeg";
    if (jpg && !confirm) {
        usize clear = 0;
        for (usize i = 3; i < p->image.rgba8.size(); i += 4) clear += p->image.rgba8[i] != 255;
        if (clear > 0) {
            warning_ = "В JPG нет прозрачности: " + count_words(static_cast<u32>(std::min<usize>(clear, 0xffffffffu)), "пиксель", "пикселя", "пикселей") +
                       " не непрозрачны и станут непрозрачными. JPG ещё и сжимает с потерями. «Всё равно в JPG» или PNG, WebP, TGA";
            jpg_asked_ = p->rel;
            say(warning_);
            rebuild_panels();
            return false;
        }
    }
    std::vector<u8> bytes;
    if (!assets::encode_image(p->image, extension, bytes)) {
        say("Картинка не кодируется в " + format_of(extension));
        return false;
    }
    std::string stem = name_of(p->rel);
    const usize dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    const std::string made = assets_.add_file(folder_of(p->rel), stem + extension, bytes, "Сохранить как " + format_of(extension));
    if (made.empty()) {
        say("Не сохранено как " + format_of(extension) + ": файл не записан. «" + p->rel + "» и правки здесь — как были");
        return false;
    }
    warning_.clear();
    jpg_asked_.clear();
    // The tab goes on with the new file: a resource of its own (its id comes when «Ресурсы» sees it); the old file
    // stays as it was saved last, with what used it.
    const std::string from = p->rel;
    p->rel = made;
    p->file = assets_.abs(made);
    p->ext = extension;
    p->id = Guid{};
    p->disk_hash = editor::sources::hash_of(bytes);
    p->missing = p->outside = p->kept = false;
    p->history.mark_saved();
    say("Сохранено как «" + made + "» — новый ресурс; «" + from + "» и то, что его использует, как были");
    list_version_ = ~0ull;
    touched();
    if (written) written();
    return true;
}

bool ImageEditor::outside() const { return current_ && current_->outside; }

bool ImageEditor::take_disk() {
    Picture* p = current_;
    if (!p || !editable("«Взять с диска»")) return false;
    cancel_effect();
    std::vector<u8> bytes;
    CookedTexture disk;
    std::string why;
    if (!read_disk(*p, bytes, disk, why)) {
        say("С диска не взять: " + why);
        return false;
    }
    const CookedTexture before = p->image;
    p->image = std::move(disk);
    commit(*p, before, assets::whole_rect(p->image), "Взято с диска: «" + p->rel + "»");
    p->disk_hash = editor::sources::hash_of(bytes);
    p->history.mark_saved();
    p->outside = p->kept = false;
    say("«" + p->rel + "» — как на диске; Ctrl+Z вернёт правки, сделанные здесь");
    touched();
    return true;
}

bool ImageEditor::keep_mine() {
    Picture* p = current_;
    if (!p || !editable("«Оставить мои»") || !p->outside) return false;
    std::vector<u8> bytes;
    if (read_file(p->file, bytes)) p->disk_hash = editor::sources::hash_of(bytes);
    p->outside = false;
    p->kept = true; // Ctrl+Z back to the saved state is not the file any more: Ctrl+S still writes
    say("Ctrl+S запишет правки, сделанные здесь, поверх того, что на диске");
    rebuild_panels();
    return true;
}

void ImageEditor::files_changed() {
    game_files_known_ = false;
    for (const auto& up : pictures_) {
        Picture& p = *up;
        if (p.view_only) {
            // Shown only: the file as it is now, nothing to keep or ask about; its identity stays the game's file.
            const std::string copy = copy_from ? copy_from(p.game_file) : std::string();
            if (copy != p.copy_of) {
                if (!copy.empty())
                    FORGE_INFO("Изображения: «%s» теперь копия «%s» из «Ресурсов» — здесь по-прежнему только просмотр, правьте «%s»",
                               p.game_file.c_str(), copy.c_str(), copy.c_str());
                p.copy_of = copy;
            }
            std::vector<u8> bytes;
            std::error_code ec;
            if (!fs::is_regular_file(p.file, ec) || !read_file(p.file, bytes)) {
                if (!p.missing) FORGE_WARN("Изображения: файла игры «%s» больше нет — показана картинка, какой была", p.game_file.c_str());
                p.missing = true;
                continue;
            }
            const std::string hash = editor::sources::hash_of(bytes);
            if (hash == p.disk_hash && !p.missing) continue;
            CookedTexture now;
            if (!assets::decode_image(bytes, now)) {
                FORGE_WARN("Изображения: «%s» в игре не читается как картинка — показана, какой была", p.game_file.c_str());
                continue;
            }
            const bool back = p.missing;
            p.image = std::move(now);
            p.disk_hash = hash;
            p.missing = false;
            if (&p == current_) {
                if (p.area >= 0 && assets::frame_rect(p.image, frames(), static_cast<u32>(p.area)).empty()) p.area = -1;
                colors_of_ = nullptr; // its colours again
            }
            FORGE_INFO("Изображения: картинка игры «%s» %s — показана как сейчас (только просмотр)", p.game_file.c_str(),
                       back ? "снова на месте" : "изменена снаружи");
            continue;
        }
        const assets::AssetRecord* r = p.id.is_null() ? assets_.record_of(p.file) : assets_.find_id(p.id);
        if (r) {
            if (p.id.is_null()) p.id = r->id;
            if (r->path != p.rel) {
                FORGE_INFO("Изображения: «%s» теперь «%s»", p.rel.c_str(), r->path.c_str());
                p.rel = r->path;
                p.file = assets_.abs(r->path);
            }
            p.missing = false;
        } else if (!p.id.is_null()) {
            p.missing = true;
            continue;
        }
        std::vector<u8> bytes;
        if (!read_file(p.file, bytes)) continue;
        const std::string hash = editor::sources::hash_of(bytes);
        if (hash == p.disk_hash) continue;
        if (p.unsaved() || (&p == current_ && (trying_ || stroking_))) {
            if (!p.outside) FORGE_WARN("Изображения: файл «%s» изменён другой программой, а здесь есть несохранённые правки", p.rel.c_str());
            p.outside = true;
            continue;
        }
        // Nothing unsaved here: the file as it is now, a step (Ctrl+Z: as it was here).
        CookedTexture now;
        if (!assets::decode_image(bytes, now)) continue;
        const CookedTexture before = p.image;
        p.image = std::move(now);
        Picture* was = current_;
        current_ = &p;
        commit(p, before, assets::whole_rect(p.image), "Изменена снаружи: «" + p.rel + "»");
        current_ = was;
        p.disk_hash = hash;
        p.history.mark_saved();
        FORGE_INFO("Изображения: «%s» изменена другой программой — взята как есть (Ctrl+Z — как было здесь)", p.rel.c_str());
    }
    list_version_ = ~0ull;
    touched();
}

// --- the view ------------------------------------------------------------------------------------------------------

u32 ImageEditor::zoom() const { return current_ && current_->zoom ? current_->zoom : 1; }

void ImageEditor::set_zoom(u32 z) {
    if (!current_) return;
    current_->zoom = std::clamp(z, 1u, 32u);
    canvas_wanted_ = true;
    rebuild_panels();
}

void ImageEditor::fit() {
    if (!current_) return;
    const CookedTexture& img = current_->image;
    u32 z = 1;
    if (canvas_w_ > 16 && canvas_h_ > 16 && img.width && img.height)
        for (u32 k : kZooms)
            if (static_cast<u64>(img.width) * k + 16 <= canvas_w_ && static_cast<u64>(img.height) * k + 16 <= canvas_h_) z = k;
    current_->zoom = z;
    current_->cx = img.width / 2.0;
    current_->cy = img.height / 2.0;
    canvas_wanted_ = true;
    rebuild_panels();
}

bool ImageEditor::screen_of(f32 x, f32 y, f32& sx, f32& sy) const {
    if (!current_ || canvas_w_ == 0) return false;
    const f32 z = static_cast<f32>(zoom());
    sx = view_x_ + static_cast<f32>(origin_x_) + x * z;
    sy = view_y_ + static_cast<f32>(origin_y_) + y * z;
    return sx >= view_x_ && sy >= view_y_ && sx < view_x_ + static_cast<f32>(canvas_w_) && sy < view_y_ + static_cast<f32>(canvas_h_);
}

bool ImageEditor::to_pixel(f32 sx, f32 sy, i32& x, i32& y) const {
    if (!current_) return false;
    const f64 z = zoom();
    x = static_cast<i32>(std::floor((sx - view_x_ - static_cast<f64>(origin_x_)) / z));
    y = static_cast<i32>(std::floor((sy - view_y_ - static_cast<f64>(origin_y_)) / z));
    return true;
}

bool ImageEditor::over_canvas(Rml::Context* context) const {
    if (!context || mouse_x_ < view_x_ || mouse_y_ < view_y_ || mouse_x_ >= view_x_ + static_cast<f32>(canvas_w_) ||
        mouse_y_ >= view_y_ + static_cast<f32>(canvas_h_))
        return false;
    for (const Rml::Element* e = context->GetHoverElement(); e; e = e->GetParentNode())
        if (e->GetId() == "im-canvas") return true;
    return false;
}

void ImageEditor::sync_hover() {
    i32 x = -1, y = -1;
    if (!current_ || !to_pixel(mouse_x_, mouse_y_, x, y) || x < 0 || y < 0 || static_cast<u32>(x) >= image().width ||
        static_cast<u32>(y) >= image().height)
        x = y = -1;
    if (x == hover_x_ && y == hover_y_) return;
    hover_x_ = x;
    hover_y_ = y;
    rebuild_panels();
}

bool ImageEditor::handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context* context) {
    (void)ui_used;
    if (context) context_ = context;
    // The picker open on a colour has the mouse (its eyedropper the picture's pixels) and Esc.
    if (picking() && cp_->handle_event(e, density, context)) return true;
    switch (e.type) {
    case SDL_EVENT_KEY_UP:
        if (e.key.key == SDLK_SPACE) space_ = false;
        return false;
    case SDL_EVENT_MOUSE_MOTION: {
        mouse_x_ = e.motion.x * density;
        mouse_y_ = e.motion.y * density;
        if (panning_ && current_) {
            const f64 z = zoom();
            current_->cx -= (mouse_x_ - pan_from_x_) / z;
            current_->cy -= (mouse_y_ - pan_from_y_) / z;
            pan_from_x_ = mouse_x_;
            pan_from_y_ = mouse_y_;
            canvas_wanted_ = true;
        }
        sync_hover();
        if (stroking_) {
            i32 x, y;
            if (to_pixel(mouse_x_, mouse_y_, x, y)) drag(x, y);
            return true;
        }
        return panning_;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouse_x_ = e.button.x * density;
        mouse_y_ = e.button.y * density;
        if (!current_ || !over_canvas(context)) return false;
        if (e.button.button == SDL_BUTTON_MIDDLE || (e.button.button == SDL_BUTTON_LEFT && space_)) {
            panning_ = true;
            pan_from_x_ = mouse_x_;
            pan_from_y_ = mouse_y_;
            return true;
        }
        if (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT) return false;
        i32 x, y;
        if (!to_pixel(mouse_x_, mouse_y_, x, y)) return false;
        const SDL_Keymod mod = SDL_GetModState();
        press(x, y, e.button.button == SDL_BUTTON_RIGHT, (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_ALT) != 0);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (panning_) {
            panning_ = false;
            return true;
        }
        if (stroking_) {
            release();
            return true;
        }
        return false;
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!current_ || !over_canvas(context)) return false;
        const SDL_Keymod mod = SDL_GetModState();
        if (mod & SDL_KMOD_CTRL) {
            // Around the point under the mouse: it stays under the mouse.
            i32 bx, by;
            to_pixel(mouse_x_, mouse_y_, bx, by);
            const f64 fx = (mouse_x_ - view_x_ - static_cast<f64>(origin_x_)) / zoom(), fy = (mouse_y_ - view_y_ - static_cast<f64>(origin_y_)) / zoom();
            u32 to = zoom();
            if (e.wheel.y > 0) {
                for (u32 z : kZooms)
                    if (z > zoom()) {
                        to = z;
                        break;
                    }
            } else if (e.wheel.y < 0) {
                to = 1;
                for (u32 z : kZooms)
                    if (z < zoom()) to = z;
            }
            const f64 before = zoom();
            current_->zoom = to;
            // The canvas's middle moves so that (fx, fy) stays where it was.
            current_->cx = fx + (current_->cx - fx) * before / to;
            current_->cy = fy + (current_->cy - fy) * before / to;
            canvas_wanted_ = true;
            rebuild_panels();
            return true;
        }
        const f64 step = 48.0 / zoom();
        if (mod & SDL_KMOD_SHIFT) current_->cx -= e.wheel.y * step;
        else {
            current_->cy -= e.wheel.y * step;
            current_->cx += e.wheel.x * step;
        }
        canvas_wanted_ = true;
        return true;
    }
    default: break;
    }
    return false;
}

bool ImageEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (picking()) return cp_->handle_key(k); // Esc, Enter; the tab's keys wait
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
    switch (k.key) {
    case SDLK_SPACE: space_ = true; return true;
    case SDLK_B: set_tool("pencil"); return true;
    case SDLK_E: set_tool("eraser"); return true;
    case SDLK_G: set_tool("fill"); return true;
    case SDLK_I: set_tool("picker"); return true;
    case SDLK_X: swap_colors(); return true;
    case SDLK_F5: assets_.refresh(); return true; // «Ресурсы» looks again: a file changed outside is taken or said
    case SDLK_LEFTBRACKET: set_size(size_ > 1 ? size_ - 1 : 1); return true;
    case SDLK_RIGHTBRACKET: set_size(size_ + 1); return true;
    case SDLK_ESCAPE:
        if (!trying_) return false;
        cancel_effect();
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (!trying_) return false;
        apply_effect();
        return true;
    default: break;
    }
    return false;
}

// --- the panels and the canvas ----------------------------------------------------------------------------------

std::vector<std::string> ImageEditor::listed() const {
    std::vector<std::string> out;
    for (const ListRow& r : m_list_)
        if (!r.header && !r.game) out.push_back(r.rel);
    return out;
}

std::vector<std::string> ImageEditor::game_listed() const {
    std::vector<std::string> out;
    for (const ListRow& r : m_list_)
        if (!r.header && r.game) out.push_back(r.rel);
    return out;
}

void ImageEditor::rebuild_list() {
    std::vector<std::string> rels;
    for (const fs::path& f : assets_.images())
        if (const assets::AssetRecord* r = assets_.record_of(f)) rels.push_back(r->path);
    std::sort(rels.begin(), rels.end(), [](const std::string& a, const std::string& b) {
        const std::string fa = alphabet_key(folder_of(a)), fb = alphabet_key(folder_of(b));
        return fa != fb ? fa < fb : alphabet_key(name_of(a)) < alphabet_key(name_of(b));
    });
    std::vector<ListRow> rows;
    std::string folder = "\x01";
    for (const std::string& rel : rels) {
        if (folder_of(rel) != folder) {
            folder = folder_of(rel);
            ListRow h;
            h.header = true;
            h.name = folder.empty() ? "Ресурсы" : folder;
            h.index = static_cast<int>(rows.size());
            rows.push_back(h);
        }
        ListRow r;
        r.rel = rel;
        r.name = name_of(rel);
        r.folder = folder;
        r.index = static_cast<int>(rows.size());
        for (const auto& p : pictures_)
            if (!p->view_only && p->rel == rel) {
                r.unsaved = p->unsaved();
                r.selected = p.get() == current_;
            }
        rows.push_back(r);
    }
    // The game's pictures that «Ресурсы» has not: shown only. (Looked for again after each look at the files, not
    // after each stroke.)
    if (!game_files_known_) {
        game_files_ = game_pictures();
        game_files_known_ = true;
    }
    std::vector<std::string> game = game_files_;
    // (One open and gone since stays listed while it is open: the tab still shows it.)
    for (const auto& p : pictures_)
        if (p->view_only && p->missing && std::find(game.begin(), game.end(), p->game_file) == game.end()) game.push_back(p->game_file);
    std::sort(game.begin(), game.end(), [](const std::string& a, const std::string& b) {
        const std::string fa = alphabet_key(folder_of(a)), fb = alphabet_key(folder_of(b));
        return fa != fb ? fa < fb : alphabet_key(name_of(a)) < alphabet_key(name_of(b));
    });
    folder = "\x01";
    for (const std::string& rel : game) {
        if (folder_of(rel) != folder) {
            folder = folder_of(rel);
            ListRow h;
            h.header = true;
            h.game = true;
            h.name = "Только в игре: " + folder;
            h.index = static_cast<int>(rows.size());
            rows.push_back(h);
        }
        ListRow r;
        r.rel = rel;
        r.name = name_of(rel);
        r.folder = folder;
        r.game = true;
        r.index = static_cast<int>(rows.size());
        for (const auto& p : pictures_)
            if (p->view_only && p->game_file == rel) r.selected = p.get() == current_;
        rows.push_back(r);
    }
    auto same = [](const ListRow& a, const ListRow& b) {
        return a.rel == b.rel && a.name == b.name && a.header == b.header && a.selected == b.selected && a.unsaved == b.unsaved &&
               a.game == b.game;
    };
    if (!std::equal(rows.begin(), rows.end(), m_list_.begin(), m_list_.end(), same)) {
        m_list_ = std::move(rows);
        dirty("im_list");
    }
}

template <typename T>
static void set_value(T& member, const T& value, Rml::DataModelHandle model, const char* name) {
    if (member == value) return;
    member = value;
    if (model) model.DirtyVariable(name);
}

void ImageEditor::rebuild_panels() {
    Picture* p = current_;
    set_value(m_has_, p != nullptr, model_, "im_has");
    set_value(m_tool_, Rml::String(tool_), model_, "im_tool");
    set_value(m_size_, static_cast<int>(size_), model_, "im_size");
    set_value(m_c1_, Rml::String(css_of(first_)), model_, "im_c1");
    set_value(m_c2_, Rml::String(css_of(second_)), model_, "im_c2");
    set_value(m_c1_hex_, Rml::String(hex_of(first_)), model_, "im_c1_hex");
    set_value(m_c2_hex_, Rml::String(hex_of(second_)), model_, "im_c2_hex");
    {
        std::string note;
        auto copied = [&](const char* which, const std::string& name, Rgba c) {
            if (name.empty()) return;
            note += std::string(note.empty() ? "" : " ") + which + " " + hex_of(c) + " — копия цвета игры «" + name + "».";
        };
        copied("Левая", first_theme_, first_);
        copied("Правая", second_theme_, second_);
        if (!note.empty()) note += " Картинка хранит пиксели RGBA: если поменять цвет игры, нарисованное не изменится.";
        set_value(m_color_note_, Rml::String(note), model_, "im_color_note");
    }
    set_value(m_trying_, trying_, model_, "im_trying");
    set_value(m_effect_, Rml::String(effect_), model_, "im_effect");
    set_value(m_warning_, Rml::String(warning_), model_, "im_warning");
    set_value(m_note_, Rml::String(note_), model_, "im_note");
    bool effects_changed = false;
    for (EffectChip& c : m_effects_)
        if (c.selected != (c.id == effect_)) {
            c.selected = c.id == effect_;
            effects_changed = true;
        }
    if (effects_changed) dirty("im_effects");
    // The sliders of the effect chosen.
    std::vector<Param> params;
    if (effect_ == "light") params = {{"Яркость", -100, 100, params_[0]}, {"Контраст", -100, 100, params_[1]}};
    else if (effect_ == "hue") params = {{"Тон, °", -180, 180, params_[0]}, {"Насыщенность", -100, 100, params_[1]}, {"Светлота", -100, 100, params_[2]}};
    else if (effect_ == "alpha") params = {{"Порог", 1, 255, params_[0]}};
    else if (effect_ == "reduce") params = {{"Цветов", 2, 64, params_[0]}};
    else if (effect_ == "replace") params = {{"Допуск", 0, 255, params_[0]}};
    auto same_param = [](const Param& a, const Param& b) { return a.label == b.label && a.min == b.min && a.max == b.max && a.value == b.value; };
    if (!std::equal(params.begin(), params.end(), m_params_.begin(), m_params_.end(), same_param)) {
        m_params_ = std::move(params);
        dirty("im_params");
    }
    if (!p) {
        set_value(m_name_, Rml::String(), model_, "im_name");
        set_value(m_status_, Rml::String(), model_, "im_status");
        set_value(m_view_, false, model_, "im_view");
        return;
    }
    set_value(m_name_, Rml::String(p->view_only ? "игра: " + p->game_file : p->rel), model_, "im_name");
    set_value(m_view_, p->view_only, model_, "im_view");
    {
        std::string text;
        if (p->view_only) {
            text = "Только просмотр: " + view_reason(*p) + ".";
            if (p->missing) text += " Файла больше нет в игре: показана картинка, какой была.";
        }
        set_value(m_view_text_, Rml::String(text), model_, "im_view_text");
        set_value(m_view_copy_, Rml::String(p->view_only ? p->copy_of : std::string()), model_, "im_view_copy");
    }
    set_value(m_unsaved_, p->unsaved(), model_, "im_unsaved");
    const u32 n = frames();
    set_value(m_info_, Rml::String(size_words(p->image) + " · " + format_of(p->ext) + (n > 1 ? " · " + count_words(n, "кадр", "кадра", "кадров") : "")),
              model_, "im_info");
    set_value(m_frames_, static_cast<int>(n), model_, "im_frames");
    set_value(m_frames_from_, Rml::String(frames_from()), model_, "im_frames_from");
    set_value(m_frames_own_,
              p->view_only ? !(users_of_file && !users_of_file(p->game_file).empty()) : !(users_of && !p->id.is_null() && !users_of(p->id).empty()),
              model_, "im_frames_own");
    set_value(m_zoom_, static_cast<int>(zoom()), model_, "im_zoom");
    set_value(m_outside_shown_, p->outside, model_, "im_outside");
    set_value(m_outside_, Rml::String(p->outside ? "Файл «" + p->rel + "» изменён другой программой, а здесь есть несохранённые правки." : ""), model_,
              "im_outside_text");
    // The areas: the whole picture, then each frame of a strip that divides.
    std::vector<AreaChip> areas{{"Вся картинка", -1, p->area < 0}};
    if (n > 1 && p->image.width % n == 0)
        for (u32 i = 0; i < n; ++i) areas.push_back({"Кадр " + std::to_string(i + 1), static_cast<int>(i), p->area == static_cast<int>(i)});
    auto same_area = [](const AreaChip& a, const AreaChip& b) { return a.label == b.label && a.frame == b.frame && a.selected == b.selected; };
    if (!std::equal(areas.begin(), areas.end(), m_areas_.begin(), m_areas_.end(), same_area)) {
        m_areas_ = std::move(areas);
        dirty("im_areas");
    }
    // The fields of size and canvas start at the picture's (a frame's, for a strip).
    const u32 fw = n > 1 && p->image.width % n == 0 ? p->image.width / n : p->image.width;
    if (const std::string key = p->rel + " " + size_words(p->image) + " " + std::to_string(n); key != resize_for_) {
        resize_for_ = key;
        m_rw_ = m_cw_ = std::to_string(fw);
        m_rh_ = m_ch_ = std::to_string(p->image.height);
        for (const char* name : {"im_rw", "im_rh", "im_cw", "im_ch"}) dirty(name);
    }
    // The picture's colours.
    if (colors_version_ != p->history.version() || colors_of_ != p) {
        colors_version_ = p->history.version();
        colors_of_ = p;
        colors_ = assets::picture_colors(p->image);
        std::vector<Swatch> sw;
        for (const Rgba& c : colors_) sw.push_back({css_of(c), hex_of(c)});
        m_colors_ = std::move(sw);
        dirty("im_colors");
    }
    // The line under the canvas.
    std::string line = size_words(p->image) + " · " + format_of(p->ext) + " · " + std::to_string(zoom()) + "×";
    if (hover_x_ >= 0) {
        const Rgba c = assets::pixel_at(shown(), static_cast<u32>(hover_x_), static_cast<u32>(hover_y_));
        line += " · x " + std::to_string(hover_x_) + ", y " + std::to_string(hover_y_) + " · " + hex_of(c);
        if (n > 1 && p->image.width % n == 0) line += " · кадр " + std::to_string(static_cast<u32>(hover_x_) / (p->image.width / n) + 1);
    }
    if (p->unsaved()) line += " · не сохранена";
    set_value(m_status_, Rml::String(line), model_, "im_status");
    if (list_version_ != p->history.version() + pictures_.size()) {
        list_version_ = p->history.version() + pictures_.size();
        rebuild_list();
    }
}

void ImageEditor::update(Rml::Context* context) {
    if (!context) {
        if (picking()) cp_->close(true); // another tab: the colour as picked
        return;
    }
    context_ = context;
    if (m_list_.empty() && list_version_ == ~0ull) rebuild_list();
    if (list_version_ == ~0ull) {
        rebuild_list();
        list_version_ = current_ ? current_->history.version() + pictures_.size() : 0;
    }
    draw_canvas(context);
}

void ImageEditor::draw_canvas(Rml::Context* context) {
    Rml::Element* view = nullptr;
    for (int i = 0; i < context->GetNumDocuments() && !view; ++i) view = context->GetDocument(i)->GetElementById("im-canvas");
    if (!view || !view->IsVisible(true)) return;
    const Rml::Vector2f at = view->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = view->GetBox().GetSize(Rml::BoxArea::Content);
    const u32 w = static_cast<u32>(std::max(0.0f, std::floor(size.x))), h = static_cast<u32>(std::max(0.0f, std::floor(size.y)));
    view_x_ = std::round(at.x);
    view_y_ = std::round(at.y);
    if (w != canvas_w_ || h != canvas_h_) {
        canvas_w_ = w;
        canvas_h_ = h;
        canvas_wanted_ = true;
    }
    if (current_ && current_->zoom == 0 && w > 16) fit();
    if (!canvas_wanted_ || w == 0 || h == 0) return;
    canvas_wanted_ = false;

    canvas_.assign(static_cast<usize>(w) * h * 4, 0);
    auto token = [&](const char* name, Rgba fallback) {
        const std::string* hex = ui_ ? ui_->tokens().find(name) : nullptr;
        Rgba out = fallback;
        return hex && parse_hex(*hex, out) ? out : fallback;
    };
    const Rgba bg = token("surface-container-highest", Rgba{40, 42, 48, 255});
    const Rgba accent = token("primary", Rgba{120, 200, 255, 255});
    const Rgba frame_line{255, 170, 40, 255};
    const CookedTexture& img = shown();
    const Picture* p = current_;
    const u32 z = zoom();
    origin_x_ = p ? std::lround(w / 2.0 - p->cx * z) : 0;
    origin_y_ = p ? std::lround(h / 2.0 - p->cy * z) : 0;
    const u32 n = frames();
    const u32 fw = p && n > 1 && img.width % n == 0 ? img.width / n : 0;
    const PixelRect area = p && p->area >= 0 ? area_rect() : PixelRect{};
    for (u32 y = 0; y < h; ++y) {
        u8* row = canvas_.data() + static_cast<usize>(y) * w * 4;
        const i64 ry = static_cast<i64>(y) - origin_y_;
        const i64 py = floor_div(ry, z);
        for (u32 x = 0; x < w; ++x) {
            u8* q = row + static_cast<usize>(x) * 4;
            const i64 rx = static_cast<i64>(x) - origin_x_;
            const i64 px = floor_div(rx, z);
            if (!p || px < 0 || py < 0 || px >= img.width || py >= img.height) {
                q[0] = bg.r;
                q[1] = bg.g;
                q[2] = bg.b;
                q[3] = 255;
                continue;
            }
            const u8* s = img.rgba8.data() + (static_cast<usize>(py) * img.width + static_cast<usize>(px)) * 4;
            const u32 checker = ((floor_div(rx, 8) + floor_div(ry, 8)) & 1) ? 204u : 240u;
            u32 c[3];
            for (int k = 0; k < 3; ++k) c[k] = (s[k] * s[3] + checker * (255u - s[3]) + 127u) / 255u;
            // Outside the area: dimmed toward the background.
            if (!area.empty() && !area.contains(px, py))
                for (int k = 0; k < 3; ++k) c[k] = (c[k] + (k == 0 ? bg.r : k == 1 ? bg.g : bg.b)) / 2;
            // The pixel grid from 8 times.
            if (z >= 8 && (rx % z == 0 || ry % z == 0))
                for (u32& v : c) v = v * 3 / 4 + 24;
            // The frames' edges.
            if (fw && px % fw == 0 && rx % z == 0 && px > 0) {
                c[0] = frame_line.r;
                c[1] = frame_line.g;
                c[2] = frame_line.b;
            }
            // The area's border, two pixels inside it.
            if (!area.empty() && area.contains(px, py)) {
                const i64 left = static_cast<i64>(area.x) * z, top = static_cast<i64>(area.y) * z;
                const i64 right = static_cast<i64>(area.x + area.w) * z - 1, bottom = static_cast<i64>(area.y + area.h) * z - 1;
                if (rx - left < 2 || right - rx < 2 || ry - top < 2 || bottom - ry < 2) {
                    c[0] = accent.r;
                    c[1] = accent.g;
                    c[2] = accent.b;
                }
            }
            q[0] = static_cast<u8>(c[0]);
            q[1] = static_cast<u8>(c[1]);
            q[2] = static_cast<u8>(c[2]);
            q[3] = 255;
        }
    }
    if (!ui_) return;
    // The picture before the last one is shown by nothing now: the canvas took the last one at the UI's update.
    if (!canvas_old_.empty()) ui_->drop_image(canvas_old_);
    canvas_old_ = canvas_name_;
    canvas_name_ = "im_canvas_" + std::to_string(++canvas_serial_);
    ui_->set_image(canvas_name_, canvas_.data(), w, h);
    set_value(m_canvas_, Rml::String("/memory/" + canvas_name_), model_, "im_canvas");
    set_value(m_canvas_w_, static_cast<int>(w), model_, "im_canvas_w");
    set_value(m_canvas_h_, static_cast<int>(h), model_, "im_canvas_h");
    rebuild_panels();
}

} // namespace forge::editor_app
