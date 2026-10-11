#include "animation_editor.h"

#include "forge/assets/image.h"
#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <optional>

namespace forge::editor_app {

namespace {

constexpr u32 kThumbPx = 72;    // a frame in the strip
constexpr u32 kPreviewPx = 240; // the frame playing

// Puts a template's frames and animations back as they were, onto the template as it is now: the tab «Объекты»
// changes its other fields in a history of its own.
class AnimCommand final : public editor::Command {
public:
    struct Look {
        u32 frames = 1;
        std::map<std::string, objects::Clip> animations;
    };
    AnimCommand(objects::Library& lib, u64 key, Look before, Look after, std::string label)
        : lib_(lib), key_(key), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
    void apply(editor::Document&) override { set(after_); }
    void revert(editor::Document&) override { set(before_); }
    std::string label() const override { return label_; }

private:
    void set(const Look& to) {
        const objects::Template* now = lib_.find(key_);
        if (!now) return;
        objects::Template t = *now;
        t.frames = to.frames;
        t.animations = to.animations;
        std::string error;
        if (!lib_.put(std::move(t), &error)) FORGE_ERROR("Шаблон «%s»: %s", now->name.c_str(), error.c_str());
    }
    objects::Library& lib_;
    u64 key_;
    Look before_, after_;
    std::string label_;
};

// 7.5 → «7,5».
std::string fps_words(f32 fps) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<f64>(fps));
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// Frames as the author counts them, from 1: «2 · 1 · 1».
std::string frames_words(const objects::Clip& clip) {
    std::string s;
    for (usize i = 0; i < clip.frames.size(); ++i) s += (i ? " · " : "") + std::to_string(clip.frames[i] + 1);
    return s;
}

// A name as the alphabet orders it: Ё with Е (its UTF-8 bytes come before А's).
std::string alphabet_key(std::string s) {
    for (usize at = 0; (at = s.find("Ё", at)) != std::string::npos;) s.replace(at, 2, "Е");
    for (usize at = 0; (at = s.find("ё", at)) != std::string::npos;) s.replace(at, 2, "е");
    return s;
}

std::string count_words(u32 n, const char* one, const char* few, const char* many) {
    const u32 d = n % 10, h = n % 100;
    return std::to_string(n) + " " + (d == 1 && h != 11 ? one : d >= 2 && d <= 4 && (h < 12 || h > 14) ? few : many);
}

// Columns x0..x0+w of the picture into a box of `box` pixels: bigger by a whole number (pixel art stays crisp), or
// smaller to fit.
std::vector<u8> fit_frame(const assets::CookedTexture& image, u32 x0, u32 w, u32 box, u32& out_w, u32& out_h) {
    const u32 h = image.height;
    f32 scale = std::min(static_cast<f32>(box) / static_cast<f32>(w), static_cast<f32>(box) / static_cast<f32>(h));
    if (scale >= 1) scale = std::floor(scale);
    out_w = std::max(1u, static_cast<u32>(static_cast<f32>(w) * scale));
    out_h = std::max(1u, static_cast<u32>(static_cast<f32>(h) * scale));
    std::vector<u8> rgba(static_cast<usize>(out_w) * out_h * 4);
    for (u32 y = 0; y < out_h; ++y)
        for (u32 x = 0; x < out_w; ++x) {
            const u32 sx = x0 + std::min(w - 1, static_cast<u32>(static_cast<f32>(x) / scale));
            const u32 sy = std::min(h - 1, static_cast<u32>(static_cast<f32>(y) / scale));
            const u8* p = &image.rgba8[(static_cast<usize>(sy) * image.width + sx) * 4];
            std::copy(p, p + 4, &rgba[(static_cast<usize>(y) * out_w + x) * 4]);
        }
    return rgba;
}

} // namespace

AnimationEditor::AnimationEditor(level::LevelModule& module) : module_(module) {}

bool AnimationEditor::init(ui::Ui& ui) {
    ui_ = &ui;
    for (f32 fps : objects::kClipFps) m_fps_.push_back({fps_words(fps), false});
    return true;
}

void AnimationEditor::bind(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<Row>()) {
        s.RegisterMember("name", &Row::name);
        s.RegisterMember("icon", &Row::icon);
        s.RegisterMember("frames", &Row::frames);
        s.RegisterMember("note", &Row::note);
        s.RegisterMember("selected", &Row::selected);
    }
    model.RegisterArray<std::vector<Row>>();
    if (auto s = model.RegisterStruct<StateView>()) {
        s.RegisterMember("id", &StateView::id);
        s.RegisterMember("name", &StateView::name);
        s.RegisterMember("frames", &StateView::frames);
        s.RegisterMember("fps", &StateView::fps);
        s.RegisterMember("problem", &StateView::problem);
        s.RegisterMember("own", &StateView::own);
        s.RegisterMember("loop", &StateView::loop);
        s.RegisterMember("selected", &StateView::selected);
    }
    model.RegisterArray<std::vector<StateView>>();
    if (auto s = model.RegisterStruct<Thumb>()) {
        s.RegisterMember("icon", &Thumb::icon);
        s.RegisterMember("index", &Thumb::index);
        s.RegisterMember("used", &Thumb::used);
        s.RegisterMember("shown", &Thumb::shown);
    }
    model.RegisterArray<std::vector<Thumb>>();
    if (auto s = model.RegisterStruct<FpsChip>()) {
        s.RegisterMember("label", &FpsChip::label);
        s.RegisterMember("selected", &FpsChip::selected);
    }
    model.RegisterArray<std::vector<FpsChip>>();

    model.Bind("an_list", &m_list_);
    model.Bind("an_states", &m_states_);
    model.Bind("an_strip", &m_strip_);
    model.Bind("an_seq", &m_seq_);
    model.Bind("an_fps", &m_fps_);
    model.Bind("an_note", &m_note_);
    model.Bind("an_has_sel", &m_has_sel_);
    model.Bind("an_sel_name", &m_sel_name_);
    model.Bind("an_sel_note", &m_sel_note_);
    model.Bind("an_preview", &m_preview_);
    model.Bind("an_problem", &m_problem_);
    model.Bind("an_state_name", &m_state_name_);
    model.Bind("an_frames", &m_frames_);
    model.Bind("an_own", &m_own_);
    model.Bind("an_loop", &m_loop_);
    model.Bind("an_playing", &m_playing_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) { fn(args); });
    };
    auto arg_int = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<int>(-1) : -1; };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    on("an_select", [this, arg_int](const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(listed_.size())) select(listed_[static_cast<usize>(i)]);
    });
    on("an_state", [this, arg_str](const Rml::VariantList& a) { select_state(arg_str(a, 0)); });
    on("an_add", [this, arg_int](const Rml::VariantList& a) {
        if (const int i = arg_int(a, 0); i >= 0) add_frame(static_cast<u32>(i));
    });
    on("an_remove_last", [this](const Rml::VariantList&) { remove_last(); });
    on("an_set_fps", [this, arg_int](const Rml::VariantList& a) {
        const int i = arg_int(a, 0);
        if (i >= 0 && i < static_cast<int>(std::size(objects::kClipFps))) set_fps(objects::kClipFps[i]);
    });
    on("an_loop_toggle", [this](const Rml::VariantList&) { set_loop(!clip().loop); });
    on("an_reset", [this](const Rml::VariantList&) { reset_state(); });
    on("an_frames_less", [this](const Rml::VariantList&) {
        if (const objects::Template* t = selected()) set_frames(static_cast<int>(t->frames) - 1);
    });
    on("an_frames_more", [this](const Rml::VariantList&) {
        if (const objects::Template* t = selected()) set_frames(static_cast<int>(t->frames) + 1);
    });
    on("an_play", [this](const Rml::VariantList&) { play(!playing_); });
    on("an_step", [this, arg_int](const Rml::VariantList& a) { step(arg_int(a, 0) < 0 ? -1 : 1); });
}

void AnimationEditor::update(f64 dt) {
    if (library().version() != built_) rebuild();
    if (playing_) {
        time_ += std::min(dt, 0.1);
        ticks_ = static_cast<u64>(time_ * 60.0);
    }
    show_frame();
}

bool AnimationEditor::handle_key(const SDL_KeyboardEvent& k) {
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) return false;
    switch (k.key) {
    case SDLK_SPACE: play(!playing_); return true;
    case SDLK_LEFT: step(-1); return true;
    case SDLK_RIGHT: step(1); return true;
    case SDLK_BACKSPACE: remove_last(); return true;
    case SDLK_DELETE: reset_state(); return true;
    case SDLK_UP:
    case SDLK_DOWN: {
        if (listed_.empty()) return true;
        const auto at = std::find(listed_.begin(), listed_.end(), selected_);
        const i64 i = at == listed_.end() ? 0 : (at - listed_.begin()) + (k.key == SDLK_UP ? -1 : 1);
        select(listed_[static_cast<usize>(std::clamp<i64>(i, 0, static_cast<i64>(listed_.size()) - 1))]);
        return true;
    }
    default: break;
    }
    // 1, 2, 3…: the states in their order.
    if (k.key >= SDLK_1 && k.key <= SDLK_9) {
        const usize i = static_cast<usize>(k.key - SDLK_1);
        if (i < states_.size()) select_state(states_[i].id);
        return true;
    }
    return false;
}

void AnimationEditor::undo() {
    if (!history_.undo()) return;
    FORGE_INFO("Отменено");
    rebuild();
}

void AnimationEditor::redo() {
    if (!history_.redo()) return;
    FORGE_INFO("Повторено");
    rebuild();
}

std::string AnimationEditor::status() const {
    const objects::Template* t = selected();
    if (!t) return "Анимация: шаблонов со своей картинкой нет";
    std::string s = "Анимация: «" + t->name + "»";
    for (const auto& st : states_)
        if (st.id == state_) {
            const objects::Clip c = clip();
            s += " · " + st.name + (own() ? "" : " (как в игре)") + " · " + count_words(static_cast<u32>(c.frames.size()), "кадр", "кадра", "кадров") +
                 " · " + fps_words(c.fps) + " к/с" + (c.loop ? "" : " · без повтора");
        }
    return s + (playing_ ? " · играет" : " · пауза");
}

// --- the selected template and state ------------------------------------------

const objects::Template* AnimationEditor::selected() const { return selected_ ? module_.library()->find(selected_) : nullptr; }

bool AnimationEditor::select_picture(const std::string& picture, const std::string& asset_name) {
    if (library().version() != built_) rebuild();
    for (u64 key : listed_)
        if (const objects::Template* t = library().find(key); t && !picture.empty() && t->picture == picture) {
            from_note_.clear();
            built_ = 0;
            select(key);
            if (built_ == 0) rebuild();
            return true;
        }
    from_note_ = "Картинку «" + asset_name + "» пока не рисует ни один шаблон игры. Дайте её шаблону во вкладке «Объекты», и он появится здесь.";
    rebuild();
    return false;
}

void AnimationEditor::select(u64 key) {
    from_note_.clear();
    if (key == selected_) return;
    selected_ = key;
    state_.clear();
    time_ = 0;
    ticks_ = 0;
    built_ = 0; // the views again
    rebuild();
}

bool AnimationEditor::select_state(const std::string& id) {
    if (id == state_ || std::none_of(states_.begin(), states_.end(), [&](const auto& s) { return s.id == id; })) return false;
    state_ = id;
    time_ = 0;
    ticks_ = 0;
    built_ = 0;
    rebuild();
    return true;
}

objects::Clip AnimationEditor::clip() const {
    const objects::Template* t = selected();
    const auto st = std::find_if(states_.begin(), states_.end(), [&](const auto& s) { return s.id == state_; });
    if (!t || st == states_.end()) return {{0}, 10, true};
    const auto it = t->animations.find(state_);
    if (it != t->animations.end() && objects::clip_problem(it->second, cut_frames_ ? cut_frames_ : t->frames).empty()) return it->second;
    return st->rule;
}

bool AnimationEditor::own() const {
    const objects::Template* t = selected();
    return t && t->animations.count(state_);
}

std::string AnimationEditor::problem() const {
    const objects::Template* t = selected();
    if (!t) return {};
    const auto it = t->animations.find(state_);
    return it == t->animations.end() ? std::string() : objects::clip_problem(it->second, cut_frames_ ? cut_frames_ : t->frames);
}

bool AnimationEditor::change_state(std::optional<objects::Clip> to, std::string label) {
    const objects::Template* t = selected();
    if (!t || state_.empty()) return false;
    AnimCommand::Look before{t->frames, t->animations}, after = before;
    if (to) after.animations[state_] = std::move(*to);
    else after.animations.erase(state_);
    if (after.animations == before.animations) return false;
    history_.execute(std::make_unique<AnimCommand>(library(), selected_, std::move(before), std::move(after), std::move(label)));
    history_.seal();
    rebuild(); // the views and the frames cut at once: the next click finds them as the file has it
    return true;
}

std::string AnimationEditor::strip_image(u32 frame) const { return frame < thumbs_.size() ? "/memory/" + thumbs_[frame] : std::string(); }

bool AnimationEditor::add_frame(u32 frame) {
    const objects::Template* t = selected();
    if (!t || frame >= std::max(1u, cut_frames_)) return false;
    objects::Clip c = own() ? t->animations.at(state_) : clip();
    if (!own()) c.frames.clear(); // a new animation of its own begins with the frame clicked
    if (c.frames.size() >= objects::kMaxClipFrames) {
        FORGE_WARN("Анимация: кадров в ней не больше %u", objects::kMaxClipFrames);
        return false;
    }
    c.frames.push_back(frame);
    return change_state(std::move(c), "«" + t->name + "»: кадр " + std::to_string(frame + 1) + " в «" + m_state_name_ + "»");
}

bool AnimationEditor::remove_last() {
    const objects::Template* t = selected();
    if (!t || !own()) return false;
    objects::Clip c = t->animations.at(state_);
    if (c.frames.size() <= 1) return change_state(std::nullopt, "«" + t->name + "»: «" + m_state_name_ + "» как в игре");
    c.frames.pop_back();
    return change_state(std::move(c), "«" + t->name + "»: последний кадр из «" + m_state_name_ + "» убран");
}

bool AnimationEditor::set_fps(f32 fps) {
    const objects::Template* t = selected();
    if (!t || objects::clip_ticks(fps) == 0) return false;
    objects::Clip c = own() ? t->animations.at(state_) : clip();
    c.fps = fps;
    return change_state(std::move(c), "«" + t->name + "»: «" + m_state_name_ + "» " + fps_words(fps) + " к/с");
}

bool AnimationEditor::set_loop(bool on) {
    const objects::Template* t = selected();
    if (!t) return false;
    objects::Clip c = own() ? t->animations.at(state_) : clip();
    c.loop = on;
    return change_state(std::move(c), "«" + t->name + "»: «" + m_state_name_ + "» " + (on ? "повторяется" : "без повтора"));
}

bool AnimationEditor::reset_state() {
    const objects::Template* t = selected();
    if (!t || !own()) return false;
    return change_state(std::nullopt, "«" + t->name + "»: «" + m_state_name_ + "» как в игре");
}

bool AnimationEditor::set_frames(int n) {
    const objects::Template* t = selected();
    if (!t) return false;
    const u32 frames = static_cast<u32>(std::clamp(n, 1, static_cast<int>(objects::kMaxFrames)));
    if (frames == t->frames) return false;
    AnimCommand::Look before{t->frames, t->animations}, after = before;
    after.frames = frames;
    history_.execute(std::make_unique<AnimCommand>(library(), selected_, std::move(before), std::move(after),
                                                   "«" + t->name + "»: кадров в картинке " + std::to_string(frames)));
    history_.seal();
    rebuild();
    return true;
}

// --- the preview ---------------------------------------------------------------

void AnimationEditor::play(bool on) {
    playing_ = on;
    time_ = static_cast<f64>(ticks_) / 60.0;
    m_playing_ = on;
    dirty("an_playing");
}

void AnimationEditor::step(int frames) {
    play(false);
    const u64 tp = std::max(1u, objects::clip_ticks(clip().fps));
    const i64 at = static_cast<i64>(ticks_ / tp) + frames;
    set_clock(static_cast<u64>(std::max<i64>(0, at)) * tp);
}

void AnimationEditor::set_clock(u64 ticks) {
    ticks_ = ticks;
    time_ = static_cast<f64>(ticks) / 60.0;
    show_frame();
}

u32 AnimationEditor::shown_frame() const { return objects::clip_frame(clip(), ticks_); }

void AnimationEditor::show_frame() {
    const u32 f = shown_frame();
    if (f == shown_ && !m_preview_.empty()) return;
    shown_ = f;
    m_preview_ = f < bigs_.size() ? "/memory/" + bigs_[f] : Rml::String();
    for (Thumb& th : m_strip_) th.shown = static_cast<u32>(th.index) == f;
    dirty("an_preview");
    dirty("an_strip");
}

void AnimationEditor::cut_frames() {
    const objects::Template* t = selected();
    if (!t || !ui_) {
        thumbs_.clear();
        bigs_.clear();
        cut_from_.clear();
        cut_frames_ = 0;
        return;
    }
    const std::string from = std::to_string(t->key) + "_" + std::to_string(t->look());
    if (from == cut_from_) return;
    cut_from_ = from;
    thumbs_.clear();
    bigs_.clear();
    cut_frames_ = 0;
    std::vector<u8> bytes;
    assets::CookedTexture image;
    if (!read_file(library().picture_file(*t), bytes) || !assets::decode_image(bytes, image) || !image.width || !image.height) return;
    // As the game cuts it: a width the frames do not divide is one frame.
    cut_frames_ = image.width % t->frames == 0 ? t->frames : 1;
    const u32 fw = image.width / cut_frames_;
    for (u32 f = 0; f < cut_frames_; ++f) {
        u32 w, h;
        const std::string thumb = "an_" + from + "_" + std::to_string(f), big = thumb + "_big";
        std::vector<u8> rgba = fit_frame(image, f * fw, fw, kThumbPx, w, h);
        ui_->set_image(thumb, rgba.data(), w, h);
        rgba = fit_frame(image, f * fw, fw, kPreviewPx, w, h);
        ui_->set_image(big, rgba.data(), w, h);
        thumbs_.push_back(thumb);
        bigs_.push_back(big);
    }
}

// --- the views -------------------------------------------------------------------

void AnimationEditor::rebuild() {
    objects::Library& lib = library();
    built_ = lib.version();
    listed_.clear();
    m_list_.clear();
    std::vector<const objects::Template*> all;
    for (const objects::Template& t : lib.templates())
        if (!t.picture.empty() && !module_.anim_states(lib, t).empty()) all.push_back(&t);
    std::sort(all.begin(), all.end(), [](const auto* a, const auto* b) {
        const std::string ka = alphabet_key(a->name), kb = alphabet_key(b->name);
        return ka != kb ? ka < kb : a->name < b->name;
    });
    if (std::none_of(all.begin(), all.end(), [&](const auto* t) { return t->key == selected_; })) selected_ = all.empty() ? 0 : all.front()->key;
    for (const objects::Template* t : all) {
        listed_.push_back(t->key);
        const usize own = t->animations.size();
        m_list_.push_back({t->name, template_icon ? template_icon(*t) : std::string(), count_words(t->frames, "кадр", "кадра", "кадров"),
                           own ? "своих анимаций: " + std::to_string(own) : std::string(), t->key == selected_});
    }
    m_note_ = !from_note_.empty() ? from_note_
              : all.empty()       ? "У шаблонов игры пока нет своих картинок. Картинку шаблону даёт вкладка «Объекты»."
                                  : "";
    const objects::Template* t = selected();
    m_has_sel_ = t != nullptr;
    states_ = t ? module_.anim_states(lib, *t) : std::vector<level::LevelModule::AnimState>{};
    if (std::none_of(states_.begin(), states_.end(), [&](const auto& s) { return s.id == state_; }))
        state_ = states_.empty() ? std::string() : states_.front().id;
    cut_frames();
    m_sel_name_ = t ? t->name : std::string();
    m_sel_note_ = t ? module_.object_note(lib, *t) : std::string();
    if (t && cut_frames_ == 0) m_sel_note_ = "Картинка " + t->picture + " не читается.";
    else if (t && cut_frames_ != t->frames)
        m_sel_note_ = "Ширина картинки не делится на " + std::to_string(t->frames) + " кадров: игра рисует её одним кадром.";
    m_frames_ = t ? static_cast<int>(t->frames) : 1;
    m_states_.clear();
    for (const auto& st : states_) {
        const auto it = t->animations.find(st.id);
        const bool mine = it != t->animations.end();
        const std::string why = mine ? objects::clip_problem(it->second, cut_frames_ ? cut_frames_ : t->frames) : std::string();
        const objects::Clip& c = mine && why.empty() ? it->second : st.rule;
        m_states_.push_back({st.id, st.name, frames_words(mine ? it->second : st.rule), fps_words(c.fps) + " к/с", why, mine, c.loop, st.id == state_});
        if (st.id == state_) m_state_name_ = st.name;
    }
    const objects::Clip c = clip();
    m_own_ = own();
    m_problem_ = problem();
    m_loop_ = c.loop;
    m_strip_.clear();
    for (u32 f = 0; f < thumbs_.size(); ++f)
        m_strip_.push_back({"/memory/" + thumbs_[f], static_cast<int>(f), std::find(c.frames.begin(), c.frames.end(), f) != c.frames.end(), false});
    m_seq_.clear();
    for (u32 f : c.frames) m_seq_.push_back({f < thumbs_.size() ? "/memory/" + thumbs_[f] : std::string(), static_cast<int>(f), true, false});
    for (usize i = 0; i < m_fps_.size(); ++i) m_fps_[i].selected = std::fabs(objects::kClipFps[i] - c.fps) < 1e-4f;
    shown_ = ~0u;
    for (const char* name : {"an_list", "an_states", "an_strip", "an_seq", "an_fps", "an_note", "an_has_sel", "an_sel_name", "an_sel_note",
                             "an_problem", "an_state_name", "an_frames", "an_own", "an_loop"})
        dirty(name);
    show_frame();
}

} // namespace forge::editor_app
