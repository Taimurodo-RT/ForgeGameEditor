// «Векторная», part 3в of step 3 of docs/models/движение-модель.md «Просмотр и звук»: the vector zoomed and panned, its
// grid, the line under the views (the zoom, the picture's point under the mouse, the eyedropper's colour), and the
// sounds of the clip's marks: their files among the game's sounds, their rows of waves on the timeline, the preview
// sounding them (pixel_editor.h). The numbers are forge/pixel/view.h and forge/pixel/sound.h.

#include "pixel_editor.h"
#include "pixel_files.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge::editor_app {

using namespace pixel_files;
namespace fs = std::filesystem;

namespace {

constexpr f64 kWavePx = 24; // a row of sounds (logical px)

// Whether the element `id` holds the element under the mouse.
bool hovering(Rml::Context* context, const char* id) {
    for (const Rml::Element* e = context ? context->GetHoverElement() : nullptr; e; e = e->GetParentNode())
        if (e->GetId() == id) return true;
    return false;
}

// Where an element's content box is on screen (context pixels); false when it is not shown.
bool content_box(Rml::Context* context, const char* id, f32& x, f32& y, f32& w, f32& h) {
    Rml::Element* el = nullptr;
    for (int i = 0; context && i < context->GetNumDocuments() && !el; ++i) el = context->GetDocument(i)->GetElementById(id);
    if (!el || !el->IsVisible(true)) return false;
    const Rml::Vector2f at = el->GetAbsoluteOffset(Rml::BoxArea::Content), size = el->GetBox().GetSize(Rml::BoxArea::Content);
    x = at.x, y = at.y, w = size.x, h = size.y;
    return w > 0 && h > 0;
}

// The texel of a picture shown in a box of the screen under (x, y), as "#rrggbb" or "пусто"; "" outside it.
std::string texel_words(const assets::CookedTexture& t, f32 bx, f32 by, f32 bw, f32 bh, f32 x, f32 y) {
    if (t.width == 0 || t.height == 0 || bw <= 0 || bh <= 0 || x < bx || y < by || x >= bx + bw || y >= by + bh) return {};
    const u32 tx = std::min(t.width - 1, static_cast<u32>((x - bx) * static_cast<f32>(t.width) / bw));
    const u32 ty = std::min(t.height - 1, static_cast<u32>((y - by) * static_cast<f32>(t.height) / bh));
    const std::string words = pixel::color_words(&t.rgba8[(static_cast<usize>(ty) * t.width + tx) * 4]);
    return words.empty() ? std::string("пусто") : words;
}

} // namespace

void PixelEditor::bind_view(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<SoundView>()) {
        s.RegisterMember("name", &SoundView::name);
        s.RegisterMember("wave", &SoundView::wave);
        s.RegisterMember("style", &SoundView::style);
        s.RegisterMember("title", &SoundView::title);
        s.RegisterMember("missing", &SoundView::missing);
    }
    model.RegisterArray<std::vector<SoundView>>();
    model.Bind("px_sounds", &m_sounds_);
    model.Bind("px_zoom_words", &m_zoom_words_);
    model.Bind("px_coords", &m_coords_);
    model.Bind("px_picked", &m_picked_);
    model.Bind("px_pick_swatch", &m_pick_swatch_);
    model.Bind("px_grid", &m_grid_);
    model.Bind("px_picking", &m_picking_);
    model.Bind("px_sound_on", &m_sound_on_);
    model.BindEventCallback("px_grid_toggle", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { set_grid(!grid_); });
    model.BindEventCallback("px_picking_toggle", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { set_picking(!picking_); });
    model.BindEventCallback("px_sound_toggle", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { set_sound(!sound_on_); });
}

// --- the zoom -----------------------------------------------------------------------------------------

pixel::Affine PixelEditor::to_view() const { return zoom_.affine() * rig_.to_pixels(kViewPx, kViewPx); }

pixel::Affine PixelEditor::to_texels() const {
    // The view's zoom in its texels: the same steps, its pan view_px() / kViewPx texels a point.
    const f64 k = static_cast<f64>(view_px()) / kViewPx, z = zoom_.zoom();
    return pixel::Affine{z, 0, 0, z, zoom_.ox * k, zoom_.oy * k} * rig_.to_pixels(view_px(), view_px());
}

void PixelEditor::fit_box() {
    zoom_.width = zoom_.height = kViewPx;
    const f64 w = rig_.width(), h = rig_.height();
    if (rig_.loaded() && w > 0 && h > 0) {
        const f64 s = std::min(kViewPx / w, kViewPx / h);
        zoom_.box[0] = (kViewPx - w * s) / 2, zoom_.box[1] = (kViewPx - h * s) / 2;
        zoom_.box[2] = (kViewPx + w * s) / 2, zoom_.box[3] = (kViewPx + h * s) / 2;
    } else {
        zoom_.box[0] = zoom_.box[1] = 0;
        zoom_.box[2] = zoom_.box[3] = kViewPx;
    }
    zoom_.clamp();
    const std::string words = pixel::zoom_words(zoom_.step);
    if (words != m_zoom_words_) {
        m_zoom_words_ = words;
        dirty("px_zoom_words");
    }
}

void PixelEditor::zoom_view(i32 step, f32 x, f32 y) {
    if (!has_) return;
    fit_box();
    // The point of the view under (x, y); its middle when it is not laid out.
    vector::Point at{kViewPx / 2.0, kViewPx / 2.0};
    if (view_w_ > 0 && view_h_ > 0) at = {(x - view_x_) * static_cast<f64>(kViewPx) / view_w_, (y - view_y_) * static_cast<f64>(kViewPx) / view_h_};
    zoom_.zoom_to(step, at);
    fit_box();
    view_dirty_ = true;
}

void PixelEditor::zoom_view_by(int steps) {
    if (!has_) return;
    fit_box();
    zoom_.zoom_to(zoom_.step + steps, {kViewPx / 2.0, kViewPx / 2.0});
    fit_box();
    view_dirty_ = true;
}

void PixelEditor::fit_view() {
    zoom_.fit();
    fit_box();
    view_dirty_ = true;
}

void PixelEditor::pan_view(f32 dx, f32 dy) {
    if (view_w_ <= 0 || view_h_ <= 0) return;
    fit_box();
    zoom_.pan(dx * static_cast<f64>(kViewPx) / view_w_, dy * static_cast<f64>(kViewPx) / view_h_);
    view_dirty_ = true;
}

// --- the grid, the line under the views, the eyedropper ---------------------------------------------------

void PixelEditor::set_grid(bool on) {
    grid_ = m_grid_ = on;
    dirty("px_grid");
    view_dirty_ = true;
}

void PixelEditor::set_picking(bool on) {
    picking_ = m_picking_ = on;
    dirty("px_picking");
    if (!on) picked_.clear();
    track_mouse(context_);
}

void PixelEditor::track_mouse(Rml::Context* context) {
    std::string coords, picked;
    if (has_ && rig_.loaded() && view_w_ > 0 && hovering(context, "px-vector")) {
        const std::string over = texel_words(vector_, view_x_, view_y_, view_w_, view_h_, mouse_x_, mouse_y_);
        if (!over.empty()) {
            coords = pixel::coords_words(view_to_picture(mouse_x_, mouse_y_));
            if (picking_) picked = over;
        }
    }
    if (has_ && picking_ && picked.empty() && hovering(context, "px-pixel") &&
        content_box(context, "px-pixel-img", pixel_x_, pixel_y_, pixel_w_, pixel_h_))
        picked = texel_words(frame_px_, pixel_x_, pixel_y_, pixel_w_, pixel_h_, mouse_x_, mouse_y_);
    if (coords != coords_) {
        coords_ = m_coords_ = coords;
        dirty("px_coords");
    }
    if (picked != picked_ || m_picked_ != picked) {
        picked_ = m_picked_ = picked;
        m_pick_swatch_ = picked.rfind('#', 0) == 0 ? "background-color: " + picked + ";" : std::string("display: none;");
        dirty("px_picked");
        dirty("px_pick_swatch");
    }
}

bool PixelEditor::copy_coords() {
    if (coords_.empty()) {
        note_ = "P копирует координаты под мышью: наведите её на вектор";
        rebuild();
        return false;
    }
    SDL_SetClipboardText(coords_.c_str());
    note_ = "Скопировано: " + coords_;
    rebuild();
    return true;
}

bool PixelEditor::pick_click(f32 x, f32 y) {
    if (!picking_) return false;
    mouse_x_ = x, mouse_y_ = y;
    track_mouse(context_);
    // Nothing under it, or nothing drawn there: nothing copied.
    if (picked_.rfind('#', 0) != 0) return true;
    SDL_SetClipboardText(picked_.c_str());
    note_ = "Скопирован цвет " + picked_;
    rebuild();
    return true;
}

// --- the sounds ---------------------------------------------------------------------------------------------

void PixelEditor::set_sound(bool on) {
    sound_on_ = m_sound_on_ = on;
    dirty("px_sound_on");
    if (!on) mixer_.stop_all();
}

void PixelEditor::away() {
    if (mixer_.voices() == 0) return;
    mixer_.stop_all();
    if (!mixer_.has_device()) mixer_.advance(1); // what was stopped ends
}

void PixelEditor::find_sounds() {
    std::map<std::string, SoundFound> found;
    const fs::path folder = game_dir.empty() ? fs::path() : game_dir / "sounds";
    if (rec_ok_ && !folder.empty())
        for (const pixel::Mark& m : rec_.marks) {
            if (m.kind != pixel::Mark::Kind::Sound || found.count(m.name)) continue;
            SoundFound f;
            for (const fs::path& p : pixel::sound_files(folder, m.name)) f.stamps.push_back(stamp_of(p));
            // The same files as they were: what was read of them stays.
            const auto was = sound_files_.find(m.name);
            f.file = was != sound_files_.end() && was->second.stamps == f.stamps ? was->second.file : pixel::find_sound(folder, m.name);
            found[m.name] = std::move(f);
        }
    sound_files_ = std::move(found);
}

bool PixelEditor::sounds_changed() {
    const fs::path folder = game_dir.empty() ? fs::path() : game_dir / "sounds";
    for (const auto& [name, f] : sound_files_) {
        std::vector<i64> now;
        for (const fs::path& p : pixel::sound_files(folder, name)) now.push_back(stamp_of(p));
        if (now != f.stamps) return true;
    }
    return false;
}

void PixelEditor::lay_sounds() {
    // The same timeline, recording and files: the rows as they are.
    const f64 dp = std::max(1e-3f, dp_);
    const u32 cols = tl_w_ > 0 ? static_cast<u32>(std::lround(tl_w_)) : 0, high = static_cast<u32>(std::lround(kWavePx * dp));
    std::string key;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d %.17g %.17g %u %u %.17g|", rec_ok_ ? 1 : 0, tv_.t(0), tv_.per_second(), cols, high, rec_.length);
    key = buf;
    if (rec_ok_)
        for (const pixel::Mark& m : rec_.marks) {
            if (m.kind != pixel::Mark::Kind::Sound) continue;
            const auto f = sound_files_.find(m.name);
            std::snprintf(buf, sizeof buf, "%.17g %d %p %u|", m.time, m.site.site, f == sound_files_.end() ? nullptr : static_cast<const void*>(f->second.file.clip.get()),
                          m.site.file);
            key += m.name + " " + buf;
        }
    for (const auto& [name, f] : sound_files_) key += name + ":" + f.file.why + "|";
    if (key == wave_key_) return;
    wave_key_ = key;

    // The rows: one a name, in the order of its first mark.
    std::vector<SoundRow> rows;
    if (rec_ok_)
        for (const pixel::Mark& m : rec_.marks) {
            if (m.kind != pixel::Mark::Kind::Sound) continue;
            if (std::any_of(rows.begin(), rows.end(), [&](const SoundRow& r) { return r.name == m.name; })) continue;
            SoundRow r;
            r.name = m.name;
            if (const auto f = sound_files_.find(m.name); f != sound_files_.end()) {
                r.file = f->second.file.file;
                r.found = f->second.file.clip != nullptr;
                r.why = f->second.file.why;
            } else {
                r.why = "звук «" + m.name + "»: у игры нет папки sounds";
            }
            if (m.site.file == 0 && m.site.site >= 0 && m.site.site < static_cast<i32>(script_.code().sites().size()))
                r.line = script_.code().sites()[static_cast<usize>(m.site.site)].line;
            rows.push_back(std::move(r));
        }
    // Their waves: a column a point of the timeline, the clip's end cutting them.
    waves_.assign(rows.size(), {});
    for (usize i = 0; i < rows.size(); ++i) {
        SoundRow& r = rows[i];
        r.columns.assign(cols, 0);
        const auto f = sound_files_.find(r.name);
        if (!r.found || f == sound_files_.end() || cols == 0) continue;
        const f64 t0 = tv_.t(0), dt = 1 / tv_.per_second();
        for (const pixel::Mark& m : rec_.marks)
            if (m.kind == pixel::Mark::Kind::Sound && m.name == r.name) pixel::add_wave(*f->second.file.clip, m.time, rec_.length, t0, dt, r.columns);
        // Where it sounds at all: the middle line there even when it is silent.
        std::vector<bool> sounding(cols, false);
        for (const pixel::Mark& m : rec_.marks) {
            if (m.kind != pixel::Mark::Kind::Sound || m.name != r.name) continue;
            const f64 end = std::min(rec_.length, m.time + f->second.file.clip->seconds());
            for (u32 c = 0; c < cols; ++c) {
                const f64 a = t0 + c * dt, b = a + dt;
                if (b > m.time && a < end) sounding[c] = true;
            }
        }
        assets::CookedTexture& t = waves_[i];
        t.width = cols, t.height = high;
        t.rgba8.assign(static_cast<usize>(cols) * high * 4, 0);
        for (u32 c = 0; c < cols; ++c) {
            if (!sounding[c]) continue;
            // 1: the row's height; 0: the line in its middle.
            const f64 half = std::max(0.5, std::min<f64>(1, r.columns[c]) * high / 2);
            for (u32 y = 0; y < high; ++y)
                if (std::fabs(y + 0.5 - high / 2.0) <= half) std::fill_n(&t.rgba8[(static_cast<usize>(y) * cols + c) * 4], 4, u8{255});
        }
    }
    sound_rows_ = std::move(rows);
    // Their pictures: the ones before the last are shown by nothing now.
    if (ui_) {
        for (const std::string& old : wave_images_[1]) ui_->drop_image(old);
        wave_images_[1] = std::move(wave_images_[0]);
        wave_images_[0].clear();
    }
    m_sounds_.clear();
    for (usize i = 0; i < sound_rows_.size(); ++i) {
        const SoundRow& r = sound_rows_[i];
        SoundView v;
        v.name = r.name;
        v.missing = !r.found;
        v.title = r.found ? "sound(\"" + r.name + "\"): " + path_to_utf8(r.file.filename()) : r.why;
        if (r.found && ui_ && !waves_[i].rgba8.empty()) {
            const std::string name = "px_w_" + std::to_string(++image_serial_);
            ui_->set_image(name, waves_[i].rgba8.data(), waves_[i].width, waves_[i].height);
            wave_images_[0].push_back(name);
            v.wave = "/memory/" + name;
            char style[96];
            std::snprintf(style, sizeof style, "width: %udevpx; height: %udevpx;", waves_[i].width, waves_[i].height);
            v.style = style;
        } else {
            v.style = "display: none;";
        }
        m_sounds_.push_back(std::move(v));
    }
    dirty("px_sounds");
}

void PixelEditor::sound_frames(i64 now, bool wrapped) {
    const i64 shown = sound_shown_;
    sound_shown_ = now;
    if (!sound_on_ || !rec_ok_ || !cut_.code) return;
    for (usize i : pixel::passed_sounds(rec_, cut_, shown, now, wrapped)) {
        const pixel::Mark& m = rec_.marks[i];
        const auto f = sound_files_.find(m.name);
        if (f == sound_files_.end() || !f->second.file.clip) continue; // a red mark does not sound
        if (!mixer_tried_ && !offscreen) {
            mixer_tried_ = true;
            if (!mixer_.open()) FORGE_WARN("Звук просмотра: устройства нет (%s), звуки идут без него", SDL_GetError());
        }
        mixer_.play(f->second.file.clip);
        sounded_.push_back({static_cast<u32>(pixel::sound_frame(cut_, m.time)), m.name});
    }
}

} // namespace forge::editor_app
