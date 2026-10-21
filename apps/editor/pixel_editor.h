#pragma once

// «Векторная» of the tab «Анимация» (step 2 of docs/models/вектор-модель.md): a vector character of «Ресурсы» posed
// by its script and baked into a strip of pixel frames for a template of the game.
//
// The left column lists the project's «*.pixel.json» (name, frame size, clips) and the SVGs a new one can be made of
// (a «.pixel.json» and a «.luau» beside the SVG). The middle shows the frame twice: the vector as ThorVG draws it in
// the pose (the selected part's bounds and pivot over it) and the pixel frame exactly as it bakes, at its size and
// bigger without smoothing; under them the frames of the selected clip (Space, ← and → as in «Покадровая»; «Калька»
// shows the frames either side faintly). The right column has the SVG's parts as they nest (a click on the vector
// selects the part under it, the selected part's pivot is dragged with the mouse), the script's parameters and seed,
// the clips (a state of the game's module, frames, frames a second from «Покадровая»'s list, «Повторять»), the baking
// (frame size, the SVG's colours or the game's, an outline), «Запечь» and «Отдать шаблону…».
//
// Every change is written into the «.pixel.json» at once and is one step of the tab's history (Ctrl+Z), shared with
// «Покадровая». «Запечь» writes the strip as a picture of «Ресурсы» beside the «.pixel.json» (the same asset again on
// the next bake, so the game's copies follow) and records its hash: a picture changed since («Изображения») is not
// baked over without asking. «Отдать шаблону…» gives a template of the game the picture, «Кадров в картинке» and an
// animation for each baked clip of a state it plays. The script and the SVG are read again when their files change on
// disk; a script that fails says its line, and the views stay on the last pose that worked.
//
// Step 3 of docs/models/движение-модель.md («Вкладка», part 3а «Код и время»): the parts' tree goes left under the
// characters; the script is in a code panel (line numbers, Ctrl+S saves it as one step, Ctrl+Z and Ctrl+Y undo the
// typing since then) with the console under it (log lines at their time, errors with their line, the tab's
// warnings); the player and the timeline go along the bottom: the clips' chips, a ruler of frames and seconds zoomed
// round the mouse, the range of a clip of the code played and baked, the time events' flags dragged by frames into the
// file, marks, pauses and the tracks' blocks of the recording, each lit in the code. A block's info is on the right.
//
// Part 3б «Правки мышью»: the mouse writes into the code. A block's edge on the timeline is its length or its start
// (at, delay), a pause's edge its length; the inspector's fields are the selected block's value, length and timing
// (the list of timings with their curves); a part dragged in the vector is the end of its transition going in this
// frame, a field of the other's table, or a line at(t, part("…"):to({ x, y }, 0)). Each edit is one step that writes
// the script and the «.pixel.json»; unsaved text in the panel is saved first, a step of its own. The inspector shows
// the selected part's values in the frame and «К коду»; the tree's row under the mouse outlines its part.
//
// Part 3в «Просмотр и звук»: the vector zooms in steps of ×1.25 to ×16 (0, =, −, the wheel round the mouse) and pans
// with the middle button, the picture kept over the view; ' lays a grid over it; the line under the views has the
// zoom, the picture's point under the mouse (P copies it) and, with the eyedropper (I), the colour under it (a click
// copies it). sound("шаг") is the game's sounds/шаг, шаг.wav or шаг.ogg: a row of its waves on the timeline, red with
// a console line when there is none; the preview sounds each mark in the frame it first shows its moment, while M is
// on, and remembers what it sounded since it was started.
//
// Part 4е of step 4 «Фигуры мышью»: a row over the views has the tools (a rect, an ellipse, a line, the pen), the colour
// of new shapes and «Привязка»; a shape drawn is a new last line of the code of the drawing the frame shows. The
// selected part's «Фигура» has the fields of the call that drew it, each written into the call.
//
// Part 6а of docs/models/сцены-модель.md «Сцена и актёры» (pixel_scene.cpp): «Сцены» under the characters, each a
// «.scene.json» beside its code; a scene opens as a character does, its code the panel's, its stage the vector's (one
// clip «scene» of the timeline), its actors and their parts in the tree. A part, a picture, an actor or an actor's
// part dragged writes the scene's code as a character's part does; the actors' clips are a row each on the timeline,
// a clip's start dragged by frames. A scene has no pixel view, no baking and no giving to a template.
//
// Part 6б «Камера и переходы»: the vector shows the whole scene with the camera's frame over it (what the player sees at
// that moment), its edge dragged writing the camera's place as a part's drag writes; the small view is «Через камеру»,
// the player's frame (Scene::draw_frame) with the transition in or out over it, and «Снимок кадра» takes that. The tree's
// first row is «Камера», its fields x, y, zoom and rot; the timeline has the camera's tracks, its row «камера · следит»
// and a row of the transitions, each chosen showing its kind, length and call.
//
// Step 7 «Вид по образцу», part 7а «Раскладка» (pixel_look.cpp): a bar of the path, the three views and «Экспорт» over
// the tab; the tools' rail, the left panel with «Слои» and «Файлы», the middle (the code in «Код», the vector filling
// its frame, «Пиксели») over the bottom panel («Таймлайн», «Проблемы N», «Консоль»), the right panel («Свойства»,
// «Анимация», or «Экспорт» in their place) all down the side. The vector's points are the px of the interface of its
// frame, whatever its size; ×1 fits the picture in it.

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/input/input.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/draw_edit.h"
#include "forge/pixel/scene.h"
#include "forge/pixel/sound.h"
#include "forge/pixel/timeline.h"
#include "forge/pixel/view.h"
#include "forge/ui/ui.h"

#include "object_library.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace forge::editor_app {

class AssetLibrary;

class PixelEditor {
public:
    PixelEditor(AssetLibrary& assets, level::LevelModule& module, editor::UndoStack& history);
    ~PixelEditor();

    bool init(ui::Ui& ui);
    void bind(Rml::DataModelConstructor& model);
    void set_model(Rml::DataModelHandle handle) { model_ = handle; }
    // Each frame while the mode is shown: files looked at, the clock, the views.
    void update(f64 dt, Rml::Context* context);
    bool handle_event(const SDL_Event& e, f32 density, Rml::Context* context);
    // The wheel over the timeline, before the UI scrolls anything under it: zoom round the mouse, with Shift along.
    bool wheel(const SDL_Event& e, f32 density, Rml::Context* context);
    bool handle_key(const SDL_KeyboardEvent& k);
    // Keys while the code panel has the keyboard (main asks first): Ctrl+S, Ctrl+Z, Ctrl+Y, Tab.
    bool panel_key(const SDL_KeyboardEvent& k, Rml::Context* context);
    std::string status() const;

    // main wires these.
    CopyIn copy_in;                                                  // a picture of «Ресурсы» into the game
    std::function<std::vector<pixel::Rgba8>()> game_colors;          // «Цвета игры»
    std::function<std::string(const objects::Template&)> template_icon;
    std::function<std::string(const Guid&)> game_copy;               // the game's picture copied from that asset, or ""
    std::filesystem::path game_dir;                                  // its game.json holds «Предел кадров»
    std::function<std::vector<input::Action>()> game_actions;        // the game's actions as «Ввод» has them (part 6в)

    // --- the characters (buttons, keys, the self-test) ---
    // The «.pixel.json» of the project, by path in «Ресурсы».
    const std::vector<std::string>& listed() const { return listed_; }
    bool open(const std::string& rel);
    const std::string& opened() const { return rel_; }
    bool has() const { return has_; }
    // The SVGs a character can be made of, by path; a new one beside it (one step): «Имя.pixel.json», «Имя.luau».
    const std::vector<std::string>& svgs() const { return svgs_; }
    bool create(const std::string& svg_rel);
    // «Новый»: a character the code draws (its draw section, forge/pixel/draw.h), «персонажи/Персонаж.pixel.json» and
    // «Персонаж.luau» in «Ресурсы» («Персонаж 2» … when taken), one step.
    bool create_drawn();
    // «Векторный персонаж» of «Ресурсы» on an SVG: the character made of it when there is one, else a new one.
    bool from_svg(const std::string& svg_rel);
    // Reads the files of the character again (after Ctrl+Z, or when they changed on disk).
    void reload();
    // «Ресурсы» looked at the project's files: the lists again, the SVG found where it is now.
    void indexed() { list_dirty_ = true; }
    // Looks at the dates of the SVG, the script and the «.pixel.json» now (each frame does it twice a second).
    void check_files();

    const pixel::PixelDoc& doc() const { return doc_; }
    // The picture the views show: the character's, or a scene's stage with its actors in it.
    const pixel::Rig& rig() const { return scene_mode_ && scene_ ? scene_->rig() : rig_; }
    pixel::Rig& rig() { return scene_mode_ && scene_ ? scene_->rig() : rig_; }
    // Whether the picture is the code's (draw), and the text of the SVG the rig was made of (the file's or the drawing's).
    bool drawn() const { return drawn_; }
    const std::string& picture_svg() const { return svg_text_; }
    // The other drawings of the code, as the rig has them (part 4д).
    const std::vector<pixel::NamedDrawing>& drawings() const { return others_; }
    // Why the character does not show or the pose fails: empty when all is well.
    const std::string& problem() const { return problem_; }
    u32 error_line() const { return error_line_; }
    // What the last action said (a refusal, what was done).
    const std::string& note() const { return note_; }
    // A refusal said under the fields, the fields shown as things are (a step of the history that could not be done).
    void refused(std::string why);

    // Parts: the selected one, and its pivot (one step; picture units).
    bool select_part(const std::string& id);
    const std::string& part() const { return part_; }
    bool set_pivot(const std::string& id, vector::Point at);
    // The mouse on the vector view, in the context's pixels: a press near the selected part's pivot drags it (one step
    // on release), elsewhere selects the part under it.
    void press(f32 x, f32 y);
    void drag(f32 x, f32 y);
    void release();
    // Where a point of the picture (in the pose shown) is on the screen, in the context's pixels; false when the view
    // is not laid out.
    bool screen_of(vector::Point p, f32& x, f32& y) const;
    // A part's pivot as it stands in the pose shown (picture units).
    vector::Point posed_pivot(const std::string& id) const;

    // The script's parameters (the declared ones, by name) and the seed.
    bool set_param(const std::string& name, f64 value);
    bool set_seed(u64 seed);

    // Clips: by state. A state without a clip gets one (4 frames, 10 a second, repeated) and is selected.
    bool select_clip(const std::string& state);
    const std::string& clip() const { return clip_; }
    bool add_clip(const std::string& state);
    bool remove_clip();
    bool set_clip_frames(int n);
    bool set_clip_fps(f32 fps);
    bool set_clip_loop(bool on);
    // The states a clip may be for: the module's, over the game's templates.
    const std::vector<level::LevelModule::AnimState>& states() const { return states_; }

    // The clock: playing or not, a frame back or forward (it stops), a frame chosen, the frames either side. Frames are
    // the whole clip's by its fps (frame k at k / fps; of the code without «Повторять» one more, the last at its end).
    void play(bool on);
    bool playing() const { return playing_; }
    void step(int frames);
    void set_frame(u32 k);
    u32 frame() const { return frame_; }
    void set_onion(bool on);
    bool onion() const { return onion_; }
    // The playhead: the moment of the clip shown, seconds (a frame's moment, or the range's when it plays).
    f64 time() const { return t_; }
    // Shift+← and Shift+→: the range's start and end (the first and the last frame of a clip of pose(t)).
    void to_start();
    void to_end();
    // The player: speed ×0.25 … ×2 (kSpeeds), the preview repeating (L; not «Повторять» of the clip), the frames a
    // second it shows in fact. None of them is written into files.
    static constexpr f32 kSpeeds[] = {0.25f, 0.5f, 1, 1.5f, 2};
    void set_speed(f32 speed);
    f32 speed() const { return speed_; }
    void set_loop_view(bool on);
    bool loop_view() const { return loop_view_; }
    f64 shown_fps() const { return shown_fps_; }

    // --- the clip on the timeline (part 3а) ---
    // The selected clip as it plays and bakes (its range), and all of it (the frames «кадр 3 из 8» counts).
    const pixel::ClipCut& cut() const { return cut_; }
    const pixel::ClipCut& whole() const { return whole_; }
    // The frames of each clip as baked now (by its range for the code), and their sum.
    u32 clip_frames(const std::string& state) const;
    u32 total_frames() const;
    // The range of the selected clip of the code: [from, to] (one step; all of the clip: "range" leaves the file). B and
    // N: its start or end at the playhead. The middle button on a clip's chip: all of that clip.
    bool set_range(f64 from, f64 to);
    bool range_start_here();
    bool range_end_here();
    bool whole_range(const std::string& state);
    // The recording on the timeline (the selected clip of the code; null for pose(t) alone) and whether it is the last
    // that worked while the code fails now (its blocks faint).
    const pixel::Recording* recording() const { return rec_ok_ ? &rec_ : nullptr; }
    bool stale() const { return rec_stale_; }
    // The timeline's view: its width in the window's pixels, its scale and start. F: the playhead in the middle.
    const pixel::TimeView& view() const { return tv_; }
    void zoom(f64 factor, f32 at_x);
    void pan(f32 dx);
    void center_playhead();
    // A time event's flag to moment `to` (by frames; with only_this the next event gives up as much): its lengths in
    // the file, one step.
    bool set_flag(usize event, f64 to, bool only_this);
    // A block of the recording: selected (the inspector, its call's lines lit), its call's text selected in the panel
    // (a double click). The blocks of the call the panel's caret is in.
    bool select_block(i32 index);
    i32 block() const { return block_; }
    bool block_to_code(i32 index);
    const std::vector<i32>& caret_blocks() const { return caret_blocks_; }
    // A time event's line in the panel (a double click on its flag); the playhead on its flag (the middle button).
    bool flag_to_code(usize event);
    void playhead_to_flag(usize event);
    // The line of the clip's start_from in the panel (a double click on the flag «из …» at 0, part 5б).
    bool start_to_code();
    // Where a moment of the clip is on the timeline, the window's pixels; false when it is not laid out.
    bool timeline_x(f64 t, f32& x) const;

    // --- mouse edits written into the code (part 3б, pixel_mouse.cpp) ---
    // Each is one step of the history that writes the script and the «.pixel.json» with the clips' frames counted anew,
    // its label saying what changed. Refused, note() saying why, when that number is not in the code (a binding, a
    // spring, a module's call…) or the code does not record; nothing written when nothing changes. Unsaved text in the
    // panel is saved first, a step of its own, and the edit goes by the new recording.
    // The selected block: its value (a number; #rrggbb, or nothing for its own colour, of fill; a shape's id, or
    // nothing, of morph), its length, its timing.
    bool set_block_value(const std::string& text);
    bool set_block_length(f64 length);
    bool set_block_timing(vector::Timing timing);
    // A block's end or start, a pause's end at moment `to` (the timeline's edges, by frames).
    bool set_block_end(i32 index, f64 to);
    bool set_block_start(i32 index, f64 to);
    bool set_pause_end(usize index, f64 to);
    // The selected part moved in the frame shown by dx, dy: the units its x and y move it by (the vector's drag, to
    // tenths, through what carries the part).
    bool move_part(f64 dx, f64 dy);
    // The selected part in the frame shown (the inspector): its properties as the view shows them, the block «К коду»
    // goes to (-1: none), and «К коду» itself: that block's call selected in the panel.
    struct PartValue {
        std::string name, value;
    };
    const std::vector<PartValue>& part_values() const { return part_values_; }
    i32 part_block() const { return part_block_; }
    bool part_to_code();
    // The part whose row of the tree the mouse is over, outlined in the vector; "" for none.
    const std::string& hovered_part() const { return hovered_; }
    // The timings' list of the inspector open.
    bool timings_open() const { return timings_open_; }

    // --- the part's fields and the curve (part 4ж, pixel_mouse.cpp) ---
    // The fields of the selected part: «Трансформация» (x … opacity), «Контур» (stroke, stroke_width), «Заливка»
    // (fill). Each is what the clip's code gives the property in the frame shown (its transition going: its end,
    // `going`), `now` the frame's own value when it shows another ("" when the same), `section` the heading over the
    // first of each. A field typed is written by the drag's rules for one property (pixel::edit_part_prop): one step,
    // the panel saved first; note() why not. part_values() keeps the frame's values of all the properties.
    struct PartField {
        std::string name, section, value, now;
        bool going = false;
    };
    const std::vector<PartField>& part_fields() const { return part_fields_; }
    bool set_part_field(const std::string& field, const std::string& text);
    // The list's «кривая»: the selected block's timing as bezier(0.25, 0.1, 0.25, 1); nothing when it is one already.
    bool set_block_bezier_timing();
    // The numbers of the selected block's bezier(…) (the graph's handle let go): one step.
    bool set_block_bezier(const vector::Bezier& curve);
    // The graph under «как»: the selected block's curve as shown (where a handle held is now); null when its timing is no
    // bezier(…). ease_screen: the graph's point (x of the time, y of the way) on the screen (the context's pixels), false
    // when no graph shows; ease_handle: handle 1 or 2 there, false when the graph shows none (a curve not to be edited).
    const vector::Bezier* ease_curve() const { return ease_has_ ? &ease_live_ : nullptr; }
    bool ease_screen(f64 x, f64 y, f32& sx, f32& sy) const;
    bool ease_handle(int handle, f32& x, f32& y) const;
    int ease_held() const { return ease_held_; }
    const std::string& ease_why() const { return m_ease_why_; }
    const std::string& ease_share() const { return m_ease_share_; }
    // The graph's picture as last drawn (RGBA, w × h) and where its point (x, y) falls in it; false before one is drawn.
    const std::vector<u8>& ease_pixels(u32& w, u32& h) const { return w = ease_w_, h = ease_h_, ease_px_; }
    bool ease_texel(f64 x, f64 y, i64& tx, i64& ty) const;

    // --- the view and the sounds (part 3в, pixel_view.cpp) ---
    // The vector's zoom and pan: step 0 … pixel::kZoomTop round a point of the screen (the context's pixels), = and −
    // round the view's middle, 0 fitted; the middle button or the hand pans by the mouse's move. A new character starts
    // fitted. The view's points are its frame's px of the interface (part 7а): when the frame changes its size, the step
    // stays and the point of the picture in the view's middle stays there, as far as the edges let it.
    const pixel::PictureView& picture_view() const { return zoom_; }
    void zoom_view(i32 step, f32 x, f32 y);
    void zoom_view_by(int steps);
    void fit_view();
    void pan_view(f32 dx, f32 dy);
    // The grid ('), the eyedropper (I), the preview's sound (M). None of them is written into files.
    void set_grid(bool on);
    bool grid() const { return grid_; }
    void set_picking(bool on);
    bool picking() const { return picking_; }
    void set_sound(bool on);
    bool sound() const { return sound_on_; }
    // The line under the views: the picture's point under the mouse ("12.5, -3"; "" off the vector) and, picking, the
    // colour under it ("#rrggbb", "пусто"; "" off both views). P copies the point; a click while picking the colour.
    const std::string& coords() const { return coords_; }
    const std::string& picked() const { return picked_; }
    bool copy_coords();
    // The rows of sounds on the timeline: one a name, in the order of their first marks, its file and its wave's
    // columns (the loudest of each point of the timeline); none found or it does not read: why, at the line of its first
    // call.
    struct SoundRow {
        std::string name, why;
        std::filesystem::path file;
        bool found = false;
        u32 line = 0;
        std::vector<f32> columns;
    };
    const std::vector<SoundRow>& sound_rows() const { return sound_rows_; }
    // What the preview sounded since it was last started: the range's frame and the sound's name, in order.
    struct Sounded {
        u32 frame = 0;
        std::string name;
    };
    const std::vector<Sounded>& sounded() const { return sounded_; }
    const audio::Mixer& mixer() const { return mixer_; }
    // The mode is not shown: the preview's sounds stop.
    void away();
    bool offscreen = false; // main: no sound device is opened

    // --- shapes drawn with the mouse (part 4е, pixel_shapes.cpp) ---
    // The tools of the row over the views: «Выбор» (V), «Прямоугольник» (R), «Эллипс» (O), «Линия» (Shift+L), «Перо»
    // (Shift+P). A rect, an ellipse or a line is dragged in the vector, the pen's points clicked; the shape is a new last
    // line of the function of the drawing the frame shows (one step, the panel saved first), the new part selected and
    // the tool «Выбор» again. Refused, note() saying why and nothing written, for a picture of an SVG file, a drawing
    // function not written in this file, code with an error, a frame in a swap, a shape of no size. Choosing a tool turns
    // the eyedropper off and finishes the pen as Enter does.
    // Part 7а: «Рука» (H) drags the view with the left button as the middle one does, in a scene too.
    enum class Tool : u8 { Select, Rect, Ellipse, Line, Pen, Hand };
    void set_tool(Tool t);
    Tool tool() const { return tool_; }
    // The pen's points so far (picture units). Enter finishes it open (a path), a click on its first point closes it (a
    // poly); Esc, another character or clip, the code read again drop it.
    const std::vector<vector::Point>& pen_points() const { return pen_; }
    bool finish_pen();
    void drop_pen();
    // The colour of new shapes, "#rrggbb" (#5968e8 at first; an eyedropper's click sets it too). Not written into files.
    bool set_shape_color(const std::string& text);
    const std::string& shape_color() const { return shape_color_; }
    // «Привязка»: the tools' points, a part dragged and a pivot dragged go by the grid's lines, its step at the zoom the
    // press was made at (snap_step() at the zoom now). Not written into files.
    void set_snapping(bool on);
    bool snapping() const { return snapping_; }
    f64 snap_step() const;
    // The «Фигура» section of the selected part: the fields of the call that drew it as it drew them (only those the
    // field can write), why the others are not shown, how many shapes the call drew (a loop: more than one). A field
    // typed: written into the call (one step, the panel saved first), checked as draw checks it; note() why not.
    struct ShapeField {
        std::string name, value;
    };
    const std::vector<ShapeField>& shape_fields() const { return shape_fields_; }
    const std::string& shape_why() const { return shape_why_; }
    bool set_shape_field(const std::string& field, const std::string& text);

    // --- the code panel and the console (part 3а) ---
    // The panel's text: the script as a text field shows it (pixel::panel_text). Not saved while it is not the file's.
    const std::string& code() const { return code_; }
    bool code_unsaved() const { return has_ && code_ != code_disk_; }
    // The panel's text as typed (its change event): a step of the typing history, letters typed on one after another
    // one step.
    void code_typed(const std::string& text);
    // Ctrl+S in the panel, «Сохранить»: the script written as the panel has it, one step of the tab's history (the
    // counted frames of the clips into the «.pixel.json» in the same step). False when there is nothing to save.
    bool save_code();
    // Ctrl+Z, Ctrl+Y in the panel: the typing since the last save.
    bool code_undo();
    bool code_redo();
    usize typing_steps() const { return typing_at_; }
    // The script changed on disk while the panel had unsaved text: «Взять с диска» takes the file's, «Оставить мой»
    // keeps the panel's (the next save writes it).
    bool code_conflict() const { return code_conflict_; }
    void take_disk_code();
    void keep_my_code();
    // What closing the game or the editor would lose; the panel saved (save_unsaved of main).
    std::vector<std::string> unsaved() const;
    bool save_all(std::string& error);
    struct ConsoleLine {
        std::string text;
        u32 line = 0; // of the character's script; 0 for none
        bool error = false, warn = false;
    };
    const std::vector<ConsoleLine>& console() const { return console_; }
    // Line `line` of the script: the panel's caret at its start, the panel taking the keyboard.
    bool goto_line(u32 line);
    // The panel's selection (characters), as it is now.
    bool code_selection(u32& from, u32& to) const;
    // The lines the gutter lights for the block selected (1-based), and the error's line.
    u32 lit_first() const { return lit_first_; }
    u32 lit_last() const { return lit_last_; }

    // «Другое зерно» (one step): pixel::other_seed of the file's.
    bool other_seed();
    // «Снимок кадра» (one step): the pixel frame at the playhead as it bakes, a PNG of «Ресурсы» beside the
    // «.pixel.json»: «Ёжик walk 5.png», «Ёжик walk 5 (2).png» when that is taken. Its path, "" when none was made.
    std::string snapshot();

    // Baking: frame size, the game's colours or the SVG's, the outline and its colour.
    bool set_frame_size(u32 w, u32 h);
    bool set_game_palette(bool on);
    bool set_outline(bool on);
    bool set_outline_color(const std::string& text);
    // «Предел кадров» of the game ("frame_limit" of game.json, docs/models/движение-модель.md): one step that writes
    // only that field of game.json and has the library read its templates by it. Refused, note() saying why, outside
    // 1..16384 or below what the game's templates, their animations or the open character's clips already take.
    u32 frame_limit() const;
    bool set_frame_limit(u32 n);
    // «Запечь» (one step). A picture changed since it was baked is baked over only with over_edits: without, false and
    // edited() is true.
    bool bake(bool over_edits = false);
    bool edited() const { return edited_; }
    // The baked picture in «Ресурсы» (its path), "" before the first bake or when it is gone.
    std::string picture() const;

    // «Отдать шаблону…»: the templates of the game by key, in the list's order; the picture, its frames and the baked
    // clips the template plays as its animations (one step).
    const std::vector<u64>& templates() const { return templates_; }
    void open_give(bool on);
    bool giving() const { return giving_; }
    bool give(u64 key);
    // «Отдать экрану» (part 5в of docs/models/движение-модель.md): «Имя.svg» and «Имя.motion.json» into the game's
    // pictures, one step, for a layer of a screen to play the clips of the code. Refused, note() saying why, outside an
    // open game, over a pair another character gave, when a clip fails or there is none; false with «Уже отдано так»
    // when the game has these bytes already.
    bool give_screen();
    // «Отдать игре» of a scene (part 6г1 of docs/models/сцены-модель.md): its folder game/scenes/<name>/ (name: its
    // code's file without .luau) as pixel::build_scene_gift makes it, one step; the folder's files it writes no more go
    // in that step. Refused, note() saying why, outside a scene or an open game, when the scene or a clip of an actor
    // does not record, a name makes no file, the files would part from the tab, or over the folder of a scene of another
    // file; false with «Уже отдано так» when the folder has these bytes already.
    bool give_game();

    // --- scenes (part 6а of docs/models/сцены-модель.md, pixel_scene.cpp) ---
    // The «.scene.json» of the project, by path in «Ресурсы»: open() opens one as it opens a character.
    const std::vector<std::string>& scenes() const { return scenes_; }
    bool in_scene() const { return scene_mode_; }
    // The scene open, as read and recorded; null outside a scene.
    pixel::Scene* scene() { return scene_mode_ ? scene_.get() : nullptr; }
    // «Новая сцена»: «сцены/Сцена.luau» and «Сцена.scene.json» in «Ресурсы» («Сцена 2» … when taken), one step.
    bool create_scene();
    // The actors' clips on the timeline (the recording's plays): one selected (-1: none), its start dragged by frames
    // into the code (one step, the at(…) or delay(…) it starts at, as a block's start), its call in the panel, its
    // actor's character opened.
    bool select_actor_clip(i32 index);
    i32 actor_clip() const { return play_; }
    // Part 6б: the transitions in and out on their row of the timeline (the recording's transitions): one chosen (-1:
    // none), its call in the panel. The camera's frame on the screen (its corners, the context's pixels, clockwise from
    // the top left), false when no scene shows. The small view «Через камеру» is pixel_frame(), the player's frame
    // through_size() big: the canvas's shape, its longer side the view's.
    bool select_transition(i32 index);
    i32 transition() const { return transition_; }
    bool transition_to_code(i32 index);
    bool camera_corners(f32 x[4], f32 y[4]) const;
    bool through_size(u32& w, u32& h) const;
    bool set_actor_clip_start(i32 index, f64 to);
    bool actor_clip_to_code(i32 index);
    // Part 6в: the music's row (a block a file plays, from the call that began it to another's or music(nil), its fade
    // and the calls of the same music on it) and the stops' (flags of wait_action and talk), each double-clicked to its
    // call. While the player plays, the music of its moment sounds from where it is in its file (M off, paused, the ruler
    // dragged, the tab away: silence); music_played() what it began since the player last started, music_now() what
    // the mixer plays now. «Ждать, как в игре» (hold, not written into files): the player stands at a stop, the
    // scene's time with it, the actors' clips and the music going on, until a key of the action waited for (its keys
    // as «Ввод» has them, game_actions; any key for wait_action()) or ▶ and Space (go_on); paused or round again, the
    // waits are forgotten and the frames are the recording's.
    bool music_to_code(i32 call);
    bool stop_to_code(i32 index);
    struct MusicPlayed {
        std::string name;
        f64 from = 0; // seconds into its file
        u32 call = 0;
    };
    const std::vector<MusicPlayed>& music_played() const { return music_played_; }
    struct MusicNow {
        std::string name;
        f32 volume = 0;
        u32 call = 0;
        audio::Voice voice; // in mixer()
    };
    std::vector<MusicNow> music_now() const;
    void set_hold(bool on);
    bool hold() const { return hold_on_; }
    i32 held_at() const { return held_at_; } // the stop the player stands at; -1: none
    const std::vector<f64>& held() const { return held_; }
    const std::string& held_words() const { return m_held_words_; }
    bool go_on();
    void toggle_play(); // ▶ and Space: on from a stop, else play or pause
    bool open_actor_character();

    // --- the look (step 7, part 7а «Раскладка», pixel_look.cpp) ---
    // The three views: «Дизайн» (the vector, «Пиксели», «Свойства»), «Код» (the code beside the vector), «Анимация» (the
    // timeline higher, «Анимация» on the right); Ctrl+1 … Ctrl+3. A view shows its right tab at first, and «Пиксели» and
    // the bottom panel's height as it last had them; the code's share of the middle is one for all. None of it is
    // written into files or the history; the first view of a run is «Код», a character or scene opens in the last.
    enum class Look : u8 { Design, Code, Animation };
    void set_look(Look l);
    Look look() const { return look_; }
    // The left panel's tab: «Слои» ("layers") or «Файлы» ("files"); nothing open shows «Файлы», a file opened from
    // them «Слои». The right's: «Свойства» ("props") or «Анимация» ("anim"), set by what is selected (a part:
    // «Свойства»; a block, a pause, an actor's clip, a transition: «Анимация») and by the view. The bottom's: "timeline",
    // "problems", "console". «Экспорт» in place of the right's tabs (Esc closes it after the steps Esc had before).
    void set_left(const std::string& tab);
    const std::string& left() const { return left_; }
    void set_side(const std::string& tab);
    const std::string& side() const { return side_; }
    void set_bottom(const std::string& tab);
    const std::string& bottom() const { return bottom_; }
    void set_export(bool open);
    bool export_open() const { return export_open_; }
    void fold_pixels(bool folded);
    bool pixels_folded() const { return folded_[static_cast<usize>(look_)]; }
    // «Проблемы»: the console's errors and warnings, in its order; a click puts the panel on its line, in «Код».
    const std::vector<ConsoleLine>& problems() const { return problems_; }
    bool problem_to_code(usize index);
    // The bottom panel's height dragged (px of the interface; 0: its share of the view), the code's share of the middle
    // (0: the view's own, 48 % or 50 % in a narrow window).
    f64 bottom_height() const { return tl_height_[static_cast<usize>(look_)]; }
    f64 code_share() const { return code_share_; }
    // The vector's frame: its points (px of the interface) and its texels (pixels of the window, the texture's size).
    f64 view_points_w() const { return view_pw_; }
    f64 view_points_h() const { return view_ph_; }
    u32 view_texels_w() const;
    u32 view_texels_h() const;

    // What the views show now (for the self-test): the vector, the pixel frame at its size, both as the UI names them.
    const assets::CookedTexture& vector_view() const { return vector_; }
    // The vector as its view shows it: the selected part's bounds and pivot over it, the outline of the tree's row
    // under the mouse.
    const assets::CookedTexture& vector_shown() const { return vector_shown_; }
    // Where a point of the picture is in those textures (their texels), at the view's zoom and pan.
    vector::Point texel_of(vector::Point p) const { return to_texels().apply(p); }
    const assets::CookedTexture& pixel_frame() const { return frame_px_; }
    // The wave of a row of sounds as its image shows it (white where it sounds).
    const assets::CookedTexture* wave_image(usize row) const { return row < waves_.size() ? &waves_[row] : nullptr; }
    const std::string& vector_image() const { return m_vector_; }
    const std::string& pixel_image() const { return m_pixel_; }

private:
    struct Row {
        Rml::String name, sub;
        bool selected = false;
    };
    struct SvgRow {
        Rml::String name, sub;
    };
    struct PartRow {
        Rml::String id, label;
        int depth = 0;
        bool shape = false, selected = false;
        Rml::String name; // as the tree shows it: its id, «Камера» for a scene's camera (part 6б)
        bool camera = false;
    };
    struct ParamRow {
        Rml::String name, value;
    };
    struct StateChip {
        Rml::String id, name, sub;
        bool on = false, selected = false;
    };
    struct FrameCell {
        int index = 0;
        bool shown = false;
        Rml::String style, label;
    };
    struct Tick {
        Rml::String style, label;
    };
    struct FlagRow {
        int index = 0;
        Rml::String style, trail, label, title;
    };
    struct MarkRow {
        int index = 0;
        Rml::String style, title;
        bool sound = false, span = false, missing = false;
    };
    struct PauseRow {
        int index = 0;
        Rml::String style, label, title;
    };
    struct TrackRow {
        Rml::String label, style;
    };
    struct BlockRow {
        int index = 0;
        Rml::String style, label, title;
        bool selected = false, lit = false, module = false;
    };
    struct LineRow {
        int n = 0;
        bool lit = false, error = false;
    };
    struct ConsoleRow {
        Rml::String text;
        int line = 0;
        bool error = false, warn = false;
    };
    struct SpeedChip {
        Rml::String label;
        bool selected = false;
    };
    struct FpsChip {
        Rml::String label;
        bool selected = false;
    };
    struct TemplateRow {
        Rml::String name, icon, note;
        bool draws = false;
    };
    struct Watched {
        std::filesystem::path file;
        i64 stamp = 0; // write time and size; 0: not there
    };
    struct TimingRow {
        Rml::String word, icon;
        bool selected = false;
    };
    struct PartValueRow {
        Rml::String name, value;
    };
    struct SoundView {
        Rml::String name, wave, style, title;
        bool missing = false;
    };
    struct SoundFound {
        pixel::SoundFile file;
        std::vector<i64> stamps; // of the files it may be, as they were when looked for
    };
    struct ShapeFieldRow {
        Rml::String name, value;
    };
    struct PartFieldRow {
        Rml::String name, label, section, value, now;
    };
    struct PlayRow {
        int index = 0;
        Rml::String style, label, title;
        bool selected = false, loop = false, stop = false;
        Rml::String blend; // part 6г5а: the width of its blend's wedge from its left edge; empty without one
    };
    struct MusicView {
        int index = 0; // the call that began it
        Rml::String style, fade, label, title;
        bool missing = false;
    };
    struct MusicMark {
        int index = 0; // the call
        Rml::String style, title;
    };
    struct StopRow {
        int index = 0;
        Rml::String style, label, title;
        bool missing = false, held = false;
    };

    // The code the tab works on: the character's, or the scene's (part 6а).
    pixel::PoseScript& script() { return scene_mode_ && scene_ ? scene_->script() : script_; }
    const pixel::PoseScript& script() const { return scene_mode_ && scene_ ? scene_->script() : script_; }
    // Part 6а (pixel_scene.cpp): a scene read again (its file, its code, what its stage names), the files it read
    // looked at, what it reads «Ресурсы» and the game's modules with.
    void reload_scene();
    bool scene_files_changed();
    pixel::SceneFiles scene_files();
    // A part of the frame shown, of the stage or of an actor (part 6а): the picture it is in, its index there and that
    // picture's units to the view's; rig null when there is none.
    struct ShownPart {
        pixel::Rig* rig = nullptr;
        i32 index = -1;
        pixel::Affine outer;
        bool swapping = false; // the picture it is in shows a swap: two drawings
    };
    ShownPart shown_part(const std::string& id);
    bool has_part(const std::string& id) const; // of the frame shown, an actor's too
    static bool is_scene_file(const std::string& rel); // «….scene.json»
    void bind_scene(Rml::DataModelConstructor& model);
    bool change_scene(const pixel::PixelDoc& after, std::string label); // change() of a scene: its «.scene.json»
    void rebuild_scene_list();
    // The part under a point of the view (to_view() of a picture point) in a scene: an actor's, the actor, the stage's.
    std::string scene_part_at(const vector::Point& v, const pixel::Affine& view);
    void scene_rows();         // the tree of a scene: its parts, an actor's inside it
    void lay_actor_clips();    // the actors' rows of clips on the timeline
    void rebuild_actor_clip(); // the selected clip's inspector
    // Part 6б: the camera chosen; whether a point of the screen is on its frame's edge; the frame over the vector; the
    // row of transitions and the chosen one's inspector.
    bool camera_chosen() const { return scene_mode_ && scene_ && part_ == pixel::kCameraPart; }
    bool camera_edge(f32 x, f32 y) const;
    void draw_camera_frame(assets::CookedTexture& t, const pixel::Affine& texels) const;
    void lay_transitions();
    void rebuild_transition();
    // Part 6в: the rows of music and stops; the stop's words, the action it waits for (null: none such, or any key)
    // and why its flag is marked; the mixer's music as the player's moment has it (silence when not going); a stop
    // reached while playing; the waits forgotten, the next stop the first from `from`; the line under the player; the
    // keys of the action waited for.
    void lay_music();
    void lay_stops();
    std::string stop_words(const pixel::SceneStop& st) const;
    const input::Action* stop_action(const pixel::SceneStop& st, const std::vector<input::Action>& actions) const;
    std::string stop_missing(const pixel::SceneStop& st, const std::vector<input::Action>& actions) const;
    void play_music(bool going);
    void hold_check();
    void forget_holds(f64 from);
    void rebuild_held();
    const std::vector<pixel::CallSite>& panel_sites() const; // the script's call sites; none while it is not loaded
    bool hold_key(const SDL_KeyboardEvent& k);
    void check_stops(); // the game's talks and actions looked at again: the flags marked anew when they changed
    void redraw_scene();
    std::string scene_snapshot();
    pixel::ClipSpec* selected_clip();
    const pixel::ClipSpec* selected_clip() const;
    // The file's new text (one step of the history).
    bool change(const pixel::PixelDoc& after, std::string label);
    // A character about to be made: open, empty, until the command writes its files.
    void open_new(const std::filesystem::path& doc_file);
    // The picture: the code's drawing when the script has draw, else the SVG of the file.
    void load_picture();
    void load_svg();
    // The other drawings of the code (part 4д) on the main picture: drawn again, put on a fresh rig when they changed.
    void load_drawings();
    void load_script();
    void rebuild_list();
    void rebuild_states();
    void rebuild();
    void sync_problem();
    void check_edited();
    void redraw();
    void show(const assets::CookedTexture& vec, const assets::CookedTexture& px);
    vector::Point view_to_picture(f32 x, f32 y) const;
    std::filesystem::path script_file() const;
    std::string name() const;
    // Part 3а (pixel_motion.cpp).
    void bind_motion(Rml::DataModelConstructor& model);
    const pixel::MotionLengths& lengths_now(const std::string& state) const; // the file's, or the flag's being dragged
    void give_lengths(); // every clip's to the script, lengths_now's (part 5б)
    bool pose_at(const pixel::ClipSpec& c, f64 t, u32 k, pixel::Pose& out, std::string& why);
    void recount();          // the cuts, the clips' frames, the recording of the selected clip
    void sync_time();        // frame_ of t_, the labels
    void place_head();       // the playhead's place on the timeline
    void lay_timeline();     // the timeline's rows from the recording and the view
    void rebuild_console();
    void forget_character(); // the player, the timeline and the panel as for a character just opened
    void rebuild_gutter();
    void adopt_code(const std::string& file_text); // the script read: the panel takes it, keeps its own or asks
    void set_panel(const std::string& text, i32 caret); // the panel's field to this text (no typing step)
    Rml::Element* element(const char* id) const;
    void watch_modules();
    bool modules_changed();
    void update_caret();
    void update_timeline_box();
    u32 frame_of(f64 t) const; // the frame of the whole clip at moment t
    void timeline_press(f32 x, f32 y, Rml::Element* over, bool middle);
    void timeline_drag(f32 x);
    void timeline_release();
    f64 snap(f64 t) const; // to the nearest frame of the clip
    // Part 3б (pixel_mouse.cpp).
    void bind_mouse(Rml::DataModelConstructor& model);
    // A mouse edit may begin: unsaved text in the panel saved first (saved: it was); false, note() saying why, when the
    // code does not record.
    bool begin_edit(bool& saved);
    bool code_records(); // the code records the clip; else false, note() saying why
    bool refuse_edit(std::string why);
    // The script's new text: one step, the «.pixel.json» with the clips' frames counted by it in the same step.
    bool write_script(const std::string& text, std::string label);
    // The selected block, found again (its track, its number on it) when the panel was saved first; null with a note.
    const pixel::Block* edited_block();
    void rebuild_inspector(); // the selected block's fields
    void rebuild_part();      // the selected part's values in the frame shown, from shown_pose_
    void select_call(const pixel::Block& b); // its call's text selected in the panel
    // A press on the timeline at a block's or a pause's edge takes it (true, also when refused); x, y in the context.
    bool edge_press(f32 x, f32 y, Rml::Element* over, bool again);
    f64 edge_snap(f64 t) const; // to a frame of the clip, not before 0, past its end too
    f64 block_end(i32 index) const;
    std::string part_at(f32 x, f32 y); // the smallest part under the mouse in the pose shown
    void start_move();
    void update_hover(Rml::Context* context);
    void make_curves();
    // Part 4ж (pixel_mouse.cpp).
    void rebuild_fields(bool has, const pixel::PartPose& pp); // the part's fields: the code's values, pp the frame's
    void rebuild_ease(const pixel::Block* b);      // the graph of block b (null: none), its texture when it changed
    void draw_ease();                              // the graph's texture as ease_live_ / the block's timing shows it (update())
    bool ease_box(f32& x, f32& y, f32& w, f32& h) const; // the graph's image on the screen
    bool ease_press(f32 x, f32 y);                 // a press on the graph: a handle near it held
    void ease_drag(f32 x, f32 y);
    void ease_release();
    void ease_cancel();                            // Esc while held: back where it was, nothing written
    // Part 7а (pixel_look.cpp): the look's bindings and events; the frames of the vector and «Пиксели» measured, the
    // vector resized to its frame; the grips of the bottom panel and the code's share dragged.
    void bind_look(Rml::DataModelConstructor& model);
    void measure_views(Rml::Context* context);
    void resize_view(f64 pw, f64 ph, u32 tw, u32 th);
    bool grip_press(Rml::Context* context, f32 x, f32 y);
    void grip_drag(f32 x, f32 y);
    void grip_release();
    void look_styles(); // px_tl_style, px_code_style
    void rebuild_problems();
    bool look_key(const SDL_KeyboardEvent& k); // Ctrl+1 … Ctrl+3
    // Part 3в (pixel_view.cpp).
    void bind_view(Rml::DataModelConstructor& model);
    pixel::Affine to_view() const;   // picture units to the view's points (its frame's px) at its zoom
    pixel::Affine to_texels() const; // … to the vector's texels (view_texels_w() × view_texels_h())
    void fit_box();                  // the picture's box at ×1, the zoom kept within it
    void track_mouse(Rml::Context* context); // the line under the views from the mouse
    bool pick_click(f32 x, f32 y);   // a click while picking: the colour copied
    void find_sounds();              // the files of the recording's sounds, by name
    bool sounds_changed();           // one of them appeared, changed or went on disk
    void lay_sounds();               // the rows of sounds from the view of the timeline
    void sound_frames(i64 now, bool wrapped); // the preview showed range frame `now`: the marks it passed sound
    void hush(); // another clip or character: the preview's sounds stop, what it sounded is forgotten
    std::string state_name(const std::string& id) const; // «Идёт» of "walk"; the id when no template plays it
    // Part 4е (pixel_shapes.cpp).
    void bind_shapes(Rml::DataModelConstructor& model);
    void shape_press(f32 x, f32 y); // a press in the vector with a tool: a shape begins, or the pen's point
    void shape_drag(f32 x, f32 y);
    void shape_release();
    std::string shown_drawing() const;           // "draw", or the name of the drawing the frame shows
    // Whether a shape can be written into drawing `target` ("draw" or a name) as the frame shows it now.
    bool can_draw(const std::string& target, std::string& why) const;
    bool write_shape(const pixel::NewShape& s);  // the call, one step; the part selected, «Выбор»
    // A part's shape as the code drew it in drawing `where` (shape_drawing: the one shown when it has the part), and how
    // many shapes its call drew; null with from_svg for a part of the SVG file's picture.
    std::string shape_drawing(const std::string& id) const;
    const pixel::DrawShape* shape_of(const std::string& id, const std::string& where, bool& from_svg, usize& count) const;
    void rebuild_shape();
    void draw_tool(assets::CookedTexture& t, const pixel::Affine& texels) const; // the shape being drawn, over the vector
    void code_error(std::string& why) const; // "Код с ошибкой: …" with its line
    void dirty(const char* var) {
        if (model_) model_.DirtyVariable(var);
    }

    AssetLibrary& assets_;
    level::LevelModule& module_;
    editor::UndoStack& history_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;

    std::vector<std::string> listed_, svgs_, scenes_;
    std::vector<level::LevelModule::AnimState> states_;
    std::vector<u64> templates_;
    bool list_dirty_ = true;
    u64 lib_seen_ = ~0ull; // the library's version the states and templates were made from
    f64 file_clock_ = 0;

    // A scene open in place of a character (part 6а): its stage's picture and code are rig() and script(); the files
    // it read and the modules (by their files) watched.
    bool scene_mode_ = false;
    std::unique_ptr<pixel::Scene> scene_;
    pixel::SceneDoc scene_doc_;
    std::vector<Watched> scene_watch_;
    std::vector<std::filesystem::path> scene_read_; // what the scene asked for while it was read, there or not
    std::vector<const pixel::Rig*> shown_rigs_;     // the drawings the tree of a scene was made of
    // The list's line of each scene not open, kept while its file and its code are as they were.
    struct SceneInfo {
        i64 doc = 0, code = 0;
        std::string sub;
    };
    std::map<std::string, SceneInfo> scene_info_;
    std::string scene_problem_;
    i32 play_ = -1;          // the actor's clip selected on the timeline
    std::vector<i32> plays_; // the plays shown, by row of m_plays_
    i32 transition_ = -1;    // the transition selected on the timeline (part 6б)
    // Part 6в: «Ждать, как в игре», the stop stood at, how long the player stood at each, the next it stops at; the
    // game's actions as asked when it stopped; the music voices of the mixer by the call that began them; the game's
    // talks and actions the stops' row was laid with; the music row's wave pictures now and before.
    bool hold_on_ = false;
    i32 held_at_ = -1;
    std::vector<f64> held_;
    usize next_stop_ = 0;
    std::vector<input::Action> hold_actions_;
    struct MusicOn {
        u32 call = 0;
        std::string name;
        audio::Voice voice;
        f32 volume = 0;
    };
    std::vector<MusicOn> music_on_;
    std::vector<MusicPlayed> music_played_;
    std::vector<std::string> talks_;
    std::string stops_key_, music_key_, music_images_[2];
    // The character open.
    std::string rel_;
    std::filesystem::path file_;
    bool has_ = false;
    pixel::PixelDoc doc_;
    pixel::Rig rig_;
    pixel::PoseScript script_;
    Watched svg_watch_, script_watch_, doc_watch_;
    std::string svg_text_, script_text_; // what the rig and the script were made of
    std::string svg_problem_, script_problem_, doc_problem_, pose_problem_, problem_, note_;
    u32 error_line_ = 0;
    // A picture the code draws: the drawing the rig was made of, why it is not drawn now (its line), a note that the
    // file's SVG is not used.
    bool drawn_ = false;
    pixel::Drawing drawing_;
    std::vector<pixel::NamedDrawing> others_; // drawings = { … } of the code, by name (part 4д)
    std::string others_key_;                  // their names and SVGs, as the rig has them
    const pixel::Rig* shown_rig_ = nullptr;   // the drawing the tree was made of: rebuilt when the frame shows another
    std::string draw_problem_, picture_note_;
    std::string refused_; // the note a mouse edit was refused with: it is about the clip, gone with it
    u32 draw_line_ = 0;
    std::string part_, clip_;
    bool edited_ = false, giving_ = false;

    // The clock and the views.
    bool playing_ = true, onion_ = false;
    u32 frame_ = 0;
    bool view_dirty_ = true;
    assets::CookedTexture vector_, frame_px_, vector_shown_;
    std::string images_[2][3]; // the pictures shown now and before: vector, pixel big, pixel 1:1
    u64 image_serial_ = 0;
    f32 view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0; // the vector view on screen (context pixels)
    // The views are drawn at their size in pixels of the window, dp_ of them a px of the interface. The vector's frame in
    // points and in texels (part 7а; 0 texels: not measured yet, its points at dp_); «Пиксели»'s frame in pixels of the
    // window (0: not measured yet).
    f32 dp_ = 1;
    f64 view_pw_ = kViewPoints, view_ph_ = kViewPoints;
    u32 view_tw_ = 0, view_th_ = 0;
    f32 pixel_box_w_ = 0, pixel_box_h_ = 0;
    // … and its column's room under its head (0: not measured yet): the frame «в настоящем размере» is shown under «как
    // запечётся» only while it fits there and leaves «как запечётся» as much room.
    f32 pixel_room_w_ = 0, pixel_room_h_ = 0;
    static constexpr f64 kViewPoints = 288; // the frame before it is first measured
    // Part 7а: the look, the panels' tabs, «Пиксели» folded and the bottom panel's height of each view, the code's
    // share, the grip held (0: none, 1: the bottom panel's, 2: the code's), where it was pressed and what it held then.
    Look look_ = Look::Code;
    std::string left_ = "files", side_ = "props", bottom_ = "timeline";
    bool export_open_ = false;
    bool folded_[3] = {false, true, false};
    f64 tl_height_[3] = {0, 0, 0};
    f64 tab_h_ = 0; // the height under the bar (px of the interface; 0: not measured yet)
    f64 code_share_ = 0;
    int grip_ = 0;
    bool grip_across_ = false; // the code's grip moves up and down (the code over the vector)
    f32 grip_at_ = 0, grip_from_ = 0, grip_whole_ = 0;
    std::vector<ConsoleLine> problems_;
    std::vector<ConsoleRow> m_problems_;
    Rml::String m_look_ = "code", m_left_ = "files", m_side_ = "props", m_bottom_ = "timeline", m_tl_style_, m_code_style_,
        m_vector_style_, m_pixel_style_, m_one_style_;
    bool m_export_open_ = false, m_pixels_folded_ = true;
    bool dragging_ = false, drag_moved_ = false;
    pixel::PixelDoc drag_doc_; // the file's, while a pivot is dragged
    // Part 3б: a part dragged in the vector (pressed at move_x_, move_y_; started past 3 points), its move so far.
    bool moving_ = false, move_started_ = false;
    f32 move_x_ = 0, move_y_ = 0;
    pixel::Affine move_inv_;
    vector::Point move_from_;
    f64 move_dx_ = 0, move_dy_ = 0;
    pixel::Pose shown_pose_; // the frame the vector shows, the drag's move in it
    bool shown_ok_ = false;
    std::string hovered_;
    std::vector<PartValue> part_values_;
    std::vector<PartField> part_fields_;
    // The part's field with the keyboard and the text it showed when it took it (what it writes nothing as); the field
    // whose text is the code's again though it has the keyboard (written, or refused).
    std::string field_focus_, field_shown_, field_fresh_;
    i32 part_block_ = -1;
    // Part 4ж: the graph of the selected block's easing: a bezier(…) shown (ease_has_), editable (ease_can_), its curve
    // as shown, the handle held (0: none) and the curve when it was pressed; the texture's names now and before.
    // ease_dirty_: drawn at the next update, once between two of the UI's (the picture before the last is dropped).
    bool ease_has_ = false, ease_can_ = false, ease_dirty_ = false;
    vector::Bezier ease_live_, ease_from_;
    int ease_held_ = 0;
    i32 ease_block_ = -1;
    f32 ease_grab_x_ = 0, ease_grab_y_ = 0; // the press's offset from the handle's middle, the context's pixels
    std::string ease_key_, ease_images_[2];
    std::vector<u8> ease_px_; // the picture as last drawn, ease_w_ × ease_h_
    u32 ease_w_ = 0, ease_h_ = 0;
    bool timings_open_ = false, curves_made_ = false;
    // Part 3в: the view, the line under it, the sounds.
    pixel::PictureView zoom_;
    bool grid_ = false, picking_ = false, sound_on_ = true, panning_ = false;
    f32 mouse_x_ = -1e9f, mouse_y_ = -1e9f, pan_x_ = 0, pan_y_ = 0;
    f32 pixel_x_ = 0, pixel_y_ = 0, pixel_w_ = 0, pixel_h_ = 0; // the pixel view on screen (context pixels)
    std::string coords_, picked_;
    std::map<std::string, SoundFound> sound_files_;
    std::vector<SoundRow> sound_rows_;
    std::vector<assets::CookedTexture> waves_;
    std::vector<std::string> wave_images_[2]; // the waves' pictures shown now and before
    std::string wave_key_;                    // what the waves were laid from: the same, they are not laid again
    std::vector<Sounded> sounded_;
    i64 sound_shown_ = -1; // the range's frame the playing preview showed last
    audio::Mixer mixer_;
    bool mixer_tried_ = false;
    // Part 4е: the tool, the shape dragged (its corners or ends, picture units), the pen's points, the colour.
    Tool tool_ = Tool::Select;
    bool tooling_ = false, snapping_ = false;
    vector::Point tool_from_, tool_to_;
    f64 snap_step_ = 0; // of the press going on: the grid's step with «Привязка», else 0
    std::vector<vector::Point> pen_;
    std::string shape_color_ = pixel::kShapeColor;
    std::vector<ShapeField> shape_fields_;
    std::string shape_why_;

    // Part 3а: the player.
    f64 t_ = 0;          // the playhead
    f64 play_clock_ = 0; // seconds into the range while it plays
    f32 speed_ = 1;
    bool loop_view_ = true;
    f64 shown_fps_ = 0, fps_clock_ = 0;
    u32 fps_frames_ = 0;
    pixel::ClipCut cut_, whole_;
    std::map<std::string, u32> clip_frames_;
    // The recording on the timeline, the last that worked.
    pixel::Recording rec_;
    bool rec_ok_ = false, rec_stale_ = false;
    std::string rec_clip_;
    std::string rec_error_;             // why the selected clip of the code does not record now
    u32 rec_error_line_ = 0;
    std::vector<std::string> warnings_; // the tab's: a range cut, a length of an event the code has not
    std::vector<Watched> modules_;      // animations/*.luau the recording required
    // The timeline.
    pixel::TimeView tv_;
    f32 tl_x_ = 0, tl_y_ = 0, tl_w_ = 0, tl_h_ = 0; // its time area on screen (context pixels)
    f32 tl_laid_w_ = -1;
    i32 block_ = -1;
    std::vector<i32> caret_blocks_;
    enum class Drag : u8 { None, Ruler, From, To, Flag, BlockEnd, BlockStart, PauseEnd, PlayStart };
    Drag tl_drag_ = Drag::None;
    i32 drag_block_ = -1;
    usize drag_pause_ = 0;
    f64 drag_edge_ = 0; // where the edge is dragged to
    usize drag_event_ = 0;
    bool drag_only_ = false;
    pixel::MotionLengths drag_lengths_, drag_base_lengths_;
    pixel::Recording drag_rec_;
    pixel::TimeView drag_view_; // the timeline as at the press of a flag: the clip's length changes under the mouse
    f64 drag_from_ = 0, drag_to_ = 0;
    bool shift_ = false;
    // The code panel.
    std::string code_, code_disk_; // the panel's text, and the file's as the panel shows it
    bool code_crlf_ = false, code_conflict_ = false;
    std::string script_panel_; // what script_ was loaded from, as the panel shows it
    struct Typing {
        std::string before, after;
        u32 at = 0, removed = 0, inserted = 0; // bytes of the change in `before`
    };
    std::vector<Typing> typing_;
    usize typing_at_ = 0;
    bool typing_open_ = false; // the last step takes the next letter typed after it
    u32 typing_end_ = 0;
    u32 lit_first_ = 0, lit_last_ = 0, caret_ = ~0u;
    std::vector<ConsoleLine> console_;
    Rml::Context* context_ = nullptr;

    std::vector<Row> m_list_, m_scenes_;
    std::vector<TrackRow> m_actor_rows_;
    std::vector<PlayRow> m_plays_, m_transitions_;
    std::vector<MusicView> m_music_;
    std::vector<MusicMark> m_music_marks_;
    std::vector<StopRow> m_stops_;
    Rml::String m_music_wave_, m_music_wave_style_ = "display: none;", m_held_words_, m_stops_height_ = "height: 22px;";
    bool m_hold_ = false;
    Rml::String m_play_info_, m_play_call_, m_play_why_, m_actor_rows_height_, m_transition_info_, m_transition_call_;
    bool m_scene_ = false, m_has_play_ = false, m_play_can_code_ = false, m_has_transition_ = false, m_transition_can_code_ = false;
    std::vector<SvgRow> m_svgs_;
    std::vector<PartRow> m_parts_;
    std::vector<ParamRow> m_params_;
    std::vector<StateChip> m_states_;
    std::vector<FrameCell> m_frames_;
    std::vector<FpsChip> m_fps_;
    std::vector<TemplateRow> m_templates_;
    std::vector<Tick> m_ticks_;
    std::vector<FlagRow> m_flags_;
    std::vector<MarkRow> m_marks_;
    std::vector<PauseRow> m_pauses_;
    std::vector<TrackRow> m_tracks_;
    std::vector<BlockRow> m_blocks_;
    std::vector<LineRow> m_lines_;
    std::vector<ConsoleRow> m_console_;
    std::vector<SpeedChip> m_speeds_;
    std::vector<TimingRow> m_timings_;
    std::vector<PartValueRow> m_part_values_;
    std::vector<SoundView> m_sounds_;
    std::vector<ShapeFieldRow> m_shape_fields_;
    std::vector<PartFieldRow> m_part_fields_;
    Rml::String m_ease_image_, m_ease_share_, m_ease_why_;
    bool m_has_ease_ = false;
    Rml::String m_tool_ = "select", m_shape_color_ = pixel::kShapeColor, m_shape_swatch_, m_shape_why_, m_shape_count_;
    bool m_snapping_ = false, m_has_shape_ = false;
    Rml::String m_zoom_words_, m_coords_, m_picked_, m_pick_swatch_;
    bool m_grid_ = false, m_picking_ = false, m_sound_on_ = true;
    Rml::String m_block_value_, m_block_length_, m_block_timing_, m_timing_icon_, m_block_why_, m_part_name_, m_part_code_;
    bool m_can_value_ = false, m_can_length_ = false, m_can_timing_ = false, m_timings_open_ = false, m_has_part_ = false,
         m_part_can_code_ = false;
    Rml::String m_time_, m_shown_fps_, m_head_, m_range_, m_range_from_, m_range_to_, m_more_blocks_, m_tracks_height_, m_code_name_,
        m_inspector_, m_inspector_call_, m_clip_cut_;
    Rml::String m_start_, m_start_style_, m_start_title_; // the flag «из idle» at 0 (part 5б); empty: none
    bool m_code_unsaved_ = false, m_code_conflict_ = false, m_has_range_ = false, m_loop_view_ = true, m_stale_ = false,
         m_has_rec_ = false, m_code_clip_ = false, m_has_block_ = false;
    Rml::String m_name_, m_vector_, m_pixel_, m_one_, m_problem_, m_note_, m_frame_label_, m_clip_name_, m_seed_, m_fw_, m_fh_,
        m_outline_color_, m_picture_, m_size_, m_total_, m_limit_;
    int m_clip_frames_ = 0;
    bool m_has_ = false, m_has_clip_ = false, m_playing_ = true, m_onion_ = false, m_game_palette_ = false, m_outline_ = false,
         m_clip_loop_ = true, m_edited_ = false, m_giving_ = false, m_can_give_ = false;
};

} // namespace forge::editor_app
