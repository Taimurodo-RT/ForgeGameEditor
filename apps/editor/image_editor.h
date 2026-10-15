#pragma once

// The «Изображения» tab: a picture of «Ресурсы» edited pixel by pixel (editors/изображения-модель.md).
//
// Left, the pictures of «Ресурсы» by folder; «Открыть в редакторе изображений» there opens one here. Under them the
// pictures of the game that «Ресурсы» has not (in its pictures/, not a copy sources.json keeps): shown only, with where
// they are from and why they are not changed here. In the middle the
// canvas: the picture at 1–32 times (Ctrl with the wheel, «1:1», «Вписать»), moved with the middle button or Space with
// the mouse, a chequerboard under what is clear, the pixel grid from 8 times, the grid of the strip's frames; under it
// the size, the format, the point and colour under the mouse, the frame. Right, four sections:
//   - «Рисование»: pencil 1–4 px, eraser, fill (exactly the colour, joined by sides), eyedropper (I, or Alt and a
//     click), a line from the last point with Shift; two colours (left and right button, X swaps them): a click on
//     either opens the editor's colour picker (13.4: HSV, HEX, alpha, the game's colours, which are copied, and its
//     eyedropper on the picture); the picture's colours as swatches; the area: the whole picture or «Кадр N» (the
//     brush, the effects and the transformations keep to it);
//   - «Трансформации»: turns and mirrors of the area, the size (nearest pixel or smooth), the canvas with its anchor,
//     trim, «Сетка кадров» (a sheet of frames into the strip the game reads);
//   - «Эффекты»: shown on the canvas while their sliders move, «Применить» makes them a step, «Отмена» and Esc drop them;
//   - «Формат»: Ctrl+S writes the picture where it is (the same file, the same id of «Ресурсы»: the game's copies follow
//     it through sources.json), «Сохранить как…» PNG, JPG, BMP, TGA, WebP: a new file beside it, a new resource of its
//     own id; what used the old one keeps using it.
//
// Each picture opened has a working copy of its own, RGBA 8 bits, nothing lost between steps, and a history of its
// own: a stroke from press to release, an effect, a transformation is one step that keeps the pixels and the size
// before and after. The file is written only by saving; until then the rest of the editor and the game see the file as
// it was, the tab marks the picture unsaved, and another picture opened keeps it so. A lossy format (JPEG) is encoded
// from the working copy once, when saved. A file changed outside while its picture has unsaved changes is not written
// over without the author's choice; one changed while there are none is taken as it is now (a step: Ctrl+Z goes back).
//
// A picture shown only (of the game, not of «Ресурсы») has no history and is never written: drawing, effects,
// transformations, Ctrl+S and «Сохранить как…» say why, whatever asks for them (a button, a key, a method). It gets no
// id of «Ресурсы», no line in sources.json and no copy; a file changed outside, gone or come back is shown as it is, and
// one that became a copy of «Ресурсы» since stays shown only (its picture of «Ресурсы» opens apart, as itself).

#include "color_picker.h"

#include "forge/assets/image_edit.h"
#include "forge/data/guid.h"
#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace forge::editor_app {

class AssetLibrary;

class ImageEditor : public ColorPicker::Client {
public:
    explicit ImageEditor(AssetLibrary& assets);
    ~ImageEditor();

    bool init(ui::Ui& ui);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    // The editor's one colour picker (13.4), shared with «Интерфейс».
    void set_picker(ColorPicker* picker) { cp_ = picker; }
    // Each frame; context: only while the tab is open (the canvas is drawn for its size then).
    void update(Rml::Context* context);
    bool handle_event(const SDL_Event& e, f32 density, bool ui_used, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);
    // «Ресурсы» looked at its files again: a picture moved or renamed keeps its place here, a file changed outside is
    // taken or said.
    void files_changed();

    // The open picture's history (an empty one without a picture).
    editor::UndoStack& history();
    void undo();
    void redo();
    std::string status() const;

    // --- what main gives it ---
    // The templates drawn with this asset (its copy among the game's pictures): their names and «Кадров в картинке».
    struct User {
        std::string name;
        u32 frames = 1;
    };
    std::function<std::vector<User>(const Guid& asset)> users_of;
    // After a file was written: «Ресурсы» looks at it (the game's copies follow).
    std::function<void()> written;
    // How a file is written (the self-test: a write that fails).
    std::function<bool(const std::filesystem::path&, std::span<const u8>)> write_file;
    // The game's folder: the pictures of its pictures/ that «Ресурсы» has not are shown for viewing.
    std::filesystem::path game_dir;
    // The picture of «Ресурсы» a file of the game ("pictures/рубин.png") is a copy of, by sources.json ("" none).
    std::function<std::string(const std::string& game_file)> copy_from;
    // The templates drawn with a file of the game ("pictures/рубин.png").
    std::function<std::vector<User>(const std::string& game_file)> users_of_file;
    // The game's frame limit (game.json): the tab's own number of frames is not above it. 64 when not given.
    std::function<u32()> frame_limit;

    // --- actions (the buttons, the mouse, the keys, the self-test) ---
    // A picture of «Ресурсы» (its file); false when it is none or does not read (note() says why). A file of the
    // game's pictures/ is shown only, as open_game.
    bool open(const std::filesystem::path& file);
    // A picture of the game ("pictures/интерфейс/рамка_дерево.png"), shown only.
    bool open_game(const std::string& game_file);
    // The open picture is shown only: a picture of the game; its path there; the picture of «Ресурсы» it became a
    // copy of since it was opened; its file gone.
    bool view_only() const;
    std::string game_file() const;
    std::string copy_of() const;
    bool missing() const;
    bool has_picture() const { return current_ != nullptr; }
    std::filesystem::path file() const;
    std::string rel() const;
    Guid asset_id() const;
    // The working copy, and what the canvas shows (with an effect being tried).
    const assets::CookedTexture& image() const;
    const assets::CookedTexture& shown() const;
    // The open picture has changes not written; every open picture with such, in words.
    bool dirty() const;
    std::vector<std::string> unsaved() const;
    // Ctrl+S: the open picture into its file. False (note() says why) when it is not written: then the file, the
    // game's copies and the working copy are as they were.
    bool save();
    // Every open picture with unsaved changes (error: the first that did not save).
    bool save_all(std::string& error);
    // «Сохранить как…» .png .jpg .bmp .tga .webp: a new file beside the picture, a new resource; the tab goes on with
    // it. A JPG of a picture with pixels not fully opaque asks first: false, warning() says what is lost; again with
    // confirm writes it.
    bool save_as(const std::string& extension, bool confirm = false);
    const std::string& warning() const { return warning_; }
    // The open picture's file changed outside while it has unsaved changes: until the author chooses, Ctrl+S does
    // not write. «Взять с диска»: the file as it is now (a step: Ctrl+Z gives the changes back); «Оставить мои»: the
    // next Ctrl+S writes over it.
    bool outside() const;
    bool take_disk();
    bool keep_mine();

    // The pixel editor.
    void set_tool(const std::string& tool); // "pencil", "eraser", "fill", "picker"
    const std::string& tool() const { return tool_; }
    void set_size(u32 px);
    u32 size() const { return size_; }
    // The colours: HEX as the picker takes it ("#rgb", "#rrggbb", "#rrggbbaa"...; false: not a colour).
    bool set_color(const std::string& hex, bool secondary = false);
    assets::Rgba color(bool secondary = false) const { return secondary ? second_ : first_; }
    // The game colour a colour was copied from, its name ("" none): the brush keeps the pixels' RGBA, not the link.
    const std::string& color_theme(bool secondary = false) const { return secondary ? second_theme_ : first_theme_; }
    void swap_colors();
    // A click on the left (or right) colour: the picker opens beside the swatch at (x, y, w, h), window pixels.
    // What it picks is the brush's colour at once; «Отмена» and Esc put the one before back. The picture, its file and
    // its history do not change either way.
    bool open_color(bool secondary, f32 x = 0, f32 y = 0, f32 w = 0, f32 h = 0);
    bool picking() const { return cp_ && cp_->open_for(*this); }
    // The picture's colours as swatches (up to 64, the most used first).
    const std::vector<assets::Rgba>& colors() const { return colors_; }
    // The area: -1 the whole picture, else that frame of the strip (from 0).
    bool set_area(int frame);
    int area() const;
    assets::PixelRect area_rect() const;
    // The strip's frames: the «Кадров в картинке» of a template drawn with the picture, else the tab's own number.
    u32 frames() const;
    std::string frames_from() const;
    // The tab's own number (1..frame_limit; above it, false and note() says the limit and where it changes).
    bool set_frames(u32 n);
    // The mouse on the picture, in its pixels: what the tool does from press to release (one step).
    void press(i32 x, i32 y, bool secondary = false, bool shift = false, bool alt = false);
    void drag(i32 x, i32 y);
    void release();

    // Transformations: of the area (each frame by itself when the area is the whole of a strip of several).
    bool turn(assets::Turn turn);
    bool resize(u32 width, u32 height, bool smooth);
    bool canvas(u32 width, u32 height, assets::Anchor anchor);
    bool trim();
    // «Сетка кадров»: frames frame_w × frame_h, `columns` in a row → a strip; the tab's frames become their number.
    bool grid_to_strip(u32 frame_w, u32 frame_h, u32 columns);
    // What the last action said (a refusal, what was done).
    const std::string& note() const { return note_; }

    // Effects: tried on the canvas, then applied as one step or dropped.
    bool try_effect(const assets::Effect& effect);
    bool apply_effect();
    void cancel_effect();
    bool trying() const { return trying_; }

    // The view.
    void set_zoom(u32 zoom);
    u32 zoom() const;
    void fit();
    // Where the middle of a pixel of the picture is, in the context's pixels (false: not on the canvas now).
    bool screen_of(f32 x, f32 y, f32& sx, f32& sy) const;
    // The canvas as drawn last: its pixels (RGBA, rows top to bottom) and size.
    const std::vector<u8>& canvas_pixels() const { return canvas_; }
    u32 canvas_width() const { return canvas_w_; }
    u32 canvas_height() const { return canvas_h_; }
    // The pictures the list shows (their paths in «Ресурсы»), those of the game shown only (their paths in the game),
    // and those open with their working copies.
    std::vector<std::string> listed() const;
    std::vector<std::string> game_listed() const;
    usize open_count() const { return pictures_.size(); }

    struct Picture;

private:
    struct ListRow {
        Rml::String rel, name, folder;
        bool header = false, selected = false, unsaved = false, game = false;
        int index = 0;
    };
    struct Swatch {
        Rml::String css, hex;
    };
    struct AreaChip {
        Rml::String label;
        int frame = -1;
        bool selected = false;
    };
    struct EffectChip {
        Rml::String id, label;
        bool selected = false;
    };
    struct Param {
        Rml::String label;
        int min = 0, max = 100, value = 0;
    };

    Picture* current() const { return current_; }
    Picture* find(const Guid& id) const;
    // The open picture can be changed: true; one shown only says why `what` is not done (false).
    bool editable(const std::string& what);
    std::string view_reason(const Picture& p) const;
    // The game's pictures that «Ресурсы» has not (their paths in the game, "pictures/…").
    std::vector<std::string> game_pictures() const;
    // ColorPicker::Client: the brush's colour.
    void picker_show(editor::design::Color color, const std::string& link) override;
    void picker_closed(bool keep, bool live) override;
    bool picker_sample(f32 x, f32 y) override;
    std::string picker_title() const override;
    std::string picker_note() const override;
    std::string picker_dropper_hint(bool live) const override;
    // A step of the picture's history: its pixels in `changed` (or all of it, when its size changed) before and after;
    // frames_before: the tab's own number of frames before, when the step changed it (0: it did not).
    bool commit(Picture& p, const assets::CookedTexture& before, assets::PixelRect changed, std::string label, u32 frames_before = 0);
    // The open picture changed (its pixels, its size, its file): the canvas and the panels are made again.
    void touched();
    void rebuild_list();
    void rebuild_panels();
    void draw_canvas(Rml::Context* context);
    bool to_pixel(f32 sx, f32 sy, i32& x, i32& y) const;
    bool over_canvas(Rml::Context* context) const;
    void sync_hover();
    bool read_disk(Picture& p, std::vector<u8>& bytes, assets::CookedTexture& image, std::string& why) const;
    bool write_picture(Picture& p, const std::filesystem::path& to, const std::string& extension, std::string& why);
    void effect_from_panel();
    void say(std::string text);
    void dirty(const char* name) {
        if (model_) model_.DirtyVariable(name);
    }

    AssetLibrary& assets_;
    ui::Ui* ui_ = nullptr;
    ColorPicker* cp_ = nullptr;
    Rml::Context* context_ = nullptr; // the editor's, while the tab is shown
    Rml::DataModelHandle model_;
    editor::Document empty_doc_;
    editor::UndoStack empty_history_{empty_doc_};
    std::vector<std::unique_ptr<Picture>> pictures_;
    Picture* current_ = nullptr;

    // The pixel editor.
    std::string tool_ = "pencil";
    u32 size_ = 1;
    assets::Rgba first_{0, 0, 0, 255}, second_{255, 255, 255, 255};
    std::vector<std::string> game_files_; // game_pictures() as last looked for
    bool game_files_known_ = false;
    std::string first_theme_, second_theme_; // the game colours they were copied from (names)
    // The colour the picker is open on: which, and what it was when it opened.
    bool picking_second_ = false;
    assets::Rgba picking_was_{};
    std::string picking_was_theme_;
    std::vector<assets::Rgba> colors_;
    bool stroking_ = false, stroke_second_ = false;
    assets::CookedTexture stroke_before_;
    assets::PixelRect stroke_rect_;
    i32 last_x_ = 0, last_y_ = 0;
    bool have_last_ = false;
    std::string stroke_label_;
    // Effects.
    assets::CookedTexture tried_;
    assets::PixelRect tried_changed_;
    assets::Effect tried_effect_;
    bool trying_ = false;
    std::string effect_ = "hue";
    int params_[3] = {0, 0, 0};
    // The canvas.
    std::vector<u8> canvas_;
    u32 canvas_w_ = 0, canvas_h_ = 0;
    f32 view_x_ = 0, view_y_ = 0; // where the canvas is in the context
    i64 origin_x_ = 0, origin_y_ = 0; // where the picture's pixel 0, 0 is on the canvas
    std::string canvas_name_, canvas_old_;
    u64 canvas_serial_ = 0;
    bool canvas_wanted_ = true;
    bool panning_ = false, space_ = false;
    f32 pan_from_x_ = 0, pan_from_y_ = 0;
    f32 mouse_x_ = -1, mouse_y_ = -1; // in the context
    i32 hover_x_ = -1, hover_y_ = -1;
    std::string note_, warning_;
    std::string jpg_asked_; // the picture whose JPG warning was shown (its id)
    u64 list_version_ = ~0ull;
    std::string resize_for_; // the picture (and size) the fields of size and canvas were filled for
    u64 colors_version_ = 0;
    const Picture* colors_of_ = nullptr;

    // Model mirrors.
    std::vector<ListRow> m_list_;
    std::vector<Swatch> m_colors_;
    std::vector<AreaChip> m_areas_;
    std::vector<EffectChip> m_effects_;
    std::vector<Param> m_params_;
    Rml::String m_name_, m_info_, m_status_, m_canvas_, m_tool_ = "pencil", m_c1_ = "rgba(0,0,0,255)", m_c2_ = "rgba(255,255,255,255)", m_c1_hex_, m_c2_hex_,
        m_color_note_, m_frames_from_, m_note_, m_warning_, m_outside_, m_effect_ = "hue", m_view_text_, m_view_copy_;
    Rml::String m_rw_, m_rh_, m_cw_, m_ch_, m_gw_ = "32", m_gh_ = "32", m_gc_ = "2";
    int m_canvas_w_ = 0, m_canvas_h_ = 0, m_size_ = 1, m_frames_ = 1, m_zoom_ = 1, m_anchor_ = 4;
    bool m_has_ = false, m_unsaved_ = false, m_smooth_ = false, m_trying_ = false, m_outside_shown_ = false, m_frames_own_ = true, m_view_ = false;
};

} // namespace forge::editor_app
