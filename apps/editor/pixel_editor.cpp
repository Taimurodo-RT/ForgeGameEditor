#include "pixel_editor.h"
#include "pixel_files.h"

#include "asset_library.h"

#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/editor/sources.h"
#include "forge/pixel/svg.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>

namespace fs = std::filesystem;

namespace forge::editor_app {

using namespace pixel_files;

static_assert(pixel::kMaxStripWidth == objects::kMaxStripWidth && pixel::kMaxStripFrames == objects::kMaxFrameLimit,
              "a baked strip is a picture of the game");

namespace {

constexpr f64 kOnion = 0.3;       // how much of the frames either side «Калька» shows
constexpr f32 kPivotReach = 10;   // how near the pivot a press takes it (view pixels)
constexpr f32 kMoveStart = 3;     // how far a part is dragged before it moves (logical px)
constexpr u32 kNewClipFrames = 4; // a clip as it is added
constexpr f32 kNewClipFps = 10;

// game.json with another "frame_limit", written at once (Ctrl+Z writes the file back byte for byte), and the library
// reading its templates by it. game.json that cannot be written (read-only, held by another program) changes nothing:
// the file, the limit, the templates and the fields stay as they are, the step is not done (failed: the history stays
// where it was), and the reason is said under the fields and in the log.
class LimitCommand final : public editor::Command {
public:
    struct Side {
        std::vector<u8> bytes;
        u32 limit = objects::kDefaultFrameLimit;
    };
    LimitCommand(PixelEditor& editor, objects::Library& lib, fs::path file, Side before, Side after, std::string label)
        : editor_(editor), lib_(lib), file_(std::move(file)), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { put(after_); }
    void revert(editor::Document&) override { put(before_); }
    std::string label() const override { return label_; }
    bool failed() const override { return failed_; }

private:
    void put(const Side& to) {
        failed_ = !write_file_atomic(file_, to.bytes);
        if (failed_) {
            const std::string why = "game.json не записывается: предел кадров остался " + std::to_string(lib_.frame_limit());
            FORGE_ERROR("Векторная: %s (%s)", why.c_str(), path_to_utf8(file_).c_str());
            editor_.refused(why);
            return;
        }
        lib_.set_frame_limit(to.limit);
        lib_.reload_templates();
        editor_.reload();
    }
    PixelEditor& editor_;
    objects::Library& lib_;
    fs::path file_;
    Side before_, after_;
    std::string label_;
    bool failed_ = false;
};

// A template's picture, frames and animations as they were, onto the template as it is now.
class GiveCommand final : public editor::Command {
public:
    struct Look {
        std::string picture;
        u32 frames = 1;
        std::map<std::string, objects::Clip> animations;
        // What the file had past the game's frame limit (objects::Template): a picture given replaces it, undo brings it back.
        u32 frames_over = 0;
        std::map<std::string, objects::Clip> animations_over;
    };
    GiveCommand(objects::Library& lib, u64 key, Look before, Look after, std::string label)
        : lib_(lib), key_(key), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { set(after_); }
    void revert(editor::Document&) override { set(before_); }
    std::string label() const override { return label_; }

private:
    void set(const Look& to) {
        const objects::Template* now = lib_.find(key_);
        if (!now) return;
        objects::Template t = *now;
        t.picture = to.picture;
        t.frames = to.frames;
        t.animations = to.animations;
        t.frames_over = to.frames_over;
        t.animations_over = to.animations_over;
        std::string error;
        if (!lib_.put(std::move(t), &error)) FORGE_ERROR("Шаблон «%s»: %s", now->name.c_str(), error.c_str());
    }
    objects::Library& lib_;
    u64 key_;
    Look before_, after_;
    std::string label_;
};

// A name as the alphabet orders it: Ё with Е.
std::string alphabet_key(std::string s) {
    for (usize at = 0; (at = s.find("Ё", at)) != std::string::npos;) s.replace(at, 2, "Е");
    for (usize at = 0; (at = s.find("ё", at)) != std::string::npos;) s.replace(at, 2, "е");
    return s;
}

// "Герой.pixel.json" → "Герой".
std::string stem_of(const std::string& file_name) {
    const std::string suffix = ".pixel.json";
    if (file_name.size() > suffix.size() && file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return file_name.substr(0, file_name.size() - suffix.size());
    return path_to_utf8(utf8_path(file_name).stem());
}

// Each pixel of the frame a square of s × s.
assets::CookedTexture bigger(const assets::CookedTexture& in, u32 s) {
    assets::CookedTexture out;
    out.width = in.width * s, out.height = in.height * s;
    out.rgba8.resize(static_cast<usize>(out.width) * out.height * 4);
    for (u32 y = 0; y < out.height; ++y)
        for (u32 x = 0; x < out.width; ++x) {
            const u8* p = &in.rgba8[(static_cast<usize>(y / s) * in.width + x / s) * 4];
            std::copy(p, p + 4, &out.rgba8[(static_cast<usize>(y) * out.width + x) * 4]);
        }
    return out;
}

// `over` drawn over `under` × k (straight RGBA, the same size), into `over`.
void over_faint(assets::CookedTexture& over, const assets::CookedTexture& under, f64 k) {
    if (under.width != over.width || under.height != over.height) return;
    for (usize i = 0; i + 3 < over.rgba8.size(); i += 4) {
        const f64 da = over.rgba8[i + 3] / 255.0, sa = under.rgba8[i + 3] / 255.0 * k;
        const f64 oa = da + sa * (1 - da);
        if (oa <= 0) continue;
        for (usize c = 0; c < 3; ++c)
            over.rgba8[i + c] = static_cast<u8>(std::lround((over.rgba8[i + c] * da + under.rgba8[i + c] * sa * (1 - da)) / oa));
        over.rgba8[i + 3] = static_cast<u8>(std::lround(oa * 255));
    }
}

void plot(assets::CookedTexture& t, i64 x, i64 y, const std::array<u8, 4>& c) {
    if (x < 0 || y < 0 || x >= t.width || y >= t.height) return;
    std::copy(c.begin(), c.end(), &t.rgba8[(static_cast<usize>(y) * t.width + static_cast<usize>(x)) * 4]);
}

constexpr std::array<u8, 4> kMark{40, 120, 255, 255}, kMarkEdge{255, 255, 255, 255}, kHover{255, 150, 20, 255};

// The part of the tree's row under the mouse: dashed round its bounds, 2 px out of them, a colour of its own.
void outline(assets::CookedTexture& t, const f64 box[4], i64 k) {
    const i64 x0 = std::lround(box[0]) - 2 * k, y0 = std::lround(box[1]) - 2 * k, x1 = std::lround(box[2]) + 2 * k,
              y1 = std::lround(box[3]) + 2 * k;
    for (i64 x = x0; x <= x1; ++x)
        if ((x - x0) % (5 * k) < 3 * k)
            for (i64 j = 0; j < k; ++j) plot(t, x, y0 - j, kHover), plot(t, x, y1 + j, kHover);
    for (i64 y = y0; y <= y1; ++y)
        if ((y - y0) % (5 * k) < 3 * k)
            for (i64 j = 0; j < k; ++j) plot(t, x0 - j, y, kHover), plot(t, x1 + j, y, kHover);
}

// The selected part over the vector: its bounds and its pivot.
// k: texels to a px of the view (its dp-ratio, whole), so the marks keep their size on screen.
void mark(assets::CookedTexture& t, const f64 box[4], f64 px, f64 py, i64 k) {
    const i64 x0 = std::lround(box[0]), y0 = std::lround(box[1]), x1 = std::lround(box[2]), y1 = std::lround(box[3]);
    for (i64 x = x0; x <= x1; x += 1)
        if ((x - x0) % (6 * k) < 4 * k)
            for (i64 j = 0; j < k; ++j) plot(t, x, y0 + j, kMark), plot(t, x, y1 - j, kMark);
    for (i64 y = y0; y <= y1; y += 1)
        if ((y - y0) % (6 * k) < 4 * k)
            for (i64 j = 0; j < k; ++j) plot(t, x0 + j, y, kMark), plot(t, x1 - j, y, kMark);
    const i64 cx = std::lround(px), cy = std::lround(py);
    if (k == 1) {
        for (i64 d = -6; d <= 6; ++d) {
            for (i64 w = -1; w <= 1; w += 2) plot(t, cx + d, cy + w, kMarkEdge), plot(t, cx + w, cy + d, kMarkEdge);
            plot(t, cx + d, cy, kMark), plot(t, cx, cy + d, kMark);
        }
        return;
    }
    // A cross k texels thick, edged k texels on each side.
    const i64 lo = -(3 * k) / 2, hi = lo + 3 * k - 1;
    for (i64 j = lo; j <= hi; ++j)
        if ((j - lo) / k != 1)
            for (i64 d = -6 * k; d <= 6 * k; ++d) plot(t, cx + d, cy + j, kMarkEdge), plot(t, cx + j, cy + d, kMarkEdge);
    for (i64 j = lo + k; j < lo + 2 * k; ++j)
        for (i64 d = -6 * k; d <= 6 * k; ++d) plot(t, cx + d, cy + j, kMark), plot(t, cx + j, cy + d, kMark);
}

// The script a new character starts with: its first part bobs, so the views move at once.
std::string first_script(const std::string& stem, const std::string& svg_name, const pixel::Rig& rig) {
    std::string top;
    for (const pixel::Part& p : rig.parts())
        if (p.parent < 0) {
            top = p.id;
            break;
        }
    std::string s = "-- Поза персонажа «" + stem + "» (" + svg_name +
                    "): ставит части рисунка в кадр.\n"
                    "-- t — секунды от начала клипа, p — параметры (поля во вкладке), rng — случайность кадра,\n"
                    "-- clip — состояние клипа (\"stand\", \"walk\"…).\n"
                    "-- part(\"id\"):set{ x, y, rot, scale, sx, sy, opacity, fill } двигает часть рисунка с этим id вокруг её опорной точки,\n"
                    "-- shape(\"id\"):morph(\"другая\", k) превращает фигуру в другую, ease(\"smooth\", k) сглаживает движение.\n"
                    "return {\n"
                    "  params = { speed = 1, bob = 1 },\n"
                    "  pose = function(t, p, rng, clip)\n"
                    "    local s = math.sin(t * p.speed * 2 * math.pi)\n";
    s += top.empty() ? "    -- в рисунке нет частей с id: дайте группам и фигурам id в редакторе SVG\n"
                     : "    part(\"" + top + "\"):set{ y = -p.bob * math.abs(s) }\n";
    s += "  end,\n}\n";
    return s;
}

// The code «Новый» starts a character with: its picture drawn by draw, and a clip of the game's first state in which the
// body bobs, so the views move at once.
std::string drawn_script(const std::string& stem, const std::string& state) {
    bool word = !state.empty() && !(state[0] >= '0' && state[0] <= '9');
    for (char c : state) word = word && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
    const std::string key = word ? state : "[\"" + state + "\"]";
    return "-- «" + stem + "»: рисунок и движение кодом. draw рисует части фигурами: rect, circle, ellipse, line, poly, path и\n"
           "-- group (часть с частями внутри); clips двигает их: part(\"body\").y(значение, секунды, \"сглаживание\").\n"
           "-- Изменения видны после сохранения (Ctrl+S).\n"
           "return {\n"
           "  params = { bob = 1 },\n"
           "  draw = function(p, rng)\n"
           "    canvas(32, 32)\n"
           "    group(\"body\", function()\n"
           "      rect(\"torso\", { x = 10, y = 14, w = 12, h = 11, r = 3, fill = \"#4a7bd0\" })\n"
           "      circle(\"head\", { x = 16, y = 9, r = 6, fill = \"#f0c090\" })\n"
           "      circle(\"eye\", { x = 18, y = 8, r = 1, fill = \"#202020\" })\n"
           "    end)\n"
           "    rect(\"leg_l\", { x = 11, y = 25, w = 4, h = 6, fill = \"#303040\", pivot = { 13, 25 } })\n"
           "    rect(\"leg_r\", { x = 17, y = 25, w = 4, h = 6, fill = \"#303040\", pivot = { 19, 25 } })\n"
           "  end,\n"
           "  clips = {\n"
           "    " + key + " = function(p)\n"
           "      part(\"body\").y(-p.bob, 0.4, \"in_out_sine\"):to(0, 0.4, \"in_out_sine\")\n"
           "    end,\n"
           "  },\n"
           "}\n";
}

} // namespace

PixelEditor::PixelEditor(AssetLibrary& assets, level::LevelModule& module, editor::UndoStack& history)
    : assets_(assets), module_(module), history_(history) {}

PixelEditor::~PixelEditor() = default;

bool PixelEditor::init(ui::Ui& ui) {
    ui_ = &ui;
    for (f32 fps : objects::kClipFps) m_fps_.push_back({number_words(fps), false});
    return true;
}

void PixelEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("name", &Row::name);
        s.RegisterMember("sub", &Row::sub);
        s.RegisterMember("selected", &Row::selected);
    }
    model.RegisterArray<std::vector<Row>>();
    if (auto s = model.RegisterStruct<SvgRow>()) {
        s.RegisterMember("name", &SvgRow::name);
        s.RegisterMember("sub", &SvgRow::sub);
    }
    model.RegisterArray<std::vector<SvgRow>>();
    if (auto s = model.RegisterStruct<PartRow>()) {
        s.RegisterMember("id", &PartRow::id);
        s.RegisterMember("label", &PartRow::label);
        s.RegisterMember("depth", &PartRow::depth);
        s.RegisterMember("shape", &PartRow::shape);
        s.RegisterMember("selected", &PartRow::selected);
    }
    model.RegisterArray<std::vector<PartRow>>();
    if (auto s = model.RegisterStruct<ParamRow>()) {
        s.RegisterMember("name", &ParamRow::name);
        s.RegisterMember("value", &ParamRow::value);
    }
    model.RegisterArray<std::vector<ParamRow>>();
    if (auto s = model.RegisterStruct<StateChip>()) {
        s.RegisterMember("id", &StateChip::id);
        s.RegisterMember("name", &StateChip::name);
        s.RegisterMember("sub", &StateChip::sub);
        s.RegisterMember("on", &StateChip::on);
        s.RegisterMember("selected", &StateChip::selected);
    }
    model.RegisterArray<std::vector<StateChip>>();
    if (auto s = model.RegisterStruct<FrameCell>()) {
        s.RegisterMember("index", &FrameCell::index);
        s.RegisterMember("shown", &FrameCell::shown);
        s.RegisterMember("style", &FrameCell::style);
        s.RegisterMember("label", &FrameCell::label);
    }
    model.RegisterArray<std::vector<FrameCell>>();
    if (auto s = model.RegisterStruct<TemplateRow>()) {
        s.RegisterMember("name", &TemplateRow::name);
        s.RegisterMember("icon", &TemplateRow::icon);
        s.RegisterMember("note", &TemplateRow::note);
        s.RegisterMember("draws", &TemplateRow::draws);
    }
    model.RegisterArray<std::vector<TemplateRow>>();
    // FpsChip is AnimationEditor's shape too; registered here under its own type.
    if (auto s = model.RegisterStruct<FpsChip>()) {
        s.RegisterMember("label", &FpsChip::label);
        s.RegisterMember("selected", &FpsChip::selected);
    }
    model.RegisterArray<std::vector<FpsChip>>();

    model.Bind("px_list", &m_list_);
    model.Bind("px_svgs", &m_svgs_);
    model.Bind("px_parts", &m_parts_);
    model.Bind("px_params", &m_params_);
    model.Bind("px_states", &m_states_);
    model.Bind("px_frames", &m_frames_);
    model.Bind("px_fps", &m_fps_);
    model.Bind("px_templates", &m_templates_);
    model.Bind("px_name", &m_name_);
    model.Bind("px_vector", &m_vector_);
    model.Bind("px_pixel", &m_pixel_);
    model.Bind("px_one", &m_one_);
    model.Bind("px_problem", &m_problem_);
    model.Bind("px_note", &m_note_);
    model.Bind("px_frame_label", &m_frame_label_);
    model.Bind("px_clip_name", &m_clip_name_);
    model.Bind("px_seed", &m_seed_);
    model.Bind("px_fw", &m_fw_);
    model.Bind("px_fh", &m_fh_);
    model.Bind("px_outline_color", &m_outline_color_);
    model.Bind("px_picture", &m_picture_);
    model.Bind("px_size", &m_size_);
    model.Bind("px_total", &m_total_);
    model.Bind("px_limit", &m_limit_);
    model.Bind("px_clip_frames", &m_clip_frames_);
    model.Bind("px_has", &m_has_);
    model.Bind("px_has_clip", &m_has_clip_);
    model.Bind("px_playing", &m_playing_);
    model.Bind("px_onion", &m_onion_);
    model.Bind("px_game_palette", &m_game_palette_);
    model.Bind("px_outline", &m_outline_);
    model.Bind("px_clip_loop", &m_clip_loop_);
    model.Bind("px_edited", &m_edited_);
    model.Bind("px_giving", &m_giving_);
    model.Bind("px_can_give", &m_can_give_);
    bind_motion(model);
    bind_mouse(model);
    bind_view(model);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_enter = [](const Rml::VariantList& a, usize i) { return i < a.size() && a[i].Get<bool>(); };
    // A text field's value when it loses the keyboard.
    auto field = [](Rml::Event& ev) -> std::optional<std::string> {
        Rml::Element* e = ev.GetTargetElement();
        if (!e || e->GetTagName() != "input") return std::nullopt;
        return static_cast<Rml::ElementFormControl*>(e)->GetValue();
    };
    auto number = [](const std::string& text, f64& out) {
        std::string t = text;
        std::replace(t.begin(), t.end(), ',', '.');
        char* end = nullptr;
        out = std::strtod(t.c_str(), &end);
        return !t.empty() && end && *end == '\0' && std::isfinite(out);
    };

    on("px_open", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(listed_.size())) open(listed_[static_cast<usize>(i)]);
    });
    on("px_new", [this](Rml::Event&, const Rml::VariantList&) { create_drawn(); });
    on("px_create", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(svgs_.size())) create(svgs_[static_cast<usize>(i)]);
    });
    on("px_part", [this, arg_str](Rml::Event&, const Rml::VariantList& a) { select_part(arg_str(a, 0)); });
    auto param = [this, number](const std::string& name, const std::string& text) {
        f64 v = 0;
        if (number(text, v)) set_param(name, v);
        else rebuild(); // the field shows the value again
    };
    on("px_param", [param, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 2)) param(arg_str(a, 0), arg_str(a, 1));
    });
    on("px_param_commit", [param, arg_str, field](Rml::Event& ev, const Rml::VariantList& a) {
        if (auto text = field(ev)) param(arg_str(a, 0), *text);
    });
    auto seed = [this](const std::string& text) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
        if (!text.empty() && text[0] != '-' && end && *end == '\0') set_seed(v);
        else rebuild();
    };
    on("px_seed", [seed, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) seed(arg_str(a, 0));
    });
    on("px_seed_commit", [seed, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) seed(*text);
    });
    auto limit = [this](const std::string& text) {
        if (text == m_limit_) return; // the field left as it shows
        char* end = nullptr;
        const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
        const bool whole = !text.empty() && text[0] != '-' && end && *end == '\0';
        set_frame_limit(whole ? static_cast<u32>(std::min<unsigned long long>(v, objects::kMaxFrameLimit + 1ull)) : 0);
    };
    on("px_limit", [limit, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) limit(arg_str(a, 0));
    });
    on("px_limit_commit", [limit, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) limit(*text);
    });
    on("px_state", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string id = arg_str(a, 0);
        if (std::any_of(doc_.clips.begin(), doc_.clips.end(), [&](const auto& c) { return c.state == id; })) select_clip(id);
        else add_clip(id);
    });
    on("px_clip_remove", [this](Rml::Event&, const Rml::VariantList&) { remove_clip(); });
    on("px_clip_less", [this](Rml::Event&, const Rml::VariantList&) {
        if (const pixel::ClipSpec* c = selected_clip()) set_clip_frames(static_cast<int>(c->frames) - 1);
    });
    on("px_clip_more", [this](Rml::Event&, const Rml::VariantList&) {
        if (const pixel::ClipSpec* c = selected_clip()) set_clip_frames(static_cast<int>(c->frames) + 1);
    });
    on("px_set_fps", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(std::size(objects::kClipFps))) set_clip_fps(objects::kClipFps[i]);
    });
    on("px_loop", [this](Rml::Event&, const Rml::VariantList&) {
        if (const pixel::ClipSpec* c = selected_clip()) set_clip_loop(!c->loop);
    });
    on("px_play", [this](Rml::Event&, const Rml::VariantList&) { play(!playing_); });
    on("px_step", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { step(arg_int(a, 0) < 0 ? -1 : 1); });
    on("px_onion", [this](Rml::Event&, const Rml::VariantList&) { set_onion(!onion_); });
    auto size = [this](const std::string& which, const std::string& text) {
        char* end = nullptr;
        const long v = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || !end || *end != '\0' || v < 1 || v > static_cast<long>(pixel::kMaxFrameSide)) {
            note_ = "Размер кадра — целое от 1 до " + std::to_string(pixel::kMaxFrameSide);
            rebuild();
            return;
        }
        if (which == "w") set_frame_size(static_cast<u32>(v), doc_.frame_h);
        else set_frame_size(doc_.frame_w, static_cast<u32>(v));
    };
    on("px_size", [size, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 2)) size(arg_str(a, 0), arg_str(a, 1));
    });
    on("px_size_commit", [size, arg_str, field](Rml::Event& ev, const Rml::VariantList& a) {
        if (auto text = field(ev)) size(arg_str(a, 0), *text);
    });
    on("px_palette", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { set_game_palette(arg_int(a, 0) == 1); });
    on("px_outline_toggle", [this](Rml::Event&, const Rml::VariantList&) { set_outline(!doc_.outline); });
    on("px_outline_color", [this, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) set_outline_color(arg_str(a, 0));
    });
    on("px_outline_color_commit", [this, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) set_outline_color(*text);
    });
    on("px_bake", [this](Rml::Event&, const Rml::VariantList&) { bake(false); });
    on("px_bake_over", [this](Rml::Event&, const Rml::VariantList&) { bake(true); });
    on("px_give_open", [this](Rml::Event&, const Rml::VariantList&) { open_give(!giving_); });
    on("px_give", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(templates_.size())) give(templates_[static_cast<usize>(i)]);
    });
}

// --- each frame -------------------------------------------------------------------

u32 PixelEditor::view_px() const { return std::max(1u, static_cast<u32>(std::lround(kViewPx * dp_))); }

void PixelEditor::update(f64 dt, Rml::Context* context) {
    context_ = context;
    if (context && context->GetDensityIndependentPixelRatio() != dp_) {
        dp_ = context->GetDensityIndependentPixelRatio();
        view_dirty_ = true; // drawn again at the window's pixels
    }
    if (module_.library() && module_.library()->version() != lib_seen_) {
        lib_seen_ = module_.library()->version();
        rebuild_states();
        rebuild();
    }
    if (list_dirty_) {
        list_dirty_ = false;
        rebuild_list();
        if (has_) {
            check_files(); // the SVG may be somewhere else now
            check_edited();
            rebuild();
        }
    }
    file_clock_ += dt;
    if (file_clock_ >= 0.5) {
        file_clock_ = 0;
        check_files();
    }
    // The preview plays the range by its frames, at its speed, over again while L is on.
    const pixel::ClipSpec* c = selected_clip();
    if (playing_ && c && !dragging_ && !move_started_ && tl_drag_ == Drag::None && cut_.frames > 0) {
        play_clock_ += std::min(dt, 0.1) * speed_;
        const f64 span = cut_.frames / static_cast<f64>(cut_.fps);
        u64 k = static_cast<u64>(std::floor(play_clock_ * cut_.fps + 1e-9));
        bool wrapped = false, ended = false;
        if (k >= cut_.frames) {
            if (loop_view_) {
                play_clock_ = std::fmod(play_clock_, span);
                k %= cut_.frames;
                wrapped = true;
            } else {
                k = cut_.frames - 1;
                ended = true;
            }
        }
        // The marks of the frames shown since the last one sound (part 3в).
        sound_frames(static_cast<i64>(k), wrapped);
        if (ended) play(false);
        const f64 t = cut_.time(static_cast<u32>(k));
        if (t != t_) {
            t_ = t;
            ++fps_frames_;
            sync_time();
        }
    }
    // How many frames a second the preview shows in fact; paused, it shows none, and no number says so.
    fps_clock_ += dt;
    if (fps_clock_ >= 1 || (!playing_ && shown_fps_ != 0)) {
        const f64 shown = playing_ ? std::round(fps_frames_ / fps_clock_) : 0;
        fps_clock_ = 0;
        fps_frames_ = 0;
        const std::string words = playing_ ? number_words(shown) + " к/с" : std::string();
        if (shown != shown_fps_ || words != m_shown_fps_) {
            shown_fps_ = shown;
            m_shown_fps_ = words;
            dirty("px_shown_fps");
        }
    }
    if (context && has_) {
        // The panel's field shows the panel's text (made anew when the mode is shown, set by Ctrl+Z).
        if (auto* text = rmlui_dynamic_cast<Rml::ElementFormControl*>(element("px-code-text")); text && text->GetValue() != code_)
            text->SetValue(code_);
        update_caret();
        update_timeline_box();
        update_hover(context);
    }
    if (context) {
        Rml::Element* view = nullptr;
        for (int i = 0; i < context->GetNumDocuments() && !view; ++i) view = context->GetDocument(i)->GetElementById("px-vector-img");
        if (view && view->IsVisible(true)) {
            const Rml::Vector2f at = view->GetAbsoluteOffset(Rml::BoxArea::Content);
            const Rml::Vector2f size = view->GetBox().GetSize(Rml::BoxArea::Content);
            view_x_ = at.x, view_y_ = at.y, view_w_ = size.x, view_h_ = size.y;
        } else {
            view_w_ = view_h_ = 0;
        }
    }
    if (view_dirty_) redraw();
    track_mouse(context);
    // Without a device the preview's sounds end on time all the same.
    if (!mixer_.has_device() && mixer_.voices() > 0) mixer_.advance(dt);
}

bool PixelEditor::handle_event(const SDL_Event& e, f32 density, Rml::Context* context) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Shift as the keys say (the wheel and the mouse do not): a flag dragged with it, the wheel along.
        shift_ = e.key.key == SDLK_LSHIFT || e.key.key == SDLK_RSHIFT ? e.type == SDL_EVENT_KEY_DOWN : (e.key.mod & SDL_KMOD_SHIFT) != 0;
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST: shift_ = false; return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if ((e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_MIDDLE) || !context || !has_) return false;
        bool vector = false, timeline = false, pixels = false;
        for (const Rml::Element* el = context->GetHoverElement(); el; el = el->GetParentNode()) {
            vector = vector || el->GetId() == "px-vector";
            timeline = timeline || el->GetId() == "px-tl";
            pixels = pixels || el->GetId() == "px-pixel";
        }
        mouse_x_ = e.button.x * density, mouse_y_ = e.button.y * density;
        // Part 3в: the middle button pans the vector; picking, a click copies the colour under it.
        if (vector && e.button.button == SDL_BUTTON_MIDDLE) {
            panning_ = true;
            pan_x_ = mouse_x_, pan_y_ = mouse_y_;
            return true;
        }
        if (picking_ && (vector || pixels) && e.button.button == SDL_BUTTON_LEFT) return pick_click(mouse_x_, mouse_y_);
        if (timeline) {
            context_ = context;
            update_timeline_box();
            const bool middle = e.button.button == SDL_BUTTON_MIDDLE;
            if (!middle && edge_press(e.button.x * density, e.button.y * density, context->GetHoverElement(), false)) return true;
            timeline_press(e.button.x * density, e.button.y * density, context->GetHoverElement(), middle);
            return tl_drag_ != Drag::None;
        }
        if (!vector || e.button.button != SDL_BUTTON_LEFT) return false;
        press(e.button.x * density, e.button.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x_ = e.motion.x * density, mouse_y_ = e.motion.y * density;
        if (panning_) {
            pan_view(mouse_x_ - pan_x_, mouse_y_ - pan_y_);
            pan_x_ = mouse_x_, pan_y_ = mouse_y_;
            return true;
        }
        if (tl_drag_ != Drag::None) {
            timeline_drag(e.motion.x * density);
            return true;
        }
        if (!dragging_ && !moving_) return false;
        drag(e.motion.x * density, e.motion.y * density);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (panning_ && e.button.button == SDL_BUTTON_MIDDLE) {
            panning_ = false;
            return true;
        }
        if (tl_drag_ != Drag::None) {
            timeline_release();
            return true;
        }
        if (!dragging_ && !moving_) return false;
        release();
        return true;
    default: return false;
    }
}

bool PixelEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) return false;
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    switch (k.key) {
    case SDLK_SPACE: play(!playing_); return true;
    case SDLK_LEFT: shift ? to_start() : step(-1); return true;
    case SDLK_RIGHT: shift ? to_end() : step(1); return true;
    case SDLK_L: set_loop_view(!loop_view_); return true;
    case SDLK_F: center_playhead(); return true;
    case SDLK_B: range_start_here(); return true;
    case SDLK_N: range_end_here(); return true;
    case SDLK_ESCAPE:
        if (giving_) open_give(false);
        else if (picking_) set_picking(false);
        else return false;
        return true;
    // Part 3в: the vector's zoom, the grid, the point under the mouse, the eyedropper, the preview's sound.
    case SDLK_0:
    case SDLK_KP_0: fit_view(); return true;
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS: zoom_view_by(1); return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: zoom_view_by(-1); return true;
    case SDLK_APOSTROPHE: set_grid(!grid_); return true;
    case SDLK_P: copy_coords(); return true;
    case SDLK_I: set_picking(!picking_); return true;
    case SDLK_M: set_sound(!sound_on_); return true;
    case SDLK_UP:
    case SDLK_DOWN: {
        if (listed_.empty()) return true;
        const auto at = std::find(listed_.begin(), listed_.end(), rel_);
        const i64 i = at == listed_.end() ? 0 : (at - listed_.begin()) + (k.key == SDLK_UP ? -1 : 1);
        open(listed_[static_cast<usize>(std::clamp<i64>(i, 0, static_cast<i64>(listed_.size()) - 1))]);
        return true;
    }
    default: break;
    }
    // 1, 2, 3…: the clips in their order.
    if (k.key >= SDLK_1 && k.key <= SDLK_9) {
        const usize i = static_cast<usize>(k.key - SDLK_1);
        if (i < doc_.clips.size()) select_clip(doc_.clips[i].state);
        return true;
    }
    return false;
}

std::string PixelEditor::status() const {
    if (!has_) return listed_.empty() ? "Векторная: персонажей пока нет" : "Векторная: персонаж не выбран";
    std::string s = "Векторная: «" + name() + "» · кадр " + std::to_string(doc_.frame_w) + " × " + std::to_string(doc_.frame_h);
    if (const pixel::ClipSpec* c = selected_clip()) {
        s += " · «" + state_name(c->state) + "» " + std::to_string(frame_ + 1) + " из " + std::to_string(whole_.frames) + " · " + number_words(c->fps) + " к/с";
    }
    if (!problem_.empty()) s += " · есть ошибка";
    return s + (playing_ ? " · играет" : " · пауза");
}

// --- the characters ----------------------------------------------------------------

std::string PixelEditor::name() const { return stem_of(path_to_utf8(file_.filename())); }

std::string PixelEditor::state_name(const std::string& id) const {
    for (const auto& s : states_)
        if (s.id == id) return s.name;
    return id;
}

fs::path PixelEditor::script_file() const { return doc_.script.empty() ? fs::path() : file_.parent_path() / utf8_path(doc_.script); }

bool PixelEditor::open(const std::string& rel) {
    if (rel == rel_ && has_) return false;
    if (code_unsaved()) {
        note_ = "Код «" + doc_.script + "» не сохранён: сохраните его (Ctrl+S в панели) или отмените набор (Ctrl+Z в панели), потом откройте другого";
        rebuild();
        return false;
    }
    rel_ = rel;
    file_ = assets_.abs(rel);
    doc_ = {};
    rig_ = {};
    script_ = {};
    svg_text_.clear();
    script_text_.clear();
    drawn_ = false;
    drawing_ = {};
    svg_watch_ = script_watch_ = doc_watch_ = {};
    part_.clear();
    clip_.clear();
    note_.clear();
    giving_ = false;
    forget_character();
    vector_ = frame_px_ = vector_shown_ = {};
    zoom_ = {};
    hush();
    sound_files_.clear();
    reload();
    fit_view();
    return has_;
}

void PixelEditor::forget_character() {
    t_ = play_clock_ = 0;
    frame_ = 0;
    playing_ = true;
    rec_ = {};
    rec_ok_ = rec_stale_ = false;
    rec_clip_.clear();
    rec_error_.clear();
    block_ = -1;
    caret_blocks_.clear();
    lit_first_ = lit_last_ = 0;
    tv_ = {};
    tl_laid_w_ = -1;
    code_.clear();
    code_disk_.clear();
    script_panel_.clear();
    code_conflict_ = false;
    typing_.clear();
    typing_at_ = 0;
    typing_open_ = false;
    caret_ = ~0u;
    modules_.clear();
    m_inspector_.clear();
    m_inspector_call_.clear();
    m_has_block_ = false;
}

bool PixelEditor::create(const std::string& svg_rel) {
    note_.clear();
    if (code_unsaved()) {
        note_ = "Код «" + doc_.script + "» не сохранён: сохраните его (Ctrl+S в панели) или отмените набор (Ctrl+Z в панели), потом делайте нового";
        rebuild();
        return false;
    }
    const fs::path svg = assets_.abs(svg_rel);
    const assets::AssetRecord* r = assets_.record_of(svg);
    std::string text;
    if (!r || !read_text(svg, text)) {
        note_ = "Рисунок «" + svg_rel + "» не читается";
        rebuild();
        return false;
    }
    pixel::Rig probe;
    std::string why;
    if (!probe.load(text, why)) {
        note_ = "Рисунок «" + svg_rel + "» не открывается: " + why;
        rebuild();
        return false;
    }
    const std::string stem = path_to_utf8(svg.stem());
    const fs::path dir = svg.parent_path();
    std::string base = stem;
    std::error_code ec;
    for (int n = 2; fs::exists(dir / utf8_path(base + ".pixel.json"), ec) || fs::exists(dir / utf8_path(base + ".luau"), ec); ++n)
        base = stem + " " + std::to_string(n);
    pixel::PixelDoc d;
    d.svg = r->id.to_string();
    d.script = base + ".luau";
    d.clips.push_back({states_.empty() ? std::string("stand") : states_.front().id, kNewClipFrames, kNewClipFps, true});
    const fs::path doc_file = dir / utf8_path(base + ".pixel.json");
    std::vector<PixelFiles::File> files;
    files.push_back({dir / utf8_path(d.script), std::nullopt, text_bytes(first_script(base, path_to_utf8(svg.filename()), probe))});
    files.push_back({doc_file, std::nullopt, text_bytes(pixel::write_pixel_doc(d))});
    open_new(doc_file);
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "Векторная: «" + base + "» из «" + path_to_utf8(svg.filename()) + "»"));
    history_.seal();
    note_ = "Сделаны «" + base + ".pixel.json» и скрипт позы «" + base + ".luau» рядом с рисунком.";
    rebuild();
    return has_;
}

bool PixelEditor::create_drawn() {
    note_.clear();
    if (code_unsaved()) {
        note_ = "Код «" + doc_.script + "» не сохранён: сохраните его (Ctrl+S в панели) или отмените набор (Ctrl+Z в панели), потом делайте нового";
        rebuild();
        return false;
    }
    const fs::path dir = assets_.root() / utf8_path("персонажи");
    std::error_code ec;
    const bool had_dir = fs::is_directory(dir, ec);
    std::string base = "Персонаж";
    for (int n = 2; fs::exists(dir / utf8_path(base + ".pixel.json"), ec) || fs::exists(dir / utf8_path(base + ".luau"), ec); ++n)
        base = "Персонаж " + std::to_string(n);
    const std::string state = states_.empty() ? std::string("stand") : states_.front().id;
    const std::string code = drawn_script(base, state);
    pixel::PixelDoc d;
    d.script = base + ".luau";
    d.clips.push_back({state, kNewClipFrames, kNewClipFps, true});
    {
        // The clip's frames as the tab counts them, from its recording.
        pixel::PoseScript probe;
        pixel::Drawing drawing;
        pixel::Rig rig;
        pixel::ClipCut cut;
        std::string why;
        if (probe.load(code) && probe.draw(d.params, d.seed, drawing) && rig.load(pixel::drawing_svg(drawing, d.frame_w, d.frame_h), why) &&
            pixel::cut_clip(probe, rig, d, d.clips.front(), cut, why))
            d.clips.front().frames = cut.frames;
    }
    const fs::path doc_file = dir / utf8_path(base + ".pixel.json");
    std::vector<PixelFiles::File> files;
    files.push_back({dir / utf8_path(d.script), std::nullopt, text_bytes(code)});
    files.push_back({doc_file, std::nullopt, text_bytes(pixel::write_pixel_doc(d))});
    open_new(doc_file);
    // The folder it made goes with the files on Ctrl+Z.
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "Векторная: «" + base + "» — новый, рисунок кодом",
                                                  had_dir ? fs::path() : dir));
    history_.seal();
    note_ = "Сделаны «" + base + ".pixel.json» и «" + base + ".luau» в «персонажи»: рисунок и движение — в коде.";
    rebuild();
    return has_;
}

void PixelEditor::open_new(const fs::path& doc_file) {
    // Opened before the files are written: the command's reload reads them.
    rel_.clear();
    file_ = doc_file;
    {
        std::string rel = path_to_utf8(doc_file.lexically_relative(assets_.root()));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        rel_ = rel;
    }
    doc_ = {};
    rig_ = {};
    script_ = {};
    svg_text_.clear();
    script_text_.clear();
    drawn_ = false;
    drawing_ = {};
    part_.clear();
    clip_.clear();
    forget_character();
}

bool PixelEditor::from_svg(const std::string& svg_rel) {
    const assets::AssetRecord* r = assets_.record_of(assets_.abs(svg_rel));
    if (!r) return false;
    const std::string id = r->id.to_string();
    for (const assets::AssetRecord& d : assets_.ending(".pixel.json")) {
        pixel::PixelDoc doc;
        std::string text, why;
        if (read_text(assets_.abs(d.path), text) && pixel::read_pixel_doc(text, doc, why) && doc.svg == id) {
            open(d.path);
            return has_;
        }
    }
    return create(svg_rel);
}

void PixelEditor::check_files() {
    if (file_.empty()) return;
    fs::path svg; // none watched while the code draws the picture
    if (const std::optional<Guid> id = drawn_ ? std::nullopt : Guid::parse(doc_.svg))
        if (const assets::AssetRecord* r = assets_.find_id(*id)) svg = assets_.abs(r->path);
    if (stamp_of(file_) != doc_watch_.stamp || svg != svg_watch_.file || stamp_of(svg) != svg_watch_.stamp ||
        stamp_of(script_file()) != script_watch_.stamp) {
        modules_changed();
        reload();
    } else if (modules_changed()) {
        // A module the clip requires changed: recorded again.
        script_.forget();
        recount();
        view_dirty_ = true;
        rebuild();
    } else if (sounds_changed()) {
        // A sound appeared, changed or went: its marks, its row and the console, the code not recorded again.
        find_sounds();
        rebuild_console();
        lay_timeline();
    }
}

void PixelEditor::reload() {
    doc_problem_.clear();
    const std::optional<std::vector<u8>> bytes = file_.empty() ? std::nullopt : bytes_of(file_);
    doc_watch_ = {file_, stamp_of(file_)};
    has_ = bytes.has_value();
    if (!has_) {
        rebuild();
        view_dirty_ = true;
        return;
    }
    pixel::PixelDoc d;
    std::string why;
    if (pixel::read_pixel_doc({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, d, why)) doc_ = std::move(d);
    else doc_problem_ = "«" + path_to_utf8(file_.filename()) + "»: " + why;
    load_script();
    load_picture();
    load_drawings();
    pixel::apply_pivots(rig_, doc_, drawn_ ? &drawing_ : nullptr, &others_);
    if (std::none_of(doc_.clips.begin(), doc_.clips.end(), [&](const auto& c) { return c.state == clip_; }))
        clip_ = doc_.clips.empty() ? std::string() : doc_.clips.front().state;
    if (!part_.empty() && rig_.holder(part_).part(part_) < 0) part_.clear();
    recount();
    if (block_ >= 0) select_block(block_); // its info and lines from the recording as it is now
    check_edited();
    view_dirty_ = true;
    rebuild();
}

void PixelEditor::load_picture() {
    draw_problem_.clear();
    picture_note_.clear();
    draw_line_ = 0;
    drawn_ = script_.loaded() && script_.has_draw();
    if (!drawn_) {
        load_svg();
        return;
    }
    svg_problem_.clear();
    svg_watch_ = {};
    // The SVG the file names is there and stays, not drawn: the code's draw is the picture.
    if (!doc_.svg.empty()) {
        const std::optional<Guid> id = Guid::parse(doc_.svg);
        const assets::AssetRecord* r = id ? assets_.find_id(*id) : nullptr;
        const std::string svg = r ? "«" + path_to_utf8(utf8_path(r->path).filename()) + "»" : "рисунок SVG из «" + path_to_utf8(file_.filename()) + "»";
        picture_note_ = "Рисунок — из draw кода «" + doc_.script + "»; " + svg + " не используется";
    }
    pixel::Drawing d;
    if (!script_.draw(doc_.params, doc_.seed, d)) {
        draw_problem_ = "Рисунок «" + doc_.script + "»: " + script_.error() + (rig_.loaded() ? " (показан прежний)" : "");
        draw_line_ = script_.error_line();
        return;
    }
    std::string text = pixel::drawing_svg(d, doc_.frame_w, doc_.frame_h);
    if (text == svg_text_ && rig_.loaded()) {
        drawing_ = std::move(d);
        return;
    }
    pixel::Rig fresh;
    std::string why;
    if (!fresh.load(text, why)) {
        draw_problem_ = "Рисунок «" + doc_.script + "»: " + why + (rig_.loaded() ? " (показан прежний)" : "");
        return;
    }
    rig_ = std::move(fresh);
    svg_text_ = std::move(text);
    drawing_ = std::move(d);
    script_.forget(); // its recordings checked the parts of the picture before
}

void PixelEditor::load_drawings() {
    std::vector<pixel::NamedDrawing> drawn;
    std::string why;
    u32 line = 0;
    if (script_.loaded() && rig_.loaded() && !pixel::draw_drawings(script_, doc_, drawn, why, line)) {
        // The rig keeps the drawings it had: the frame shows what was drawn before.
        draw_problem_ = (draw_problem_.empty() ? "" : draw_problem_ + "\n") + "Рисунок «" + doc_.script + "»: " + why +
                        (others_.empty() ? "" : " (показан прежний)");
        if (!draw_line_) draw_line_ = line;
        return;
    }
    std::string key;
    for (const pixel::NamedDrawing& d : drawn) key += d.name + '\n' + d.svg + '\n';
    if (key == others_key_ && rig_.drawings().size() == drawn.size()) {
        others_ = std::move(drawn);
        return;
    }
    // A drawing gone from the code goes from the rig: the main picture again, the drawings on it.
    pixel::Rig fresh;
    if (!rig_.loaded() || !fresh.load(svg_text_, why)) return;
    if (!pixel::add_drawings(fresh, drawn, why, line)) {
        draw_problem_ = (draw_problem_.empty() ? "" : draw_problem_ + "\n") + "Рисунок «" + doc_.script + "»: " + why;
        if (!draw_line_) draw_line_ = line;
        return;
    }
    rig_ = std::move(fresh);
    others_ = std::move(drawn);
    others_key_ = std::move(key);
    shown_rig_ = nullptr;
    script_.forget(); // its recordings checked the parts of the drawings before
}

void PixelEditor::load_svg() {
    svg_problem_.clear();
    fs::path svg;
    const std::optional<Guid> id = Guid::parse(doc_.svg);
    if (const assets::AssetRecord* r = id ? assets_.find_id(*id) : nullptr) svg = assets_.abs(r->path);
    svg_watch_ = {svg, stamp_of(svg)};
    std::string text;
    if (svg.empty() || !read_text(svg, text)) {
        svg_problem_ = id                  ? "Рисунка SVG этого персонажа нет в «Ресурсах» (его удалили?)"
                       : script_.loaded() ? "Нет рисунка: допишите draw в код или сделайте персонажа из SVG"
                                          : "Рисунок из кода появится, когда код «" + doc_.script + "» загрузится";
        return;
    }
    if (text == svg_text_ && rig_.loaded()) {
        for (const std::string& line : pixel::unsupported(rig_.outline())) svg_problem_ += (svg_problem_.empty() ? "" : "\n") + line;
        return;
    }
    pixel::Rig fresh;
    std::string why;
    if (!fresh.load(text, why)) {
        svg_problem_ = "Рисунок «" + path_to_utf8(svg.filename()) + "»: " + why + (rig_.loaded() ? " (показан прежний)" : "");
        return;
    }
    rig_ = std::move(fresh);
    svg_text_ = std::move(text);
    script_.forget(); // its recordings checked the parts of the picture before
    for (const std::string& line : pixel::unsupported(rig_.outline())) svg_problem_ += (svg_problem_.empty() ? "" : "\n") + line;
}

void PixelEditor::load_script() {
    script_problem_.clear();
    const fs::path file = script_file();
    script_watch_ = {file, stamp_of(file)};
    std::string text;
    if (file.empty() || !read_text(file, text)) {
        script_problem_ = "Скрипта позы «" + doc_.script + "» нет рядом с «" + path_to_utf8(file_.filename()) + "»";
        adopt_code({});
        return;
    }
    adopt_code(text);
    if (text == script_text_ && script_.loaded()) return;
    pixel::PoseScript fresh;
    fresh.set_module_reader(pixel::animations_reader(game_dir));
    if (!fresh.load(text)) {
        script_problem_ = "Скрипт «" + doc_.script + "»: " + fresh.error() + (script_.loaded() ? " (показана последняя удачная поза)" : "");
        error_line_ = fresh.error_line();
        return;
    }
    script_ = std::move(fresh);
    script_text_ = std::move(text);
    bool crlf = false;
    script_panel_ = pixel::panel_text(script_text_, crlf);
    caret_ = ~0u;
    error_line_ = 0;
}

// --- parts and pivots --------------------------------------------------------------

bool PixelEditor::select_part(const std::string& id) {
    if (id == part_ || (!id.empty() && rig_.holder(id).part(id) < 0)) return false; // a part of any drawing (part 4д)
    part_ = id;
    view_dirty_ = true;
    rebuild();
    return true;
}

bool PixelEditor::set_pivot(const std::string& id, vector::Point at) {
    if (!has_ || rig_.holder(id).part(id) < 0 || !std::isfinite(at.x) || !std::isfinite(at.y)) return false;
    at = {std::round(at.x * 100) / 100, std::round(at.y * 100) / 100};
    pixel::PixelDoc after = doc_;
    after.pivots[id] = at;
    return change(after, "«" + name() + "»: опорная точка «" + id + "» " + number_words(at.x) + "; " + number_words(at.y));
}

vector::Point PixelEditor::view_to_picture(f32 x, f32 y) const {
    if (view_w_ <= 0 || view_h_ <= 0) return {};
    const f64 vx = (x - view_x_) * kViewPx / view_w_, vy = (y - view_y_) * kViewPx / view_h_;
    return pixel::inverse(to_view()).apply({vx, vy});
}

bool PixelEditor::screen_of(vector::Point p, f32& x, f32& y) const {
    if (view_w_ <= 0 || view_h_ <= 0 || !rig_.loaded()) return false;
    const vector::Point v = to_view().apply(p);
    x = view_x_ + static_cast<f32>(v.x * view_w_ / kViewPx);
    y = view_y_ + static_cast<f32>(v.y * view_h_ / kViewPx);
    return true;
}

vector::Point PixelEditor::posed_pivot(const std::string& id) const {
    const pixel::Rig& r = rig_.shown();
    const i32 i = r.part(id);
    return i < 0 ? vector::Point{} : r.carried(i).apply(r.pivot(i));
}

void PixelEditor::press(f32 x, f32 y) {
    if (!rig_.loaded()) return;
    if (!part_.empty()) {
        f32 px, py;
        if (screen_of(posed_pivot(part_), px, py) && std::hypot(px - x, py - y) <= kPivotReach * std::max(1.0f, view_w_ / kViewPx)) {
            play(false);
            dragging_ = true;
            drag_moved_ = false;
            drag_doc_ = doc_;
            return;
        }
    }
    const std::string best = part_at(x, y);
    if (rig_.shown().copy(best) >= 0) { // part 4г: a copy is the code's
        refuse_edit("копия «" + best + "» правится в коде: copies(…)");
        return;
    }
    select_part(best);
    // Dragged on past a few points, the part moves: into the code on release.
    moving_ = !best.empty();
    move_started_ = false;
    move_x_ = x, move_y_ = y;
    move_dx_ = move_dy_ = 0;
}

std::string PixelEditor::part_at(f32 x, f32 y) {
    // The smallest part under the mouse, as it stands in the pose shown: a copy drawn too (part 4г), of the drawing
    // shown (in a swap the one it goes to, part 4д).
    pixel::Rig& r = rig_.shown();
    const pixel::Affine view = to_view();
    const vector::Point v = view.apply(view_to_picture(x, y));
    std::string best;
    f64 best_area = 1e300;
    auto take = [&](i32 i, const std::string& id) {
        f64 b[4];
        r.bounds(i, view, b);
        const f64 area = (b[2] - b[0]) * (b[3] - b[1]);
        if (v.x >= b[0] && v.x <= b[2] && v.y >= b[1] && v.y <= b[3] && area > 0 && area < best_area) {
            best_area = area;
            best = id;
        }
    };
    for (i32 i = 0; i < static_cast<i32>(r.parts().size()); ++i) take(i, r.parts()[static_cast<usize>(i)].id);
    for (const pixel::DrawnCopy& c : r.drawn_copies()) take(c.index, c.id);
    return best;
}

void PixelEditor::drag(f32 x, f32 y) {
    if (moving_) {
        if (!move_started_ && std::hypot(x - move_x_, y - move_y_) > kMoveStart * std::max(1.0f, dp_)) start_move();
        if (!move_started_) return;
        // Back through what carries the part: the units of its own move, to tenths.
        const vector::Point a = move_inv_.apply(view_to_picture(x, y)), b = move_inv_.apply(move_from_);
        const f64 dx = std::round((a.x - b.x) * 10) / 10, dy = std::round((a.y - b.y) * 10) / 10;
        if (dx != move_dx_ || dy != move_dy_) {
            move_dx_ = dx, move_dy_ = dy;
            view_dirty_ = true;
        }
        return;
    }
    if (!dragging_) return;
    const i32 i = rig_.shown().part(part_);
    if (i < 0) return;
    // Back through what carries the pivot in the pose shown: it is kept in rest units, one for every drawing.
    const vector::Point at = pixel::inverse(rig_.shown().carried(i)).apply(view_to_picture(x, y));
    rig_.set_pivot(part_, at);
    doc_.pivots[part_] = at; // shown at once; written on release
    drag_moved_ = true;
    view_dirty_ = true;
}

void PixelEditor::release() {
    if (moving_) {
        moving_ = false;
        if (!move_started_) return;
        move_started_ = false;
        const f64 dx = move_dx_, dy = move_dy_;
        move_dx_ = move_dy_ = 0;
        view_dirty_ = true;
        if (dx != 0 || dy != 0) move_part(dx, dy);
        return;
    }
    if (!dragging_) return;
    dragging_ = false;
    if (!drag_moved_) return;
    const vector::Point to = doc_.pivots[part_];
    doc_ = std::move(drag_doc_); // as the file has it: the step is from there
    if (!set_pivot(part_, to)) reload();
}

// --- the file ------------------------------------------------------------------------

bool PixelEditor::change(const pixel::PixelDoc& changed, std::string label) {
    if (!has_) return false;
    // A clip of the code is written with its frames as counted from its range now.
    pixel::PixelDoc after = changed;
    std::string why;
    if (script_.loaded()) pixel::count_frames(script_, rig_, after, why);
    if (after == doc_) return false;
    std::optional<std::vector<u8>> before = bytes_of(file_);
    std::vector<u8> bytes = text_bytes(pixel::write_pixel_doc(after));
    if (before && *before == bytes) return false;
    note_.clear();
    std::vector<PixelFiles::File> files;
    files.push_back({file_, std::move(before), std::move(bytes)});
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), std::move(label)));
    history_.seal();
    return true;
}

bool PixelEditor::set_param(const std::string& name_, f64 value) {
    const auto& declared = script_.params();
    if (!std::isfinite(value) || std::none_of(declared.begin(), declared.end(), [&](const auto& p) { return p.name == name_; })) return false;
    pixel::PixelDoc after = doc_;
    after.params[name_] = value;
    return change(after, "«" + name() + "»: " + name_ + " = " + number_words(value));
}

u32 PixelEditor::frame_limit() const {
    const objects::Library* lib = module_.library();
    return lib ? lib->frame_limit() : objects::kDefaultFrameLimit;
}

bool PixelEditor::set_frame_limit(u32 n) {
    objects::Library* lib = module_.library();
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild(); // the field shows the limit again
        return false;
    };
    if (!lib) return false;
    if (n < 1 || n > objects::kMaxFrameLimit) return refuse("Предел кадров — целое от 1 до " + std::to_string(objects::kMaxFrameLimit));
    const u32 was = lib->frame_limit();
    if (n == was) return refuse("");
    // Not below what the game takes already: who takes the most.
    u32 most = 0;
    std::string whose;
    auto take = [&](u32 frames, std::string who) {
        if (frames <= most) return;
        most = frames;
        whose = std::move(who);
    };
    for (const objects::Template& t : lib->templates()) {
        take(t.frames, "у «" + t.name + "»");
        for (const auto& [state, clip] : t.animations)
            take(static_cast<u32>(clip.frames.size()), "у «" + t.name + "» в «" + state_name(state) + "»");
    }
    if (has_) take(total_frames(), "у «" + name() + "» в клипах");
    if (n < most) return refuse(whose + " " + count_words(most, "кадр", "кадра", "кадров") + ": предел не ниже " + std::to_string(most));
    const fs::path file = game_dir / "game.json";
    const std::optional<std::vector<u8>> before = game_dir.empty() ? std::nullopt : bytes_of(file);
    std::string after;
    if (!before || !objects::with_frame_limit({reinterpret_cast<const char*>(before->data()), before->size()}, n, after))
        return refuse("game.json игры не читается: предел кадров не записан");
    note_.clear();
    if (!history_.execute(std::make_unique<LimitCommand>(*this, *lib, file, LimitCommand::Side{*before, was},
                                                         LimitCommand::Side{text_bytes(after), n}, "Предел кадров " + std::to_string(n))))
        return false; // said by the step itself
    history_.seal();
    return true;
}

void PixelEditor::refused(std::string why) {
    note_ = std::move(why);
    rebuild(); // the fields show things as they are
}

bool PixelEditor::set_seed(u64 seed) {
    pixel::PixelDoc after = doc_;
    after.seed = seed;
    return change(after, "«" + name() + "»: seed " + std::to_string(seed));
}

pixel::ClipSpec* PixelEditor::selected_clip() {
    for (pixel::ClipSpec& c : doc_.clips)
        if (c.state == clip_) return &c;
    return nullptr;
}

const pixel::ClipSpec* PixelEditor::selected_clip() const { return const_cast<PixelEditor*>(this)->selected_clip(); }

bool PixelEditor::select_clip(const std::string& state) {
    if (state == clip_ || std::none_of(doc_.clips.begin(), doc_.clips.end(), [&](const auto& c) { return c.state == state; })) return false;
    clip_ = state;
    t_ = play_clock_ = 0;
    frame_ = 0;
    block_ = -1;
    tv_.scale = tv_.start = 0; // all of the clip in sight
    hush();
    if (!note_.empty() && note_ == refused_) note_.clear(); // a mouse edit's refusal was about the clip left
    recount();
    select_block(-1);
    view_dirty_ = true;
    rebuild();
    return true;
}

bool PixelEditor::add_clip(const std::string& state) {
    if (!has_ || std::none_of(states_.begin(), states_.end(), [&](const auto& s) { return s.id == state; })) return false;
    if (std::any_of(doc_.clips.begin(), doc_.clips.end(), [&](const auto& c) { return c.state == state; })) return select_clip(state);
    const u32 total = total_frames(), limit = frame_limit();
    if (total >= limit) {
        note_ = "В клипах уже " + count_words(total, "кадр", "кадра", "кадров") + ": это предел игры («Предел кадров» ниже)";
        rebuild();
        return false;
    }
    pixel::PixelDoc after = doc_;
    after.clips.push_back({state, std::min(kNewClipFrames, limit - total), kNewClipFps, true});
    clip_ = state;
    t_ = play_clock_ = 0;
    frame_ = 0;
    block_ = -1;
    tv_.scale = tv_.start = 0;
    hush();
    return change(after, "«" + name() + "»: клип «" + state_name(state) + "»");
}

bool PixelEditor::remove_clip() {
    const pixel::ClipSpec* c = selected_clip();
    if (!c) return false;
    pixel::PixelDoc after = doc_;
    const std::string state = c->state;
    after.clips.erase(std::remove_if(after.clips.begin(), after.clips.end(), [&](const auto& x) { return x.state == state; }), after.clips.end());
    clip_.clear();
    hush();
    return change(after, "«" + name() + "»: клип «" + state_name(state) + "» убран");
}

bool PixelEditor::set_clip_frames(int n) {
    const pixel::ClipSpec* c = selected_clip();
    if (!c) return false;
    if (cut_.code) {
        note_ = "Кадры клипа из кода считаются по его диапазону и «кадров в секунду»";
        rebuild();
        return false;
    }
    u32 others = 0;
    for (const pixel::ClipSpec& x : doc_.clips)
        if (x.state != c->state) others += clip_frames(x.state);
    const u32 limit = frame_limit();
    const u32 frames = static_cast<u32>(std::clamp(n, 1, static_cast<int>(limit - std::min(others, limit - 1))));
    if (n > static_cast<int>(frames)) {
        note_ = "Во всех клипах вместе не больше " + count_words(limit, "кадра", "кадров", "кадров") + ": это предел игры («Предел кадров» ниже)";
        rebuild();
    }
    if (frames == c->frames) return false;
    pixel::PixelDoc after = doc_;
    for (pixel::ClipSpec& x : after.clips)
        if (x.state == c->state) x.frames = frames;
    t_ = std::min(t_, (frames - 1) / static_cast<f64>(c->fps));
    return change(after, "«" + name() + "»: «" + state_name(c->state) + "» " + count_words(frames, "кадр", "кадра", "кадров"));
}

bool PixelEditor::set_clip_fps(f32 fps) {
    const pixel::ClipSpec* c = selected_clip();
    if (!c || objects::clip_ticks(fps) == 0) return false;
    pixel::PixelDoc after = doc_;
    for (pixel::ClipSpec& x : after.clips)
        if (x.state == c->state) x.fps = fps;
    return change(after, "«" + name() + "»: «" + state_name(c->state) + "» " + number_words(fps) + " к/с");
}

bool PixelEditor::set_clip_loop(bool on) {
    const pixel::ClipSpec* c = selected_clip();
    if (!c) return false;
    pixel::PixelDoc after = doc_;
    for (pixel::ClipSpec& x : after.clips)
        if (x.state == c->state) x.loop = on;
    return change(after, "«" + name() + "»: «" + state_name(c->state) + "» " + (on ? "повторяется" : "без повтора"));
}

bool PixelEditor::set_frame_size(u32 w, u32 h) {
    if (w < 1 || h < 1 || w > pixel::kMaxFrameSide || h > pixel::kMaxFrameSide) return false;
    pixel::PixelDoc after = doc_;
    after.frame_w = w, after.frame_h = h;
    return change(after, "«" + name() + "»: кадр " + std::to_string(w) + " × " + std::to_string(h));
}

bool PixelEditor::set_game_palette(bool on) {
    if (on && (!game_colors || game_colors().empty())) {
        note_ = "В игре пока нет своих цветов: их задают в «Интерфейсе», «Компоненты» → «Цвета игры»";
        rebuild();
        return false;
    }
    pixel::PixelDoc after = doc_;
    after.game_palette = on;
    return change(after, "«" + name() + "»: " + (on ? "цвета игры" : "цвета рисунка"));
}

bool PixelEditor::set_outline(bool on) {
    pixel::PixelDoc after = doc_;
    after.outline = on;
    return change(after, "«" + name() + "»: " + (on ? "контур" : "без контура"));
}

bool PixelEditor::set_outline_color(const std::string& text) {
    pixel::Rgba8 c;
    if (!pixel::parse_color(text, c)) {
        note_ = "Цвет контура: \"#rrggbb\"";
        rebuild();
        return false;
    }
    pixel::PixelDoc after = doc_;
    after.outline_color = c;
    return change(after, "«" + name() + "»: цвет контура " + text);
}

// --- the clock ---------------------------------------------------------------------------

void PixelEditor::play(bool on) {
    // From the playhead, or the range's start when it stands outside the range.
    if (on && !playing_) {
        play_clock_ = t_ - cut_.from;
        if (play_clock_ < -1e-9 || t_ > cut_.to + 1e-9) play_clock_ = 0;
        play_clock_ = std::max(0.0, play_clock_);
        fps_frames_ = 0;
        fps_clock_ = 0;
        // The frame it starts from sounds too.
        sounded_.clear();
        sound_shown_ = static_cast<i64>(std::floor(play_clock_ * cut_.fps + 1e-9)) - 1;
    }
    if (!on) mixer_.stop_all();
    playing_ = on;
    m_playing_ = on;
    dirty("px_playing");
}

void PixelEditor::step(int frames) {
    play(false);
    const pixel::ClipSpec* c = selected_clip();
    if (!c || whole_.frames == 0) return;
    const i64 n = whole_.frames, at = static_cast<i64>(frame_) + frames;
    set_frame(static_cast<u32>(c->loop ? ((at % n) + n) % n : std::clamp<i64>(at, 0, n - 1)));
}

void PixelEditor::set_frame(u32 k) {
    if (!selected_clip() || k >= whole_.frames) return;
    t_ = whole_.time(k);
    sync_time();
    view_dirty_ = true;
}

void PixelEditor::set_onion(bool on) {
    onion_ = on;
    m_onion_ = on;
    view_dirty_ = true;
    dirty("px_onion");
}

// --- baking and giving -------------------------------------------------------------------

std::string PixelEditor::picture() const {
    const std::optional<Guid> id = Guid::parse(doc_.picture);
    const assets::AssetRecord* r = id ? assets_.find_id(*id) : nullptr;
    return r ? r->path : std::string();
}

bool PixelEditor::bake(bool over_edits) {
    note_.clear();
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return false;
    };
    if (!has_ || !rig_.loaded()) return refuse("Нечего запекать: рисунок не открыт");
    if (!script_.loaded()) return refuse("Нечего запекать: скрипт позы не работает");
    if (assets_.looking()) return refuse("«Ресурсы» ещё смотрят файлы: запеките через миг");
    const std::vector<pixel::Rgba8> colors = doc_.game_palette && game_colors ? game_colors() : std::vector<pixel::Rgba8>{};
    pixel::Strip strip;
    std::string why;
    if (!pixel::bake_strip(rig_, script_, doc_, pixel::bake_options(doc_, colors), frame_limit(), strip, why)) {
        view_dirty_ = true; // the rig is at rest now
        return refuse("Не запеклось: " + why);
    }
    view_dirty_ = true;
    std::vector<u8> png;
    if (!assets::encode_image(strip.picture, ".png", png)) return refuse("Картинка не кодируется в PNG");

    // The same asset again when there is one: the game's copies follow it.
    std::optional<Guid> id = Guid::parse(doc_.picture);
    const assets::AssetRecord* was = id ? assets_.find_id(*id) : nullptr;
    std::vector<PixelFiles::File> files;
    fs::path target;
    if (was) {
        target = assets_.abs(was->path);
        std::optional<std::vector<u8>> before = bytes_of(target);
        if (before && !doc_.hash.empty() && editor::sources::hash_of(*before) != doc_.hash && !over_edits) {
            edited_ = m_edited_ = true;
            dirty("px_edited");
            return refuse("Картинку «" + path_to_utf8(target.filename()) + "» правили после запекания: новое запекание заменит правки");
        }
        if (before && *before == png && doc_.strip == strip.clips) {
            note_ = "Картинка «" + path_to_utf8(target.filename()) + "» уже такая";
            rebuild();
            return false;
        }
        files.push_back({target, std::move(before), png});
    } else {
        const fs::path dir = file_.parent_path();
        const std::string stem = name();
        target = dir / utf8_path(stem + ".png");
        std::error_code ec;
        for (int n = 2; fs::exists(target, ec); ++n) target = dir / utf8_path(stem + " " + std::to_string(n) + ".png");
        // A .meta of its own beside it, so the picture's Guid is known now (an earlier bake's, when its file is gone).
        if (!id) id = Guid::generate();
        assets::AssetMeta meta;
        meta.id = *id;
        fs::path meta_file = target;
        meta_file += ".meta";
        files.push_back({target, std::nullopt, png});
        files.push_back({meta_file, std::nullopt, text_bytes(data::to_json(meta))});
    }
    pixel::PixelDoc after = doc_;
    after.picture = id->to_string();
    after.hash = editor::sources::hash_of(png);
    after.strip = strip.clips;
    files.push_back({file_, bytes_of(file_), text_bytes(pixel::write_pixel_doc(after))});
    const std::string label = "«" + name() + "»: запечено в «" + path_to_utf8(target.filename()) + "», " +
                              count_words(strip.frames, "кадр", "кадра", "кадров");
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), label));
    history_.seal();
    note_ = "Запечено: «" + path_to_utf8(target.filename()) + "», " + count_words(strip.frames, "кадр", "кадра", "кадров") + " " +
            std::to_string(doc_.frame_w) + " × " + std::to_string(doc_.frame_h) +
            (doc_.game_palette && colors.empty() ? " (в игре нет своих цветов: в цветах рисунка)." : ".");
    FORGE_INFO("Векторная: %s", note_.c_str());
    rebuild();
    return true;
}

void PixelEditor::open_give(bool on) {
    giving_ = on && has_;
    rebuild();
}

bool PixelEditor::give(u64 key) {
    note_.clear();
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return false;
    };
    objects::Library* lib = module_.library();
    const objects::Template* t = lib ? lib->find(key) : nullptr;
    if (!t) return refuse("Такого шаблона в игре нет");
    const std::string pic = picture();
    if (pic.empty() || doc_.strip.empty()) return refuse("Сначала «Запечь»: картинки персонажа ещё нет");
    if (assets_.looking()) return refuse("«Ресурсы» ещё смотрят файлы: отдайте через миг");
    u32 frames = 0;
    for (const pixel::StripClip& c : doc_.strip) frames = std::max(frames, c.first + c.frames);
    // Baked under a higher limit: the game would read it as one picture.
    if (frames > lib->frame_limit())
        return refuse("В «" + path_to_utf8(utf8_path(pic).filename()) + "» " + count_words(frames, "кадр", "кадра", "кадров") +
                      ", а предел игры " + std::to_string(lib->frame_limit()) + ": поднимите «Предел кадров» или уберите кадры у клипов и запеките снова");
    const std::string copied = copy_into(assets_.abs(pic), lib->pictures_folder(), "картинка", copy_in);
    if (copied.empty()) return refuse("Картинка «" + pic + "» не скопировалась в игру");
    // The states the template plays, as it will be drawn.
    objects::Template probe = *t;
    probe.picture = copied;
    probe.frames = frames;
    const std::vector<level::LevelModule::AnimState> plays = module_.anim_states(*lib, probe);
    GiveCommand::Look before{t->picture, t->frames, t->animations, t->frames_over, t->animations_over}, after{copied, frames, {}, 0, {}};
    std::string given, left;
    for (const pixel::StripClip& c : doc_.strip) {
        const auto st = std::find_if(plays.begin(), plays.end(), [&](const auto& s) { return s.id == c.state; });
        objects::Clip clip;
        for (u32 f = 0; f < c.frames; ++f) clip.frames.push_back(c.first + f);
        clip.fps = c.fps;
        clip.loop = c.loop;
        const std::string why = st == plays.end() ? std::string("его шаблон не играет") : objects::clip_problem(clip, frames, lib->frame_limit());
        if (!why.empty()) {
            left += (left.empty() ? "" : ", ") + ("«" + state_name(c.state) + "» (" + why + ")");
            continue;
        }
        after.animations[c.state] = std::move(clip);
        given += (given.empty() ? "" : ", ") + ("«" + st->name + "»");
    }
    if (after.picture == before.picture && after.frames == before.frames && after.animations == before.animations &&
        before.frames_over == 0 && before.animations_over.empty()) {
        note_ = "«" + t->name + "» уже рисует эту картинку так";
        giving_ = false;
        rebuild();
        return false;
    }
    history_.execute(std::make_unique<GiveCommand>(*lib, key, std::move(before), std::move(after),
                                                   "«" + t->name + "»: картинка «" + copied + "» из «" + name() + "»"));
    history_.seal();
    giving_ = false;
    note_ = "«" + t->name + "» рисует «" + copied + "», " + count_words(frames, "кадр", "кадра", "кадров") +
            (given.empty() ? ", своих анимаций нет" : ", анимации: " + given) + (left.empty() ? "" : "; не отданы: " + left) + ".";
    FORGE_INFO("Векторная: %s", note_.c_str());
    rebuild();
    return true;
}

// --- the views -------------------------------------------------------------------------------

void PixelEditor::redraw() {
    view_dirty_ = false;
    pose_problem_.clear();
    fit_box(); // the zoom kept within the picture as it is now
    if (!has_ || !rig_.loaded()) {
        vector_ = frame_px_ = vector_shown_ = {};
        show({}, {});
        sync_problem();
        return;
    }
    const std::vector<pixel::Rgba8> colors = doc_.game_palette && game_colors ? game_colors() : std::vector<pixel::Rgba8>{};
    const pixel::BakeOptions options = pixel::bake_options(doc_, colors);
    const pixel::ClipSpec* c = selected_clip();
    const bool scripted = c && script_.loaded();
    std::string why;

    // The frames shown: the playhead's, and those either side of it for «Калька» (the whole clip's frames).
    std::vector<u32> ks{frame_};
    std::vector<f64> ts{t_};
    if (onion_ && c && whole_.frames > 1) {
        const i64 n = whole_.frames;
        for (i64 d : {-1, 1}) {
            i64 k = static_cast<i64>(frame_) + d;
            if (c->loop) k = ((k % n) + n) % n;
            else if (k < 0 || k >= n) continue;
            ks.push_back(static_cast<u32>(k));
            ts.push_back(whole_.time(static_cast<u32>(k)));
        }
    }
    std::vector<pixel::Pose> poses(ks.size());
    if (scripted)
        for (usize i = 0; i < ks.size(); ++i)
            if (!pose_at(*c, ts[i], ks[i], poses[i], why)) {
                pose_problem_ = "Скрипт «" + doc_.script + "»: " + why;
                error_line_ = script_.error_line();
                if (!vector_.rgba8.empty()) { // the views stay on the last pose that worked
                    sync_problem();
                    return;
                }
                poses.assign(ks.size(), {});
                break;
            }
    if (pose_problem_.empty() && script_problem_.empty()) error_line_ = 0;
    // The part being dragged, moved by the mouse so far (its code changes on release).
    if (move_started_ && !part_.empty() && (move_dx_ != 0 || move_dy_ != 0)) {
        pixel::PartPose& pp = poses[0].parts[part_];
        pp.x += move_dx_;
        pp.y += move_dy_;
    }
    shown_pose_ = poses[0];
    shown_ok_ = scripted && pose_problem_.empty();

    // Pixel frames as they bake: the rig's moves in whole pixels.
    const f64 scale = rig_.to_pixels(doc_.frame_w, doc_.frame_h).a;
    std::vector<assets::CookedTexture> px(ks.size());
    for (usize i = 0; i < ks.size(); ++i) {
        rig_.pose(poses[i], why, scale > 0 ? 1 / scale : 0);
        px[i] = rig_.bake(doc_.frame_w, doc_.frame_h, options);
    }
    // The vector, smooth; the shown pose last, so the pivot is found in it.
    std::vector<assets::CookedTexture> vec(ks.size());
    const pixel::Affine texels = to_texels();
    for (usize i = ks.size(); i-- > 0;) {
        if (!rig_.pose(poses[i], why) && i == 0 && pose_problem_.empty()) pose_problem_ = why;
        vec[i] = rig_.draw(view_px(), view_px(), texels);
    }
    for (usize i = 1; i < ks.size(); ++i) {
        over_faint(vec[0], vec[i], kOnion);
        over_faint(px[0], px[i], kOnion);
    }
    frame_px_ = px[0];
    vector_ = vec[0];
    // The grid over the picture, under the marks: its cell 8 points or more.
    if (grid_) pixel::draw_grid(vec[0], texels, pixel::grid_step(to_view().a), static_cast<u32>(std::max<i64>(1, std::lround(dp_))));
    // Marks on the drawing shown (part 4д); the tree is of it too, made again when the frame shows another.
    pixel::Rig& shown = rig_.shown();
    if (const i32 pi = shown.part(part_); pi >= 0) {
        f64 box[4];
        shown.bounds(pi, texels, box);
        const vector::Point at = texels.apply(shown.carried(pi).apply(shown.pivot(pi)));
        mark(vec[0], box, at.x, at.y, std::max<i64>(1, std::lround(dp_)));
    }
    if (const i32 hi = shown.part(hovered_); hi >= 0) {
        f64 box[4];
        shown.bounds(hi, texels, box);
        outline(vec[0], box, std::max<i64>(1, std::lround(dp_)));
    }
    // The copy of the selected block, where it stands (part 4г).
    if (rec_ok_ && block_ >= 0 && static_cast<usize>(block_) < rec_.blocks.size() && rec_.blocks[static_cast<usize>(block_)].part < rec_.parts.size())
        for (const pixel::DrawnCopy& c : shown.drawn_copies())
            if (c.id == rec_.parts[rec_.blocks[static_cast<usize>(block_)].part]) {
                f64 box[4];
                shown.bounds(c.index, texels, box);
                outline(vec[0], box, std::max<i64>(1, std::lround(dp_)));
            }
    vector_shown_ = vec[0];
    show(vec[0], frame_px_);
    sync_problem();
    if (&shown != shown_rig_) {
        shown_rig_ = &shown;
        rebuild();
    }
    rebuild_part();
}

void PixelEditor::show(const assets::CookedTexture& vec, const assets::CookedTexture& px) {
    if (!ui_) return;
    // The pictures before the last ones are shown by nothing now: the views took the last ones at the UI's update.
    for (std::string& old : images_[1])
        if (!old.empty()) ui_->drop_image(old);
    for (usize i = 0; i < 3; ++i) images_[1][i] = images_[0][i], images_[0][i].clear();
    m_vector_ = m_pixel_ = m_one_ = "";
    if (!vec.rgba8.empty() && !px.rgba8.empty()) {
        const std::string n = std::to_string(++image_serial_);
        const u32 s = std::max(1u, view_px() / std::max(px.width, px.height));
        const assets::CookedTexture big = bigger(px, s);
        images_[0][0] = "px_v_" + n, images_[0][1] = "px_p_" + n, images_[0][2] = "px_1_" + n;
        ui_->set_image(images_[0][0], vec.rgba8.data(), vec.width, vec.height);
        ui_->set_image(images_[0][1], big.rgba8.data(), big.width, big.height);
        ui_->set_image(images_[0][2], px.rgba8.data(), px.width, px.height);
        m_vector_ = "/memory/" + images_[0][0];
        m_pixel_ = "/memory/" + images_[0][1];
        m_one_ = "/memory/" + images_[0][2];
    }
    dirty("px_vector");
    dirty("px_pixel");
    dirty("px_one");
}

void PixelEditor::rebuild_list() {
    std::vector<std::string> listed;
    std::vector<Row> rows;
    for (const assets::AssetRecord& r : assets_.ending(".pixel.json")) {
        listed.push_back(r.path);
        pixel::PixelDoc d;
        std::string text, why, sub;
        if (read_text(assets_.abs(r.path), text) && pixel::read_pixel_doc(text, d, why))
            sub = std::to_string(d.frame_w) + " × " + std::to_string(d.frame_h) + " · " +
                  count_words(static_cast<u32>(d.clips.size()), "клип", "клипа", "клипов");
        else sub = "не читается";
        rows.push_back({stem_of(path_to_utf8(utf8_path(r.path).filename())), sub, r.path == rel_});
    }
    std::vector<std::string> svgs;
    std::vector<SvgRow> svg_rows;
    for (const assets::AssetRecord& r : assets_.ending(".svg")) {
        svgs.push_back(r.path);
        const fs::path p = utf8_path(r.path);
        svg_rows.push_back({path_to_utf8(p.filename()), p.has_parent_path() ? path_to_utf8(p.parent_path()) : std::string("«Ресурсы»")});
    }
    listed_ = std::move(listed);
    svgs_ = std::move(svgs);
    m_list_ = std::move(rows);
    m_svgs_ = std::move(svg_rows);
    dirty("px_list");
    dirty("px_svgs");
}

void PixelEditor::rebuild_states() {
    states_.clear();
    templates_.clear();
    objects::Library* lib = module_.library();
    if (!lib) return;
    std::vector<const objects::Template*> all;
    for (const objects::Template& t : lib->templates()) {
        all.push_back(&t);
        objects::Template probe = t;
        if (probe.picture.empty()) probe.picture = "?.png"; // the states it would play with a picture
        for (const auto& s : module_.anim_states(*lib, probe))
            if (std::none_of(states_.begin(), states_.end(), [&](const auto& x) { return x.id == s.id; })) states_.push_back(s);
    }
    std::sort(all.begin(), all.end(), [](const auto* a, const auto* b) {
        const std::string ka = alphabet_key(a->name), kb = alphabet_key(b->name);
        return ka != kb ? ka < kb : a->name < b->name;
    });
    for (const objects::Template* t : all) templates_.push_back(t->key);
}

void PixelEditor::sync_problem() {
    std::string all;
    for (const std::string* p : {&doc_problem_, &svg_problem_, &draw_problem_, &script_problem_, &pose_problem_})
        if (!p->empty()) all += (all.empty() ? "" : "\n") + *p;
    if (all == problem_ && m_problem_ == all) return;
    problem_ = all;
    m_problem_ = all;
    dirty("px_problem");
}

void PixelEditor::check_edited() {
    // Changed since it was baked («Изображения», another program): baking again would lose that.
    const std::string pic = picture();
    const std::optional<std::vector<u8>> now = pic.empty() || doc_.hash.empty() ? std::nullopt : bytes_of(assets_.abs(pic));
    const bool edited = now && editor::sources::hash_of(*now) != doc_.hash;
    if (edited == edited_ && m_edited_ == edited) return;
    edited_ = m_edited_ = edited;
    dirty("px_edited");
}

void PixelEditor::rebuild() {
    sync_problem();
    for (Row& r : m_list_) r.selected = false;
    for (usize i = 0; i < listed_.size() && i < m_list_.size(); ++i) m_list_[i].selected = listed_[i] == rel_;
    m_has_ = has_;
    m_name_ = has_ ? name() : std::string();
    m_note_ = note_;
    m_parts_.clear();
    // The parts of the drawing the frame shows (part 4д).
    const pixel::Rig& shown = rig_.shown();
    for (const pixel::Part& p : shown.parts()) {
        int depth = 0;
        for (i32 q = p.parent; q >= 0; q = shown.parts()[static_cast<usize>(q)].parent) ++depth;
        const vector::Point pv = shown.pivot(shown.part(p.id));
        m_parts_.push_back({p.id, number_words(std::round(pv.x * 10) / 10) + "; " + number_words(std::round(pv.y * 10) / 10), depth, p.shape,
                            p.id == part_});
    }
    m_params_.clear();
    for (const pixel::ScriptParam& p : script_.params()) {
        const auto it = doc_.params.find(p.name);
        m_params_.push_back({p.name, number_words(it != doc_.params.end() ? it->second : p.value)});
    }
    m_seed_ = std::to_string(doc_.seed);
    m_states_.clear();
    const u32 total = total_frames();
    for (const auto& s : states_) {
        const auto c = std::find_if(doc_.clips.begin(), doc_.clips.end(), [&](const auto& x) { return x.state == s.id; });
        m_states_.push_back({s.id, s.name,
                             c == doc_.clips.end() ? std::string("нет клипа")
                                                   : count_words(clip_frames(c->state), "кадр", "кадра", "кадров") + " · " + number_words(c->fps) +
                                                         " к/с" + (c->loop ? "" : " · без повтора") +
                                                         " · средняя кнопка: диапазон на весь клип",
                             c != doc_.clips.end(), s.id == clip_});
    }
    // Clips of states the game's templates do not have (another module's file): shown all the same.
    for (const pixel::ClipSpec& c : doc_.clips)
        if (std::none_of(states_.begin(), states_.end(), [&](const auto& s) { return s.id == c.state; }))
            m_states_.push_back({c.state, c.state, count_words(clip_frames(c.state), "кадр", "кадра", "кадров") + " · шаблоны игры этого не играют",
                                 true, c.state == clip_});
    const pixel::ClipSpec* c = selected_clip();
    m_has_clip_ = c != nullptr;
    m_clip_name_.clear();
    for (const StateChip& s : m_states_)
        if (s.selected) m_clip_name_ = s.name;
    m_clip_frames_ = c ? static_cast<int>(clip_frames(c->state)) : 0;
    m_clip_loop_ = c ? c->loop : true;
    for (usize i = 0; i < m_fps_.size(); ++i) m_fps_[i].selected = c && std::fabs(objects::kClipFps[i] - c->fps) < 1e-4f;
    m_code_clip_ = c && cut_.code;
    m_clip_cut_ = m_code_clip_ ? count_words(cut_.frames, "кадр", "кадра", "кадров") + ": с " + seconds_words(cut_.from) + " по " +
                                     seconds_words(cut_.to) + " с" + (c->ranged ? "" : ", весь клип")
                               : std::string();
    m_has_rec_ = rec_ok_;
    m_stale_ = rec_ok_ && rec_stale_;
    m_has_range_ = m_code_clip_;
    m_code_name_ = doc_.script;
    m_code_unsaved_ = code_unsaved();
    m_code_conflict_ = code_conflict_;
    m_loop_view_ = loop_view_;
    sync_time();
    lay_timeline();
    rebuild_console();
    rebuild_gutter();
    rebuild_inspector();
    m_total_ = "в картинке " + count_words(total, "кадр", "кадра", "кадров") + " из " + std::to_string(frame_limit());
    m_limit_ = std::to_string(frame_limit());
    m_fw_ = std::to_string(doc_.frame_w);
    m_fh_ = std::to_string(doc_.frame_h);
    m_size_ = std::to_string(doc_.frame_w) + " × " + std::to_string(doc_.frame_h);
    m_game_palette_ = doc_.game_palette;
    m_outline_ = doc_.outline;
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", doc_.outline_color.r, doc_.outline_color.g, doc_.outline_color.b);
    m_outline_color_ = hex;
    const std::string pic = picture();
    m_picture_ = pic;
    m_can_give_ = !pic.empty() && !doc_.strip.empty();
    giving_ = giving_ && has_;
    m_giving_ = giving_;
    m_templates_.clear();
    if (objects::Library* lib = module_.library()) {
        Guid id;
        if (const std::optional<Guid> g = Guid::parse(doc_.picture)) id = *g;
        const std::string copy = game_copy && !id.is_null() ? game_copy(id) : std::string();
        for (u64 key : templates_) {
            const objects::Template* t = lib->find(key);
            if (!t) continue;
            objects::Template probe = *t;
            if (probe.picture.empty()) probe.picture = "?.png";
            const std::vector<level::LevelModule::AnimState> states = module_.anim_states(*lib, probe);
            std::string plays;
            for (const pixel::StripClip& sc : doc_.strip)
                for (const auto& st : states)
                    if (st.id == sc.state) plays += (plays.empty() ? "" : ", ") + st.name;
            const bool draws = !copy.empty() && t->picture == copy;
            m_templates_.push_back({t->name, template_icon ? template_icon(*t) : std::string(),
                                    draws ? "рисует её" + (plays.empty() ? std::string() : ": " + plays)
                                          : plays.empty() ? std::string("запечённых клипов не играет")
                                                          : "сыграет: " + plays,
                                    draws});
        }
    }
    m_playing_ = playing_;
    m_onion_ = onion_;
    for (const char* var : {"px_list", "px_parts", "px_params", "px_states", "px_frames", "px_fps", "px_templates", "px_name", "px_note", "px_frame_label", "px_clip_name", "px_seed", "px_fw", "px_fh", "px_outline_color", "px_picture", "px_size",
                            "px_total", "px_limit", "px_clip_frames", "px_has", "px_has_clip", "px_playing", "px_onion", "px_game_palette", "px_outline",
                            "px_clip_loop", "px_giving", "px_can_give", "px_code_clip", "px_clip_cut", "px_has_rec", "px_stale", "px_has_range",
                            "px_code_name", "px_code_unsaved", "px_code_conflict", "px_loop_view"})
        dirty(var);
}

} // namespace forge::editor_app
