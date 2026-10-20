// «Векторная», part 3а of step 3 of docs/models/движение-модель.md: the code panel and its console, the player and
// the timeline (pixel_editor.h). The rest of the mode is pixel_editor.cpp.

#include "pixel_editor.h"
#include "pixel_files.h"

#include "asset_library.h"

#include "forge/assets/asset_pipeline.h"
#include "forge/assets/image.h"
#include "forge/data/json.h"

#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <tuple>

namespace fs = std::filesystem;

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr f64 kTickGap = 56;     // logical px between the ruler's marks at least
constexpr f64 kCellLeast = 6;    // a frame's cell is drawn while a frame is this wide (logical px)
constexpr f64 kCellNumber = 18;  // and numbered from this wide
constexpr f64 kRowPx = 20;       // a track's row (logical px)
constexpr usize kMostBlocks = 2000;
constexpr f64 kWheelPan = 40;    // logical px a notch of Shift+wheel moves the timeline
constexpr f64 kWheelZoom = 1.25; // a notch of the wheel

// Shown again by name: RmlUi's style attribute adds properties and never drops one, so «display: none;» stays until
// another display replaces it.
std::string place(f64 left, f64 width) {
    char buf[112];
    std::snprintf(buf, sizeof buf, "display: block; left: %.1fdevpx; width: %.1fdevpx;", left, std::max(0.0, width));
    return buf;
}

std::string at_x(f64 left) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "display: block; left: %.1fdevpx;", left);
    return buf;
}

// The length of the UTF-8 character that starts with byte c.
u32 utf8_length(u8 c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

std::string timing_words(const pixel::Recording& rec, const pixel::Block& b) {
    if (b.to == pixel::Block::To::Binding) return "связь: функция кода в каждом кадре";
    if (b.spring) return "пружина";
    if (!b.frames.empty()) return "tween: кадры функции";
    if (b.curve >= 0 && b.curve < static_cast<i32>(rec.curves.size())) return "своя кривая";
    return vector::timing_word(b.timing);
}

} // namespace

// --- the model -------------------------------------------------------------------------------

void PixelEditor::bind_motion(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Tick>()) {
        s.RegisterMember("style", &Tick::style);
        s.RegisterMember("label", &Tick::label);
    }
    model.RegisterArray<std::vector<Tick>>();
    if (auto s = model.RegisterStruct<FlagRow>()) {
        s.RegisterMember("index", &FlagRow::index);
        s.RegisterMember("style", &FlagRow::style);
        s.RegisterMember("trail", &FlagRow::trail);
        s.RegisterMember("label", &FlagRow::label);
        s.RegisterMember("title", &FlagRow::title);
    }
    model.RegisterArray<std::vector<FlagRow>>();
    if (auto s = model.RegisterStruct<MarkRow>()) {
        s.RegisterMember("index", &MarkRow::index);
        s.RegisterMember("style", &MarkRow::style);
        s.RegisterMember("title", &MarkRow::title);
        s.RegisterMember("sound", &MarkRow::sound);
        s.RegisterMember("span", &MarkRow::span);
        s.RegisterMember("missing", &MarkRow::missing);
    }
    model.RegisterArray<std::vector<MarkRow>>();
    if (auto s = model.RegisterStruct<PauseRow>()) {
        s.RegisterMember("index", &PauseRow::index);
        s.RegisterMember("style", &PauseRow::style);
        s.RegisterMember("label", &PauseRow::label);
        s.RegisterMember("title", &PauseRow::title);
    }
    model.RegisterArray<std::vector<PauseRow>>();
    if (auto s = model.RegisterStruct<TrackRow>()) {
        s.RegisterMember("label", &TrackRow::label);
        s.RegisterMember("style", &TrackRow::style);
    }
    model.RegisterArray<std::vector<TrackRow>>();
    if (auto s = model.RegisterStruct<BlockRow>()) {
        s.RegisterMember("index", &BlockRow::index);
        s.RegisterMember("style", &BlockRow::style);
        s.RegisterMember("label", &BlockRow::label);
        s.RegisterMember("title", &BlockRow::title);
        s.RegisterMember("selected", &BlockRow::selected);
        s.RegisterMember("lit", &BlockRow::lit);
        s.RegisterMember("module", &BlockRow::module);
    }
    model.RegisterArray<std::vector<BlockRow>>();
    if (auto s = model.RegisterStruct<LineRow>()) {
        s.RegisterMember("n", &LineRow::n);
        s.RegisterMember("lit", &LineRow::lit);
        s.RegisterMember("error", &LineRow::error);
    }
    model.RegisterArray<std::vector<LineRow>>();
    if (auto s = model.RegisterStruct<ConsoleRow>()) {
        s.RegisterMember("text", &ConsoleRow::text);
        s.RegisterMember("line", &ConsoleRow::line);
        s.RegisterMember("error", &ConsoleRow::error);
        s.RegisterMember("warn", &ConsoleRow::warn);
    }
    model.RegisterArray<std::vector<ConsoleRow>>();
    if (auto s = model.RegisterStruct<SpeedChip>()) {
        s.RegisterMember("label", &SpeedChip::label);
        s.RegisterMember("selected", &SpeedChip::selected);
    }
    model.RegisterArray<std::vector<SpeedChip>>();

    model.Bind("px_ticks", &m_ticks_);
    model.Bind("px_flags", &m_flags_);
    model.Bind("px_marks", &m_marks_);
    model.Bind("px_pauses", &m_pauses_);
    model.Bind("px_tracks", &m_tracks_);
    model.Bind("px_blocks", &m_blocks_);
    model.Bind("px_lines", &m_lines_);
    model.Bind("px_console", &m_console_);
    model.Bind("px_speeds", &m_speeds_);
    model.Bind("px_time", &m_time_);
    model.Bind("px_shown_fps", &m_shown_fps_);
    model.Bind("px_head", &m_head_);
    model.Bind("px_range", &m_range_);
    model.Bind("px_range_from", &m_range_from_);
    model.Bind("px_range_to", &m_range_to_);
    model.Bind("px_more_blocks", &m_more_blocks_);
    model.Bind("px_start", &m_start_);
    model.Bind("px_start_style", &m_start_style_);
    model.Bind("px_start_title", &m_start_title_);
    model.Bind("px_tracks_height", &m_tracks_height_);
    model.Bind("px_code_name", &m_code_name_);
    model.Bind("px_inspector", &m_inspector_);
    model.Bind("px_inspector_call", &m_inspector_call_);
    model.Bind("px_clip_cut", &m_clip_cut_);
    model.Bind("px_code_unsaved", &m_code_unsaved_);
    model.Bind("px_code_conflict", &m_code_conflict_);
    model.Bind("px_has_range", &m_has_range_);
    model.Bind("px_loop_view", &m_loop_view_);
    model.Bind("px_stale", &m_stale_);
    model.Bind("px_has_rec", &m_has_rec_);
    model.Bind("px_code_clip", &m_code_clip_);
    model.Bind("px_has_block", &m_has_block_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    on("px_code_changed", [this](Rml::Event& ev, const Rml::VariantList&) { code_typed(ev.GetParameter<Rml::String>("value", "")); });
    on("px_code_save", [this](Rml::Event&, const Rml::VariantList&) { save_code(); });
    on("px_code_take", [this](Rml::Event&, const Rml::VariantList&) { take_disk_code(); });
    on("px_code_keep", [this](Rml::Event&, const Rml::VariantList&) { keep_my_code(); });
    on("px_console", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(console_.size()) && console_[static_cast<usize>(i)].line > 0) goto_line(console_[static_cast<usize>(i)].line);
    });
    on("px_speed", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(std::size(kSpeeds))) set_speed(kSpeeds[i]);
    });
    on("px_loop_view", [this](Rml::Event&, const Rml::VariantList&) { set_loop_view(!loop_view_); });
    on("px_block", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { select_block(arg_int(a, 0)); });
    on("px_block_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) { block_to_code(arg_int(a, 0)); });
    on("px_flag_code", [this, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (const int i = arg_int(a, 0); i >= 0) flag_to_code(static_cast<usize>(i));
    });
    on("px_start_code", [this](Rml::Event&, const Rml::VariantList&) { start_to_code(); });
    // The middle button on a clip's chip: all of that clip.
    on("px_state_button", [this, arg_str, arg_int](Rml::Event&, const Rml::VariantList& a) {
        if (arg_int(a, 1) == 2) whole_range(arg_str(a, 0));
    });
    on("px_other_seed", [this](Rml::Event&, const Rml::VariantList&) { other_seed(); });
    on("px_snapshot", [this](Rml::Event&, const Rml::VariantList&) { snapshot(); });

    for (const f32 s : kSpeeds) m_speeds_.push_back({"×" + number_words(s), s == speed_});
}

Rml::Element* PixelEditor::element(const char* id) const {
    if (!context_) return nullptr;
    for (int i = 0; i < context_->GetNumDocuments(); ++i)
        if (Rml::Element* e = context_->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

// --- the clip, its frames and its recording ------------------------------------------------------

const pixel::MotionLengths& PixelEditor::lengths_now(const std::string& state) const {
    static const pixel::MotionLengths none;
    if (tl_drag_ == Drag::Flag && state == clip_) return drag_lengths_;
    const auto it = doc_.lengths.find(state);
    return it != doc_.lengths.end() ? it->second : none;
}

void PixelEditor::give_lengths() {
    // Every clip's, the dragged flag's clip as dragged: a start_from records another clip with its own (part 5б).
    std::map<std::string, pixel::MotionLengths> all = doc_.lengths;
    if (tl_drag_ == Drag::Flag) all[clip_] = drag_lengths_;
    script().set_lengths(all);
}

bool PixelEditor::pose_at(const pixel::ClipSpec& c, f64 t, u32 k, pixel::Pose& out, std::string& why) {
    give_lengths();
    if (script().pose(rig(), t, c.state, doc_.params, doc_.seed, out)) return true;
    why = (scene_mode_ ? std::string() : "клип «" + c.state + "», ") + "кадр " + std::to_string(k + 1) + ": " + script().error();
    return false;
}

u32 PixelEditor::clip_frames(const std::string& state) const {
    const auto it = clip_frames_.find(state);
    if (it != clip_frames_.end()) return it->second;
    for (const pixel::ClipSpec& c : doc_.clips)
        if (c.state == state) return c.frames;
    return 0;
}

u32 PixelEditor::total_frames() const {
    u32 total = 0;
    for (const pixel::ClipSpec& c : doc_.clips) total += clip_frames(c.state);
    return total;
}

void PixelEditor::recount() {
    clip_frames_.clear();
    warnings_.clear();
    std::string rec_error;
    u32 rec_line = 0;
    for (const pixel::ClipSpec& c : doc_.clips) {
        pixel::ClipCut cut;
        std::string why;
        clip_frames_[c.state] = script().loaded() && pixel::cut_clip(script(), rig(), doc_, c, cut, why) ? cut.frames : c.frames;
    }
    const pixel::ClipSpec* c = selected_clip();
    const bool code = c && script().loaded() && std::binary_search(script().clips().begin(), script().clips().end(), c->state);
    if (!c) {
        rec_ok_ = false;
        cut_ = whole_ = {};
    } else if (code) {
        give_lengths();
        if (const pixel::Recording* rec = script().recording(rig(), c->state, doc_.params, doc_.seed)) {
            if (rec_clip_ != c->state) block_ = -1;
            rec_ = *rec;
            // A scene's actors' marks are the scene's (part 6а): on its row of marks, sounded by the preview. Their calls are
            // their characters', no line of the panel.
            std::vector<pixel::Mark> theirs;
            if (scene_mode_ && scene_ && scene_->actor_marks(rec_, theirs)) {
                for (pixel::Mark& m : theirs) m.site = {}, rec_.marks.push_back(std::move(m));
                std::stable_sort(rec_.marks.begin(), rec_.marks.end(), [](const pixel::Mark& a, const pixel::Mark& b) { return a.time < b.time; });
            } else if (scene_mode_ && scene_) {
                warnings_.push_back("Метки клипов актёров: " + scene_->error());
            }
            rec_ok_ = true;
            rec_clip_ = c->state;
            rec_stale_ = !script_problem_.empty();
        } else {
            rec_error = script().error();
            rec_line = script().error_line();
            // The last recording that worked stays on the timeline, faint.
            if (rec_clip_ != c->state) rec_ok_ = false;
            rec_stale_ = true;
        }
        if (block_ >= static_cast<i32>(rec_.blocks.size())) block_ = -1;
        cut_ = pixel::cut_clip(*c, rec_ok_, rec_ok_ ? rec_.length : 0);
        whole_ = pixel::whole_clip(*c, rec_ok_, rec_ok_ ? rec_.length : 0);
    } else {
        rec_ok_ = false;
        rec_stale_ = false;
        rec_clip_.clear();
        block_ = -1;
        cut_ = whole_ = pixel::cut_clip(*c, false, 0);
    }
    if (c && code && rec_ok_) {
        const std::string clip = "«" + state_name(c->state) + "»";
        if (cut_.cut)
            warnings_.push_back("Диапазон " + clip + " с " + seconds_words(c->from) + " по " + seconds_words(c->to) + " с длиннее клипа " +
                                seconds_words(cut_.length) + " с: играется и запекается до его конца (файл не меняется, пока диапазон не тронут)");
        const auto it = doc_.lengths.find(c->state);
        if (it != doc_.lengths.end()) {
            auto has = [&](const std::string& name, bool wait) {
                return std::any_of(rec_.events.begin(), rec_.events.end(), [&](const pixel::TimeEvent& e) { return e.name == name && e.wait == wait; });
            };
            for (const auto& [name, s] : it->second.waits)
                if (!has(name, true))
                    warnings_.push_back("Длина «" + name + "» " + seconds_words(s) + " с из файла: в клипе " + clip + " нет wait_until(\"" + name + "\")");
            for (const auto& [name, s] : it->second.durations)
                if (!has(name, false))
                    warnings_.push_back("Длина «" + name + "» " + seconds_words(s) + " с из файла: в клипе " + clip + " нет duration(\"" + name +
                                        "\") или loop_until(\"" + name + "\")");
        }
        // Marks no frame of the range shows do not go into the picture (part 5а); a scene is not baked.
        std::vector<usize> left;
        if (!scene_mode_)
        pixel::strip_events(rec_, cut_, &left);
        std::string names;
        for (usize i : left) names += (names.empty() ? "" : ", ") + rec_.marks[i].name + " " + seconds_words(rec_.marks[i].time) + " с";
        if (!names.empty()) warnings_.push_back("Клип " + clip + ": метки за диапазоном не запекаются: " + names);
    }
    // The playhead stays at its moment, at the clip's end when it got shorter.
    const f64 last = whole_.frames > 0 ? whole_.time(whole_.frames - 1) : 0;
    t_ = std::clamp(t_, 0.0, std::max(last, whole_.code ? whole_.length : last));
    if (playing_) play_clock_ = std::clamp(t_ - cut_.from, 0.0, std::max(0.0, cut_.to - cut_.from));
    rec_error_ = rec_error;
    rec_error_line_ = rec_line;
    watch_modules();
    find_sounds();
    sync_time();
}

u32 PixelEditor::frame_of(f64 t) const {
    if (whole_.frames == 0) return 0;
    if (whole_.code && !whole_.loop && t >= whole_.to - 1e-9) return whole_.frames - 1;
    const f64 k = std::floor((t - whole_.from) * whole_.fps + 1e-6);
    return static_cast<u32>(std::clamp(k, 0.0, static_cast<f64>(whole_.frames - 1)));
}

void PixelEditor::sync_time() {
    const pixel::ClipSpec* c = selected_clip();
    const u32 k = c ? frame_of(t_) : 0;
    if (k != frame_) view_dirty_ = true;
    frame_ = k;
    m_frame_label_ = c ? "кадр " + std::to_string(frame_ + 1) + " из " + std::to_string(whole_.frames) : std::string("клипа нет: поза покоя");
    m_time_ = c ? seconds_words(t_) + " из " + seconds_words(whole_.length) + " с" : std::string();
    dirty("px_frame_label");
    dirty("px_time");
    place_head();
}

f64 PixelEditor::snap(f64 t) const {
    const f64 fps = whole_.fps > 0 ? whole_.fps : 10;
    const f64 end = whole_.code ? whole_.length : (whole_.frames > 0 ? (whole_.frames - 1) / fps : 0);
    f64 s = std::clamp(std::round(t * fps) / fps, 0.0, std::max(0.0, end));
    // The end of a clip of the code is a frame of it too, on a frame or not.
    if (whole_.code && std::fabs(t - end) < std::fabs(t - s)) s = end;
    return s;
}

// --- the player ------------------------------------------------------------------------------------

void PixelEditor::to_start() {
    play(false);
    t_ = cut_.code ? cut_.from : 0;
    sync_time();
}

void PixelEditor::to_end() {
    play(false);
    t_ = cut_.frames > 0 ? cut_.time(cut_.frames - 1) : 0;
    sync_time();
}

void PixelEditor::set_speed(f32 speed) {
    if (std::none_of(std::begin(kSpeeds), std::end(kSpeeds), [&](f32 s) { return s == speed; })) return;
    speed_ = speed;
    for (usize i = 0; i < m_speeds_.size(); ++i) m_speeds_[i].selected = kSpeeds[i] == speed_;
    dirty("px_speeds");
}

void PixelEditor::set_loop_view(bool on) {
    loop_view_ = on;
    m_loop_view_ = on;
    dirty("px_loop_view");
}

// --- the range --------------------------------------------------------------------------------------

bool PixelEditor::set_range(f64 from, f64 to) {
    const pixel::ClipSpec* c = selected_clip();
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return false;
    };
    if (!c) return false;
    if (!cut_.code) return refuse("Диапазон есть у клипа из кода; кадры этого клипа задаёт «Кадров»");
    from = std::clamp(from, 0.0, whole_.length);
    to = std::clamp(to, 0.0, whole_.length);
    if (!(from < to)) return refuse("Начало диапазона — раньше его конца: с " + seconds_words(from) + " по " + seconds_words(to) + " с не выйдет");
    pixel::PixelDoc after = doc_;
    pixel::ClipSpec* a = nullptr;
    for (pixel::ClipSpec& x : after.clips)
        if (x.state == c->state) a = &x;
    const bool whole = from <= 1e-9 && to >= whole_.length - 1e-9;
    a->ranged = !whole;
    a->from = whole ? 0 : pixel::snap_time(from);
    a->to = whole ? 0 : pixel::snap_time(to);
    const std::string clip = "«" + state_name(c->state) + "»";
    return change(after, "«" + name() + "»: " + clip + (whole ? " весь клип" : " с " + seconds_words(from) + " по " + seconds_words(to) + " с"));
}

bool PixelEditor::range_start_here() { return set_range(t_, cut_.code ? cut_.to : 0); }
bool PixelEditor::range_end_here() { return set_range(cut_.code ? cut_.from : 0, t_); }

bool PixelEditor::whole_range(const std::string& state) {
    const auto it = std::find_if(doc_.clips.begin(), doc_.clips.end(), [&](const auto& c) { return c.state == state; });
    if (it == doc_.clips.end() || !it->ranged) return false;
    pixel::PixelDoc after = doc_;
    for (pixel::ClipSpec& x : after.clips)
        if (x.state == state) x.ranged = false, x.from = x.to = 0;
    return change(after, "«" + name() + "»: «" + state_name(state) + "» весь клип");
}

// --- the timeline -------------------------------------------------------------------------------------

void PixelEditor::zoom(f64 factor, f32 at_x) {
    tv_.zoom(factor, at_x);
    lay_timeline();
}

void PixelEditor::pan(f32 dx) {
    tv_.pan(dx);
    lay_timeline();
}

void PixelEditor::center_playhead() {
    tv_.center(t_);
    lay_timeline();
}

bool PixelEditor::timeline_x(f64 t, f32& x) const {
    if (tl_w_ <= 0) return false;
    x = tl_x_ + static_cast<f32>(tv_.x(t));
    return true;
}

bool PixelEditor::wheel(const SDL_Event& e, f32 density, Rml::Context* context) {
    if (e.type != SDL_EVENT_MOUSE_WHEEL || !context || !has_) return false;
    // Over the vector: its zoom a step round the mouse (part 3в), not the column of views scrolled.
    bool vector = false;
    for (const Rml::Element* el = context->GetHoverElement(); el && !vector; el = el->GetParentNode()) vector = el->GetId() == "px-vector";
    if (vector) {
        const f32 notches = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        if (notches != 0) zoom_view(zoom_.step + (notches > 0 ? 1 : -1), e.wheel.mouse_x * density, e.wheel.mouse_y * density);
        return true;
    }
    if (!selected_clip()) return false;
    bool over = false;
    for (const Rml::Element* el = context->GetHoverElement(); el && !over; el = el->GetParentNode()) over = el->GetId() == "px-tl";
    if (!over) return false;
    context_ = context;
    update_timeline_box();
    const bool along = shift_ || (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    const f32 notches = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
    if (along || e.wheel.x != 0) pan(static_cast<f32>(-(along ? notches : -e.wheel.x) * kWheelPan * dp_));
    else zoom(std::pow(kWheelZoom, notches), e.wheel.mouse_x * density - tl_x_);
    return true;
}

void PixelEditor::update_timeline_box() {
    Rml::Element* ruler = element("px-ruler");
    if (!ruler || !ruler->IsVisible(true)) {
        tl_w_ = 0;
        return;
    }
    const Rml::Vector2f at = ruler->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = ruler->GetBox().GetSize(Rml::BoxArea::Content);
    tl_x_ = at.x, tl_y_ = at.y, tl_h_ = size.y;
    if (size.x != tl_w_) {
        tl_w_ = size.x;
        lay_timeline();
    }
}

void PixelEditor::place_head() {
    std::string head = "display: none;";
    if (selected_clip() && tl_w_ > 0) {
        const f64 x = tv_.x(t_);
        if (x >= -1 && x <= tl_w_ + 1) head = at_x(x - 1);
    }
    bool cells = false;
    for (FrameCell& f : m_frames_) {
        const bool shown = static_cast<u32>(f.index) == frame_;
        cells = cells || f.shown != shown;
        f.shown = shown;
    }
    if (cells) dirty("px_frames");
    if (head != m_head_) {
        m_head_ = head;
        dirty("px_head");
    }
}

void PixelEditor::lay_timeline() {
    m_ticks_.clear();
    m_frames_.clear();
    m_flags_.clear();
    m_marks_.clear();
    m_start_.clear();
    m_start_style_.clear();
    m_start_title_.clear();
    m_pauses_.clear();
    m_tracks_.clear();
    m_blocks_.clear();
    m_more_blocks_.clear();
    m_range_ = m_range_from_ = m_range_to_ = "display: none;";
    const pixel::ClipSpec* c = selected_clip();
    const f64 dp = std::max(1e-3f, dp_);
    if (c && tl_w_ > 0) {
        const f64 fps = whole_.fps;
        tv_.width = tl_w_;
        tv_.fps = whole_.fps;
        tv_.dp = dp;
        // A frame's cell goes from its moment to the next one's (the last of a clip of the code without «Повторять» is
        // at its end, on a frame or not).
        auto cell_end = [&](i64 k) {
            return k + 1 < static_cast<i64>(whole_.frames) ? whole_.time(static_cast<u32>(k + 1)) : whole_.time(static_cast<u32>(k)) + 1 / fps;
        };
        tv_.length = std::max(whole_.length, cell_end(static_cast<i64>(whole_.frames) - 1));
        tv_.clamp();
        // A flag dragged makes the clip longer or shorter at each move: the view stays as at the press, so the moment
        // under the mouse is the one it showed there (a short clip fills the timeline and would be fitted again).
        if (tl_drag_ == Drag::Flag) tv_.scale = drag_view_.scale, tv_.start = drag_view_.start;
        const f64 w = tl_w_, per_frame = tv_.per_second() / fps;
        // Frames and the ruler's marks in sight.
        const f64 t0 = tv_.t(0), t1 = tv_.t(w);
        const i64 k0 = std::max<i64>(0, static_cast<i64>(std::floor(t0 * fps)) - 1);
        const i64 k1 = std::min<i64>(static_cast<i64>(whole_.frames) - 1, static_cast<i64>(std::ceil(t1 * fps)) + 1);
        if (per_frame >= kCellLeast * dp)
            for (i64 k = k0; k <= k1; ++k) {
                const f64 x0 = tv_.x(whole_.time(static_cast<u32>(k))), x1 = tv_.x(cell_end(k));
                if (x1 < 0 || x0 > w) continue;
                const f64 left = std::max(0.0, x0), right = std::min(w, x1 - 1);
                m_frames_.push_back({static_cast<int>(k), static_cast<u32>(k) == frame_, place(left, right - left),
                                     right - left >= kCellNumber * dp ? std::to_string(k + 1) : std::string()});
            }
        const u32 step = pixel::ruler_step(per_frame, kTickGap * dp);
        for (i64 k = std::max<i64>(0, k0 / static_cast<i64>(step) * static_cast<i64>(step)); k <= static_cast<i64>(std::ceil(t1 * fps)); k += static_cast<i64>(step)) {
            const f64 x = tv_.x(static_cast<f64>(k) / fps);
            if (x < -1 || x > w + 1) continue;
            m_ticks_.push_back({at_x(x), seconds_words(static_cast<f64>(k) / fps)});
        }
        auto span = [&](f64 a, f64 b) {
            const f64 x0 = std::clamp(tv_.x(a), 0.0, w), x1 = std::clamp(tv_.x(b), 0.0, w);
            return place(x0, x1 - x0);
        };
        if (cut_.code) {
            const f64 from = tl_drag_ == Drag::From || tl_drag_ == Drag::To ? drag_from_ : cut_.from;
            const f64 to = tl_drag_ == Drag::From || tl_drag_ == Drag::To ? drag_to_ : cut_.to;
            m_range_ = span(from, to);
            const f64 reach = 3 * dp;
            if (tv_.x(from) >= -reach && tv_.x(from) <= w + reach) m_range_from_ = place(tv_.x(from) - reach, 2 * reach);
            if (tv_.x(to) >= -reach && tv_.x(to) <= w + reach) m_range_to_ = place(tv_.x(to) - reach, 2 * reach);
        }
        if (rec_ok_) {
            const pixel::Recording& r = rec_;
            if (!r.start.clip.empty()) {
                // start_from (part 5б): a flag at 0, the clip it starts from.
                const pixel::SiteRef s = r.start.site;
                const u32 line = s.file == 0 && s.site >= 0 && s.site < static_cast<i32>(script().code().sites().size())
                                     ? script().code().sites()[static_cast<usize>(s.site)].line
                                     : 0;
                const f64 x0 = tv_.x(0);
                m_start_ = "из " + r.start.clip;
                m_start_style_ = x0 >= -2 && x0 <= w ? at_x(std::max(0.0, x0)) : std::string("display: none;");
                m_start_title_ = "клип начинается с позы конца «" + r.start.clip + "»" +
                                 (line ? ", строка " + std::to_string(line) : std::string()) + ": двойной щелчок — к коду";
            }
            for (usize i = 0; i < r.events.size(); ++i) {
                const pixel::TimeEvent& e = r.events[i];
                const f64 end = e.start + e.length, xe = tv_.x(end);
                if (xe < -2 || tv_.x(e.start) > w + 2) continue;
                const u32 line = e.site.file == 0 && e.site.site >= 0 && e.site.site < static_cast<i32>(script().code().sites().size())
                                     ? script().code().sites()[static_cast<usize>(e.site.site)].line
                                     : 0;
                const std::string call = e.wait ? "wait_until" : "duration";
                m_flags_.push_back({static_cast<int>(i), xe >= 0 && xe <= w ? at_x(xe - 2 * dp) : std::string("display: none;"), span(e.start, end),
                                    e.name + " " + seconds_words(e.length) + " с",
                                    call + "(\"" + e.name + "\") " + seconds_words(e.length) + " с" + (e.from_file ? " (из файла)" : "") +
                                        (line ? ", строка " + std::to_string(line) : std::string()) +
                                        ": тяните по кадрам, с Shift — только он; двойной щелчок — к коду"});
            }
            for (usize i = 0; i < r.marks.size(); ++i) {
                const pixel::Mark& m = r.marks[i];
                const f64 x = tv_.x(m.time);
                if (x + tv_.per_second() * m.length < -4 || x > w + 4) continue;
                const bool long_one = m.length > 0, sound = m.kind == pixel::Mark::Kind::Sound;
                // A sound without its file is red (part 3в).
                const auto file = sound_files_.find(m.name);
                const bool missing = sound && (file == sound_files_.end() || !file->second.file.clip);
                m_marks_.push_back({static_cast<int>(i), long_one ? span(m.time, m.time + m.length) : place(x - 4 * dp, 8 * dp),
                                    (m.actor.empty() ? std::string() : m.actor + ": ") + (sound ? "sound" : "emit") + "(\"" + m.name + "\") в " +
                                        seconds_words(m.time) + " с" +
                                        (long_one ? ", " + seconds_words(m.length) + " с" : std::string()) +
                                        (missing && file != sound_files_.end() ? ": " + file->second.file.why : std::string()),
                                    sound, long_one, missing});
            }
            for (usize i = 0; i < r.pauses.size(); ++i) {
                const pixel::Pause& p = r.pauses[i];
                // Its end where it is dragged to.
                const f64 end = tl_drag_ == Drag::PauseEnd && i == drag_pause_ ? drag_edge_ : p.start + p.length;
                if (tv_.x(end) < 0 || tv_.x(p.start) > w) continue;
                m_pauses_.push_back({static_cast<int>(i), span(p.start, end),
                                     "wait " + seconds_words(end - p.start),
                                     "wait(" + seconds_words(p.length) + ") с " + seconds_words(p.start) + " с: край тянет длину по кадрам"});
            }
            // Tracks in the order of the picture's parts and of the properties; a copy's right after its part's, the
            // copies by number (part 4г). The drawing's track first, the parts of the other drawings after the main
            // one's (part 4д).
            std::vector<usize> order(r.tracks.size());
            for (usize i = 0; i < order.size(); ++i) order[i] = i;
            auto index = [&](const std::string& id) {
                if (const i32 i = rig().part(id); i >= 0) return i;
                const pixel::Rig& h = rig().holder(id);
                const i32 j = &h == &rig() ? -1 : h.part(id);
                return j < 0 ? std::numeric_limits<i32>::max() : static_cast<i32>(rig().parts().size()) + j;
            };
            auto rank = [&](usize i) {
                const pixel::Recording::Track& t = r.tracks[i];
                const std::string id = t.part < r.parts.size() ? r.parts[t.part] : std::string();
                if (t.prop == pixel::Prop::Drawing) return std::tuple{-1, 0, 0};
                i32 p = index(id), copy = 0;
                if (const i32 c = pixel::copy_of(r, id); c >= 0) {
                    p = index(r.copies[static_cast<usize>(c)].source);
                    copy = static_cast<i32>(r.copies[static_cast<usize>(c)].number);
                }
                return std::tuple{p, copy, static_cast<int>(t.prop)};
            };
            std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return rank(a) < rank(b); });
            usize shown = 0, more = 0;
            for (usize row = 0; row < order.size(); ++row) {
                const pixel::Recording::Track& t = r.tracks[order[row]];
                char top[48];
                std::snprintf(top, sizeof top, "top: %.0fpx;", static_cast<f64>(row) * kRowPx);
                m_tracks_.push_back({t.prop == pixel::Prop::Drawing ? std::string(pixel::prop_word(t.prop))
                                                                    : (t.part < r.parts.size() ? r.parts[t.part] : std::string("?")) + " · " + pixel::prop_word(t.prop),
                                     top});
                for (usize j = 0; j < t.blocks.size(); ++j) {
                    const pixel::Block& b = r.blocks[t.blocks[j]];
                    f64 start = b.start;
                    // A binding and a call of the order, the mask or the link hold until the track's next block: the
                    // calls have no length of their own.
                    const bool holds = b.to == pixel::Block::To::Binding || pixel::prop_is_call(b.prop);
                    f64 end = holds ? (j + 1 < t.blocks.size() ? r.blocks[t.blocks[j + 1]].start : r.length) : b.start + b.length;
                    // An edge being dragged: the end there, or all of the block from the start there.
                    if (static_cast<i32>(t.blocks[j]) == drag_block_ && tl_drag_ == Drag::BlockEnd) end = drag_edge_;
                    if (static_cast<i32>(t.blocks[j]) == drag_block_ && tl_drag_ == Drag::BlockStart) end += drag_edge_ - start, start = drag_edge_;
                    f64 x0 = tv_.x(start), x1 = tv_.x(end);
                    if (x1 < 0 || x0 > w) continue;
                    if (shown >= kMostBlocks) {
                        ++more;
                        continue;
                    }
                    ++shown;
                    x0 = std::max(0.0, x0);
                    x1 = std::min(w, std::max(x1, x0 + 2 * dp));
                    const i32 bi = static_cast<i32>(t.blocks[j]);
                    std::string label = pixel::block_words(r, b);
                    if (b.site.file > 0 && b.site.file < r.files.size()) label += " · " + r.files[b.site.file];
                    std::string style = place(x0, x1 - x0) + " " + top;
                    const std::string title =
                        b.prop == pixel::Prop::Order
                            ? label + ": с " + seconds_words(b.start) + " с до следующего вызова порядка; правится в коде, двойной щелчок — к коду"
                        : b.prop == pixel::Prop::Mask
                            ? label + ": с " + seconds_words(b.start) + " с до следующего вызова маски; правится в коде, двойной щелчок — к коду"
                        : b.prop == pixel::Prop::Link
                            ? label + ": с " + seconds_words(b.start) + " с до следующего вызова крепления; правится в коде, двойной щелчок — к коду"
                            : label + ": с " + seconds_words(b.start) + " по " + seconds_words(holds ? end : b.start + b.length) +
                                  " с; края тянут длину и начало по кадрам, двойной щелчок — к коду";
                    m_blocks_.push_back({bi, style, label, title,
                                         bi == block_, std::find(caret_blocks_.begin(), caret_blocks_.end(), bi) != caret_blocks_.end(), b.site.file > 0});
                }
            }
            if (more > 0) m_more_blocks_ = "ещё " + count_words(static_cast<u32>(more), "блок", "блока", "блоков") + ": приблизьте";
        }
    }
    char height[48];
    std::snprintf(height, sizeof height, "height: %.0fpx;", static_cast<f64>(std::max<usize>(m_tracks_.size(), 1)) * kRowPx);
    m_tracks_height_ = height;
    lay_sounds();      // part 3в
    lay_actor_clips(); // part 6а
    tl_laid_w_ = tl_w_;
    place_head();
    for (const char* var : {"px_start", "px_start_style", "px_start_title"}) dirty(var);
    for (const char* var : {"px_ticks", "px_frames", "px_flags", "px_marks", "px_pauses", "px_tracks", "px_blocks", "px_more_blocks", "px_range",
                            "px_range_from", "px_range_to", "px_tracks_height", "px_head"})
        dirty(var);
}

// Part 6а: the actors' clips of a scene, a row an actor in the stage's order. A play is a block as long as its clip / speed
// (its pose held after it), a loop a bar to the actor's next clip or the scene's end, a stop a mark.
void PixelEditor::lay_actor_clips() {
    m_actor_rows_.clear();
    m_plays_.clear();
    const f64 dp = std::max(1e-3f, dp_), w = tl_w_;
    if (scene_mode_ && scene_ && rec_ok_ && tl_w_ > 0 && !rec_.plays.empty()) {
        using Kind = pixel::ActorPlay::Kind;
        usize row = 0;
        for (const pixel::StageItem& it : scene_->stage().items) {
            std::vector<usize> mine;
            for (usize i = 0; i < rec_.plays.size(); ++i)
                if (rec_.plays[i].actor == it.id) mine.push_back(i);
            if (mine.empty()) continue;
            std::stable_sort(mine.begin(), mine.end(), [&](usize a, usize b) {
                const pixel::ActorPlay &x = rec_.plays[a], &y = rec_.plays[b];
                return x.start != y.start ? x.start < y.start : x.order < y.order;
            });
            char top[48];
            std::snprintf(top, sizeof top, "top: %.0fpx;", static_cast<f64>(row) * kRowPx);
            m_actor_rows_.push_back({it.id + " · клипы", top});
            for (usize j = 0; j < mine.size(); ++j) {
                const usize i = mine[j];
                const pixel::ActorPlay& p = rec_.plays[i];
                f64 start = p.start;
                if (tl_drag_ == Drag::PlayStart && static_cast<i32>(i) == drag_block_) start = drag_edge_;
                const f64 next = j + 1 < mine.size() ? rec_.plays[mine[j + 1]].start : rec_.length;
                const f64 end = p.kind == Kind::Loop ? std::max(start, next + (start - p.start)) : start + p.length;
                f64 x0 = tv_.x(start), x1 = tv_.x(end);
                if (p.kind == Kind::Stop) x0 -= 3 * dp, x1 = x0 + 6 * dp;
                if (x1 < 0 || x0 > w) continue;
                x0 = std::max(0.0, x0);
                x1 = std::min(w, std::max(x1, x0 + 2 * dp));
                const std::string label = p.kind == Kind::Stop ? std::string() : p.clip + (p.kind == Kind::Loop ? " ↻" : "");
                const std::string what = p.kind == Kind::Stop ? "stop()" : std::string(p.kind == Kind::Loop ? "loop" : "play") + "(\"" + p.clip + "\")";
                std::string title = it.id + ": " + what + " с " + seconds_words(p.start) + " с";
                if (p.kind == Kind::Play) title += ", " + seconds_words(p.length) + " с";
                if (p.kind == Kind::Loop) title += " по кругу до " + seconds_words(next) + " с";
                if (p.speed != 1) title += ", speed " + number_words(p.speed);
                title += ": левый край тянет начало по кадрам, двойной щелчок — к коду";
                m_plays_.push_back({static_cast<int>(i), place(x0, x1 - x0) + " " + top, label, title, static_cast<i32>(i) == play_, p.kind == Kind::Loop,
                                    p.kind == Kind::Stop});
            }
            ++row;
        }
    }
    char height[48];
    std::snprintf(height, sizeof height, "height: %.0fpx;", static_cast<f64>(std::max<usize>(m_actor_rows_.size(), 1)) * kRowPx);
    m_actor_rows_height_ = height;
    for (const char* var : {"px_actor_rows", "px_plays", "px_actor_rows_height"}) dirty(var);
}

// The mouse on the timeline: the ruler takes the playhead to the frame under it, a range's edge and a flag are dragged by
// frames; the middle button on a flag takes the playhead there.
void PixelEditor::timeline_press(f32 x, f32, Rml::Element* over, bool middle) {
    std::string id;
    bool ruler = false;
    for (Rml::Element* e = over; e; e = e->GetParentNode()) {
        const std::string& eid = e->GetId();
        if (id.empty() && (eid.rfind("px-flag-", 0) == 0 || eid == "px-range-from" || eid == "px-range-to")) id = eid;
        ruler = ruler || eid == "px-ruler";
    }
    if (middle) {
        if (id.rfind("px-flag-", 0) == 0) playhead_to_flag(static_cast<usize>(std::atoi(id.c_str() + 8)));
        return;
    }
    if (id.rfind("px-flag-", 0) == 0 && rec_ok_) {
        const usize i = static_cast<usize>(std::atoi(id.c_str() + 8));
        if (i >= rec_.events.size()) return;
        play(false);
        tl_drag_ = Drag::Flag;
        drag_event_ = i;
        drag_rec_ = rec_;
        drag_view_ = tv_;
        drag_base_lengths_ = lengths_now(clip_);
        drag_lengths_ = drag_base_lengths_;
        drag_only_ = shift_;
        return;
    }
    if ((id == "px-range-from" || id == "px-range-to") && cut_.code) {
        play(false);
        tl_drag_ = id == "px-range-from" ? Drag::From : Drag::To;
        drag_from_ = cut_.from;
        drag_to_ = cut_.to;
        return;
    }
    if (ruler) {
        play(false);
        tl_drag_ = Drag::Ruler;
        timeline_drag(x);
    }
}

void PixelEditor::timeline_drag(f32 x) {
    const pixel::ClipSpec* c = selected_clip();
    if (!c) return;
    const f64 t = snap(tv_.t(x - tl_x_));
    switch (tl_drag_) {
    case Drag::Ruler:
        // The frame whose cell is under the mouse.
        t_ = whole_.time(frame_of(tv_.t(x - tl_x_)));
        sync_time();
        break;
    case Drag::From:
    case Drag::To: {
        const f64 frame = 1 / static_cast<f64>(whole_.fps);
        if (tl_drag_ == Drag::From) drag_from_ = std::clamp(t, 0.0, std::max(0.0, drag_to_ - frame));
        else drag_to_ = std::clamp(t, std::min(whole_.length, drag_from_ + frame), whole_.length);
        lay_timeline();
        break;
    }
    case Drag::Flag: {
        // A new recording at once: the code is the same, the lengths are new.
        drag_lengths_ = pixel::drag_flag(drag_rec_, drag_base_lengths_, drag_event_, t, drag_only_ || shift_);
        give_lengths();
        if (const pixel::Recording* rec = script().recording(rig(), c->state, doc_.params, doc_.seed)) {
            rec_ = *rec;
            cut_ = pixel::cut_clip(*c, true, rec_.length);
            whole_ = pixel::whole_clip(*c, true, rec_.length);
            view_dirty_ = true;
        }
        lay_timeline();
        break;
    }
    case Drag::BlockEnd:
    case Drag::BlockStart:
    case Drag::PauseEnd:
    case Drag::PlayStart: {
        // By frames of the clip; an end not before its start.
        f64 to = edge_snap(tv_.t(x - tl_x_));
        if (tl_drag_ == Drag::BlockEnd && drag_block_ >= 0 && drag_block_ < static_cast<i32>(rec_.blocks.size()))
            to = std::max(to, rec_.blocks[static_cast<usize>(drag_block_)].start);
        if (tl_drag_ == Drag::PauseEnd && drag_pause_ < rec_.pauses.size()) to = std::max(to, rec_.pauses[drag_pause_].start);
        if (to != drag_edge_) {
            drag_edge_ = to;
            lay_timeline();
        }
        break;
    }
    case Drag::None: break;
    }
}

void PixelEditor::timeline_release() {
    const Drag was = tl_drag_;
    tl_drag_ = Drag::None;
    switch (was) {
    case Drag::From:
    case Drag::To:
        if (drag_from_ != cut_.from || drag_to_ != cut_.to) set_range(drag_from_, drag_to_);
        else lay_timeline();
        break;
    case Drag::Flag: {
        const pixel::ClipSpec* c = selected_clip();
        if (!c || drag_event_ >= drag_rec_.events.size()) break;
        pixel::PixelDoc after = doc_;
        after.lengths[c->state] = drag_lengths_;
        if (drag_lengths_ == pixel::MotionLengths{}) after.lengths.erase(c->state);
        const pixel::TimeEvent& e = drag_rec_.events[drag_event_];
        const auto& now = e.wait ? drag_lengths_.waits : drag_lengths_.durations;
        const auto it = now.find(e.name);
        std::string label = "«" + name() + "»: «" + e.name + "» " + seconds_words(it != now.end() ? it->second : e.length) + " с";
        // Another event that gave up as much (Shift).
        for (const pixel::TimeEvent& o : drag_rec_.events) {
            if (o.name == e.name && o.wait == e.wait) continue;
            const auto& m = o.wait ? drag_lengths_.waits : drag_lengths_.durations;
            const auto& b = o.wait ? drag_base_lengths_.waits : drag_base_lengths_.durations;
            const auto at = m.find(o.name), before = b.find(o.name);
            if (at != m.end() && (before == b.end() || before->second != at->second)) label += ", «" + o.name + "» " + seconds_words(at->second) + " с";
        }
        if (!change(after, label)) recount();
        rebuild();
        break;
    }
    case Drag::BlockEnd:
        if (drag_block_ >= 0 && drag_block_ < static_cast<i32>(rec_.blocks.size()) && drag_edge_ != block_end(drag_block_)) set_block_end(drag_block_, drag_edge_);
        else lay_timeline();
        break;
    case Drag::BlockStart:
        if (drag_block_ >= 0 && drag_block_ < static_cast<i32>(rec_.blocks.size()) && drag_edge_ != rec_.blocks[static_cast<usize>(drag_block_)].start)
            set_block_start(drag_block_, drag_edge_);
        else lay_timeline();
        break;
    case Drag::PauseEnd:
        if (drag_pause_ < rec_.pauses.size() && drag_edge_ != rec_.pauses[drag_pause_].start + rec_.pauses[drag_pause_].length)
            set_pause_end(drag_pause_, drag_edge_);
        else lay_timeline();
        break;
    case Drag::PlayStart:
        if (drag_block_ >= 0 && drag_block_ < static_cast<i32>(rec_.plays.size()) && drag_edge_ != rec_.plays[static_cast<usize>(drag_block_)].start)
            set_actor_clip_start(drag_block_, drag_edge_);
        else lay_timeline();
        break;
    case Drag::Ruler:
    case Drag::None: break;
    }
}

bool PixelEditor::set_flag(usize event, f64 to, bool only_this) {
    const pixel::ClipSpec* c = selected_clip();
    if (!c || !rec_ok_ || event >= rec_.events.size()) return false;
    pixel::PixelDoc after = doc_;
    after.lengths[c->state] = pixel::drag_flag(rec_, lengths_now(c->state), event, to, only_this);
    const pixel::TimeEvent& e = rec_.events[event];
    return change(after, "«" + name() + "»: «" + e.name + "» " + seconds_words(std::max(0.0, to - e.start)) + " с");
}

void PixelEditor::playhead_to_flag(usize event) {
    if (!rec_ok_ || event >= rec_.events.size()) return;
    play(false);
    t_ = std::clamp(rec_.events[event].start + rec_.events[event].length, 0.0, whole_.length);
    sync_time();
}

bool PixelEditor::flag_to_code(usize event) {
    if (!rec_ok_ || event >= rec_.events.size()) return false;
    const pixel::SiteRef s = rec_.events[event].site;
    if (s.file != 0 || s.site < 0 || s.site >= static_cast<i32>(script().code().sites().size())) {
        note_ = "Событие «" + rec_.events[event].name + "» из модуля: его код не в панели";
        rebuild();
        return false;
    }
    return goto_line(script().code().sites()[static_cast<usize>(s.site)].line);
}

bool PixelEditor::start_to_code() {
    if (!rec_ok_ || rec_.start.clip.empty()) return false;
    const pixel::SiteRef s = rec_.start.site;
    if (s.file != 0 || s.site < 0 || s.site >= static_cast<i32>(script().code().sites().size())) {
        note_ = "start_from клипа — в модуле: его код не в панели";
        rebuild();
        return false;
    }
    return goto_line(script().code().sites()[static_cast<usize>(s.site)].line);
}

bool PixelEditor::select_block(i32 index) {
    if (!rec_ok_ || index < -1 || index >= static_cast<i32>(rec_.blocks.size())) return false;
    block_ = index;
    view_dirty_ = true; // a copy's block outlines the copy
    lit_first_ = lit_last_ = 0;
    if (index >= 0 && play_ >= 0) { // one thing of the timeline in the inspector (part 6а)
        play_ = -1;
        rebuild_actor_clip();
        lay_actor_clips();
    }
    m_inspector_.clear();
    m_inspector_call_.clear();
    if (index >= 0) {
        const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
        const std::string part = b.part < rec_.parts.size() ? rec_.parts[b.part] : std::string("?");
        // The drawing's track is the character's, no part's (part 4д).
        std::string text = (b.prop == pixel::Prop::Drawing ? std::string() : part + " · ") + pixel::prop_word(b.prop) + "\n";
        if (b.to == pixel::Block::To::Binding) text += "с " + seconds_words(b.start) + " с, пока на дорожке не начнётся другой блок\n";
        else text += "с " + seconds_words(b.start) + " по " + seconds_words(b.start + b.length) + " с (" + seconds_words(b.length) + " с)\n";
        if (b.to != pixel::Block::To::Binding)
            text += "откуда " + pixel::value_words(b.prop, b.from) + ", куда " + pixel::value_words(b.prop, b.end) + "\n";
        text += "как: " + timing_words(rec_, b);
        u32 repeats = 0;
        for (const pixel::Block& o : rec_.blocks) repeats += o.site == b.site && o.part == b.part && o.prop == b.prop;
        // A copy's call is one for all the copies of its copies(…) (part 4г): an edit changes each.
        if (const i32 c = pixel::copy_of(rec_, part); c >= 0) {
            std::set<u32> copies;
            for (const pixel::Block& o : rec_.blocks)
                if (const i32 oc = o.site == b.site && o.prop == b.prop && o.part < rec_.parts.size() ? pixel::copy_of(rec_, rec_.parts[o.part]) : -1;
                    oc >= 0 && rec_.copies[static_cast<usize>(oc)].source == rec_.copies[static_cast<usize>(c)].source)
                    copies.insert(o.part);
            text += "\nиз copies: " + count_words(static_cast<u32>(copies.size()), "копия", "копии", "копий");
        }
        if (repeats > 1) text += "\nиз цикла: " + count_words(repeats, "повтор", "повтора", "повторов");
        if (b.site.file > 0 && b.site.file < rec_.files.size()) {
            text += "\nиз модуля «" + rec_.files[b.site.file] + "» (animations/" + rec_.files[b.site.file] + ".luau)";
            if (const pixel::CodeSites* m = script().module_code(rec_.files[b.site.file]);
                m && b.site.site >= 0 && b.site.site < static_cast<i32>(m->sites().size()))
                m_inspector_call_ = rec_.files[b.site.file] + ".luau, строка " + std::to_string(m->sites()[static_cast<usize>(b.site.site)].line);
        } else if (b.site.site >= 0 && b.site.site < static_cast<i32>(script().code().sites().size())) {
            const pixel::CallSite& site = script().code().sites()[static_cast<usize>(b.site.site)];
            pixel::span_lines(script_text_, site.span, lit_first_, lit_last_);
            const usize from = std::min<usize>(site.span.begin, script_text_.size()), to = std::min<usize>(site.span.end, script_text_.size());
            m_inspector_call_ = "строка " + std::to_string(site.line) + ": " + call_words(std::string_view(script_text_).substr(from, to - from));
        }
        m_inspector_ = text;
    }
    m_has_block_ = index >= 0;
    for (BlockRow& r : m_blocks_) r.selected = r.index == block_;
    rebuild_gutter();
    rebuild_inspector();
    for (const char* var : {"px_blocks", "px_inspector", "px_inspector_call", "px_has_block"}) dirty(var);
    return true;
}

bool PixelEditor::block_to_code(i32 index) {
    if (!select_block(index) || index < 0) return false;
    const pixel::Block& b = rec_.blocks[static_cast<usize>(index)];
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return false;
    };
    if (b.site.file > 0) return refuse("Блок из модуля «" + rec_.files[b.site.file] + "»: его код — animations/" + rec_.files[b.site.file] + ".luau, не в панели");
    if (b.site.site < 0 || b.site.site >= static_cast<i32>(script().code().sites().size())) return false;
    if (code_ != script_panel_) return refuse("Код в панели не тот, что записан: сохраните его (Ctrl+S), и блок покажет свой вызов");
    select_call(b);
    return true;
}

void PixelEditor::select_call(const pixel::Block& b) {
    if (b.site.file != 0 || b.site.site < 0 || b.site.site >= static_cast<i32>(script().code().sites().size())) return;
    const pixel::CodeSpan span = script().code().sites()[static_cast<usize>(b.site.site)].span;
    auto* text = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("px-code-text"));
    if (!text) return;
    text->Focus();
    text->SetSelectionRange(static_cast<int>(pixel::panel_char(script_text_, span.begin)), static_cast<int>(pixel::panel_char(script_text_, span.end)));
    caret_ = ~0u;
}

// --- the code panel ----------------------------------------------------------------------------------

void PixelEditor::code_typed(const std::string& text) {
    if (!has_ || text == code_) return;
    const std::string old = code_;
    usize p = 0;
    while (p < old.size() && p < text.size() && old[p] == text[p]) ++p;
    while (p > 0 && ((p < old.size() && (static_cast<u8>(old[p]) & 0xC0) == 0x80) || (p < text.size() && (static_cast<u8>(text[p]) & 0xC0) == 0x80))) --p;
    usize s = 0;
    while (s < old.size() - p && s < text.size() - p && old[old.size() - 1 - s] == text[text.size() - 1 - s]) ++s;
    while (s > 0 && ((static_cast<u8>(old[old.size() - s]) & 0xC0) == 0x80)) --s;
    const u32 removed = static_cast<u32>(old.size() - p - s), inserted = static_cast<u32>(text.size() - p - s);
    const bool letter = removed == 0 && inserted > 0 && inserted == utf8_length(static_cast<u8>(text[p])) && text[p] != '\n';
    typing_.resize(typing_at_);
    if (letter && typing_open_ && !typing_.empty() && p == typing_end_ && typing_.back().after == old) {
        typing_.back().after = text;
        typing_.back().inserted += inserted;
    } else {
        typing_.push_back({old, text, static_cast<u32>(p), removed, inserted});
        typing_at_ = typing_.size();
    }
    typing_open_ = letter;
    typing_end_ = static_cast<u32>(p + inserted);
    code_ = text;
    caret_ = ~0u;
    m_code_unsaved_ = code_unsaved();
    dirty("px_code_unsaved");
    rebuild_gutter();
}

void PixelEditor::set_panel(const std::string& text, i32 caret) {
    auto* e = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("px-code-text"));
    if (!e) return;
    if (e->GetValue() != text) e->SetValue(text);
    if (caret >= 0) {
        e->Focus();
        e->SetSelectionRange(caret, caret);
    }
}

bool PixelEditor::code_undo() {
    if (typing_at_ == 0) return false;
    const Typing& t = typing_[--typing_at_];
    code_ = t.before;
    typing_open_ = false;
    set_panel(code_, static_cast<i32>(pixel::char_index(code_, t.at + t.removed)));
    m_code_unsaved_ = code_unsaved();
    dirty("px_code_unsaved");
    rebuild_gutter();
    return true;
}

bool PixelEditor::code_redo() {
    if (typing_at_ >= typing_.size()) return false;
    const Typing& t = typing_[typing_at_++];
    code_ = t.after;
    typing_open_ = false;
    set_panel(code_, static_cast<i32>(pixel::char_index(code_, t.at + t.inserted)));
    m_code_unsaved_ = code_unsaved();
    dirty("px_code_unsaved");
    rebuild_gutter();
    return true;
}

bool PixelEditor::save_code() {
    if (!has_ || doc_.script.empty()) return false;
    if (!code_unsaved() && !code_conflict_) {
        note_ = "«" + doc_.script + "» уже такой, как в панели";
        rebuild();
        return false;
    }
    return write_script(pixel::file_text(code_, code_crlf_), "«" + name() + "»: код «" + doc_.script + "» сохранён");
}

bool PixelEditor::write_script(const std::string& text, std::string label) {
    const fs::path file = script_file();
    std::vector<PixelFiles::File> files;
    files.push_back({file, bytes_of(file), text_bytes(text)});
    // The clips of the code counted anew: the «.pixel.json» says how many frames they have now.
    pixel::PoseScript probe;
    probe.set_module_reader(pixel::animations_reader(game_dir));
    pixel::PixelDoc after = doc_;
    std::string why;
    if (!scene_mode_ && probe.load(text) && pixel::count_frames(probe, rig(), after, why) && after != doc_)
        files.push_back({file_, bytes_of(file_), text_bytes(pixel::write_pixel_doc(after))});
    note_.clear();
    typing_.clear();
    typing_at_ = 0;
    typing_open_ = false;
    code_conflict_ = false;
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), std::move(label)));
    history_.seal();
    rebuild();
    return true;
}

void PixelEditor::adopt_code(const std::string& file_text) {
    bool crlf = false;
    const std::string disk = pixel::panel_text(file_text, crlf);
    code_crlf_ = crlf;
    if (disk == code_disk_) return;
    if (code_ == code_disk_ || code_ == disk) {
        // Nothing unsaved: the panel takes the file as it is now.
        code_ = disk;
        code_conflict_ = false;
        typing_.clear();
        typing_at_ = 0;
        typing_open_ = false;
    } else {
        code_conflict_ = true;
    }
    code_disk_ = disk;
    caret_ = ~0u;
}

void PixelEditor::take_disk_code() {
    code_ = code_disk_;
    code_conflict_ = false;
    typing_.clear();
    typing_at_ = 0;
    typing_open_ = false;
    set_panel(code_, -1);
    rebuild();
}

void PixelEditor::keep_my_code() {
    code_conflict_ = false;
    rebuild();
}

std::vector<std::string> PixelEditor::unsaved() const {
    if (!code_unsaved()) return {};
    return {"Векторная: код «" + path_to_utf8(utf8_path(rel_).parent_path() / utf8_path(doc_.script)) + "» не сохранён (при закрытии набор теряется)"};
}

bool PixelEditor::save_all(std::string& error) {
    if (!code_unsaved()) return true;
    save_code();
    if (!code_unsaved()) return true;
    error = "код «" + doc_.script + "» не записан";
    return false;
}

bool PixelEditor::goto_line(u32 line) {
    auto* text = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("px-code-text"));
    if (!text || line == 0) return false;
    const int at = static_cast<int>(pixel::line_char(code_, line));
    text->Focus();
    text->SetSelectionRange(at, at);
    caret_ = ~0u;
    return true;
}

bool PixelEditor::code_selection(u32& from, u32& to) const {
    auto* text = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("px-code-text"));
    if (!text) return false;
    int a = 0, b = 0;
    text->GetSelection(&a, &b, nullptr);
    from = static_cast<u32>(std::max(0, a));
    to = static_cast<u32>(std::max(0, b));
    return true;
}

bool PixelEditor::panel_key(const SDL_KeyboardEvent& k, Rml::Context* context) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0, shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    if (ctrl && k.key == SDLK_S) {
        save_code();
        return true;
    }
    if (ctrl && k.key == SDLK_Z) {
        shift ? code_redo() : code_undo();
        return true;
    }
    if (ctrl && k.key == SDLK_Y) {
        code_redo();
        return true;
    }
    // A tab is two spaces: a text field leaves the tab character out.
    if (!ctrl && !(k.mod & SDL_KMOD_ALT) && k.key == SDLK_TAB && context) {
        context->ProcessTextInput(Rml::String("  "));
        return true;
    }
    return false;
}

// The caret in the panel: the blocks of the call it is in are lit.
void PixelEditor::update_caret() {
    auto* text = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("px-code-text"));
    if (!text) return;
    // The gutter goes with the text.
    if (Rml::Element* lines = element("px-code-lines")) {
        char top[48];
        std::snprintf(top, sizeof top, "%.1fdevpx", -text->GetScrollTop());
        lines->SetProperty("top", top);
    }
    if (context_->GetFocusElement() != text) return;
    int a = 0, b = 0;
    text->GetSelection(&a, &b, nullptr);
    const u32 caret = static_cast<u32>(std::max(0, b));
    if (caret == caret_) return;
    caret_ = caret;
    std::vector<i32> lit;
    if (rec_ok_ && code_ == script_panel_) {
        const i32 site = pixel::site_at(script().code(), pixel::file_byte(script_text_, caret));
        if (site >= 0)
            for (usize i = 0; i < rec_.blocks.size(); ++i)
                if (rec_.blocks[i].site == pixel::SiteRef{0, site}) lit.push_back(static_cast<i32>(i));
    }
    if (lit == caret_blocks_) return;
    caret_blocks_ = std::move(lit);
    for (BlockRow& r : m_blocks_) r.lit = std::find(caret_blocks_.begin(), caret_blocks_.end(), r.index) != caret_blocks_.end();
    dirty("px_blocks");
}

void PixelEditor::rebuild_gutter() {
    const int lines = 1 + static_cast<int>(std::count(code_.begin(), code_.end(), '\n'));
    std::vector<LineRow> rows;
    rows.reserve(static_cast<usize>(lines));
    const u32 error = !script_problem_.empty() || !rec_error_.empty() || !pose_problem_.empty() ? error_line_ : 0;
    for (int n = 1; n <= lines; ++n)
        rows.push_back({n, lit_first_ > 0 && static_cast<u32>(n) >= lit_first_ && static_cast<u32>(n) <= lit_last_, static_cast<u32>(n) == error});
    if (rows.size() == m_lines_.size() &&
        std::equal(rows.begin(), rows.end(), m_lines_.begin(), [](const LineRow& a, const LineRow& b) { return a.lit == b.lit && a.error == b.error; }))
        return;
    m_lines_ = std::move(rows);
    dirty("px_lines");
}

void PixelEditor::rebuild_console() {
    std::vector<ConsoleLine> lines;
    if (!script_problem_.empty()) lines.push_back({script_problem_, error_line_, true, false});
    if (!draw_problem_.empty()) lines.push_back({draw_problem_, draw_line_, true, false});
    if (!picture_note_.empty()) lines.push_back({picture_note_, 0, false, true});
    if (!rec_error_.empty()) lines.push_back({"Запись клипа «" + state_name(clip_) + "»: " + rec_error_, rec_error_line_, true, false});
    if (!pose_problem_.empty()) lines.push_back({pose_problem_, script().error_line(), true, false});
    for (const std::string& w : warnings_) lines.push_back({w, 0, false, true});
    // A sound without its file, once a name, at its first call (part 3в).
    if (rec_ok_) {
        std::vector<std::string> said;
        for (const pixel::Mark& m : rec_.marks) {
            if (m.kind != pixel::Mark::Kind::Sound || std::find(said.begin(), said.end(), m.name) != said.end()) continue;
            said.push_back(m.name);
            const auto f = sound_files_.find(m.name);
            if (f == sound_files_.end() || f->second.file.clip) continue;
            u32 line = 0;
            std::string where;
            if (m.site.file == 0 && m.site.site >= 0 && m.site.site < static_cast<i32>(script().code().sites().size()))
                line = script().code().sites()[static_cast<usize>(m.site.site)].line;
            else if (m.site.file > 0 && m.site.file < rec_.files.size())
                where = rec_.files[m.site.file] + ".luau: ";
            lines.push_back({where + (line ? "строка " + std::to_string(line) + ": " : std::string()) + f->second.file.why, line, false, true});
        }
    }
    // What the clip asks and the frame does not do, once a part (part 4а): a skew past 89°, a dash without a pattern.
    if (rec_ok_)
        for (const pixel::Note& n : rec_.notes) {
            const bool here = n.site.file == 0;
            const std::string where = !here && n.site.file < rec_.files.size() ? rec_.files[n.site.file] + ".luau: " : std::string();
            lines.push_back({where + (n.line ? "строка " + std::to_string(n.line) + ": " : std::string()) + n.text, here ? n.line : 0, false, true});
        }
    if (rec_ok_)
        for (const pixel::LogLine& l : rec_.log) {
            std::string where;
            if (l.site.file > 0 && l.site.file < rec_.files.size()) where = " (" + rec_.files[l.site.file] + ".luau, строка " + std::to_string(l.line) + ")";
            lines.push_back({seconds_words(l.time) + " с: " + l.text + where, l.site.file == 0 ? l.line : 0, false, false});
        }
    console_ = std::move(lines);
    m_console_.clear();
    for (const ConsoleLine& l : console_) m_console_.push_back({l.text, static_cast<int>(l.line), l.error, l.warn});
    dirty("px_console");
}

// --- the modules the recording required ---------------------------------------------------------------

void PixelEditor::watch_modules() {
    std::vector<Watched> now;
    if (rec_ok_ && !game_dir.empty())
        for (usize i = 1; i < rec_.files.size(); ++i) {
            const fs::path f = game_dir / "animations" / utf8_path(rec_.files[i] + ".luau");
            const auto was = std::find_if(modules_.begin(), modules_.end(), [&](const Watched& w) { return w.file == f; });
            now.push_back(was != modules_.end() ? *was : Watched{f, stamp_of(f)});
        }
    modules_ = std::move(now);
}

bool PixelEditor::modules_changed() {
    bool changed = false;
    for (Watched& w : modules_) {
        const i64 s = stamp_of(w.file);
        changed = changed || s != w.stamp;
        w.stamp = s;
    }
    return changed;
}

// --- «Другое зерно», «Снимок кадра» -------------------------------------------------------------------

bool PixelEditor::other_seed() {
    if (!has_) return false;
    pixel::PixelDoc after = doc_;
    after.seed = pixel::other_seed(doc_.seed);
    return change(after, "«" + name() + "»: другое зерно " + std::to_string(after.seed));
}

std::string PixelEditor::snapshot() {
    note_.clear();
    if (scene_mode_) return scene_snapshot(); // part 6а: the frame at the scene's size, smooth
    auto refuse = [&](std::string why) {
        note_ = std::move(why);
        rebuild();
        return std::string();
    };
    const pixel::ClipSpec* c = selected_clip();
    if (!has_ || !rig().loaded() || !c) return refuse("Снимка нет: нет клипа на таймлайне");
    if (assets_.looking()) return refuse("«Ресурсы» ещё смотрят файлы: снимите через миг");
    pixel::Pose pose;
    std::string why;
    if (!pose_at(*c, t_, frame_, pose, why)) return refuse("Снимка нет: " + why);
    const f64 scale = rig().to_pixels(doc_.frame_w, doc_.frame_h).a;
    if (!rig().pose(pose, why, scale > 0 ? 1 / scale : 0)) return refuse("Снимка нет: " + why);
    const std::vector<pixel::Rgba8> colors = doc_.game_palette && game_colors ? game_colors() : std::vector<pixel::Rgba8>{};
    const assets::CookedTexture frame = rig().bake(doc_.frame_w, doc_.frame_h, pixel::bake_options(doc_, colors));
    view_dirty_ = true; // the rig is in the frame's pose: the views pose it again
    std::vector<u8> png;
    if (!assets::encode_image(frame, ".png", png)) return refuse("Кадр не кодируется в PNG");
    const fs::path dir = file_.parent_path();
    const std::string stem = name() + " " + c->state + " " + std::to_string(frame_ + 1);
    fs::path target = dir / utf8_path(stem + ".png");
    std::error_code ec;
    for (int n = 2; fs::exists(target, ec); ++n) target = dir / utf8_path(stem + " (" + std::to_string(n) + ").png");
    assets::AssetMeta meta;
    meta.id = Guid::generate();
    fs::path meta_file = target;
    meta_file += ".meta";
    std::vector<PixelFiles::File> files;
    files.push_back({target, std::nullopt, png});
    files.push_back({meta_file, std::nullopt, text_bytes(data::to_json(meta))});
    const std::string file_name = path_to_utf8(target.filename());
    history_.execute(std::make_unique<PixelFiles>(*this, assets_, std::move(files), "«" + name() + "»: снимок «" + file_name + "»"));
    history_.seal();
    note_ = "Снимок кадра: «" + file_name + "» рядом с «" + path_to_utf8(file_.filename()) + "»";
    rebuild();
    std::string rel = path_to_utf8(target.lexically_relative(assets_.root()));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

} // namespace forge::editor_app
