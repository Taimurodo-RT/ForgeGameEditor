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

#include "forge/editor/document.h"
#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/objects/library.h"
#include "forge/pixel/bake.h"
#include "forge/pixel/timeline.h"
#include "forge/ui/ui.h"

#include "object_library.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <map>
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
    const pixel::Rig& rig() const { return rig_; }
    // Whether the picture is the code's (draw), and the text of the SVG the rig was made of (the file's or the drawing's).
    bool drawn() const { return drawn_; }
    const std::string& picture_svg() const { return svg_text_; }
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

    // What the views show now (for the self-test): the vector, the pixel frame at its size, both as the UI names them.
    const assets::CookedTexture& vector_view() const { return vector_; }
    // The vector as its view shows it: the selected part's bounds and pivot over it, the outline of the tree's row
    // under the mouse.
    const assets::CookedTexture& vector_shown() const { return vector_shown_; }
    const assets::CookedTexture& pixel_frame() const { return frame_px_; }
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
        bool sound = false, span = false;
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

    pixel::ClipSpec* selected_clip();
    const pixel::ClipSpec* selected_clip() const;
    // The file's new text (one step of the history).
    bool change(const pixel::PixelDoc& after, std::string label);
    // A character about to be made: open, empty, until the command writes its files.
    void open_new(const std::filesystem::path& doc_file);
    // The picture: the code's drawing when the script has draw, else the SVG of the file.
    void load_picture();
    void load_svg();
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
    std::string state_name(const std::string& id) const; // «Идёт» of "walk"; the id when no template plays it
    void dirty(const char* var) {
        if (model_) model_.DirtyVariable(var);
    }

    AssetLibrary& assets_;
    level::LevelModule& module_;
    editor::UndoStack& history_;
    ui::Ui* ui_ = nullptr;
    Rml::DataModelHandle model_;

    std::vector<std::string> listed_, svgs_;
    std::vector<level::LevelModule::AnimState> states_;
    std::vector<u64> templates_;
    bool list_dirty_ = true;
    u64 lib_seen_ = ~0ull; // the library's version the states and templates were made from
    f64 file_clock_ = 0;

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
    // The views are drawn at their size in pixels of the window: 288 px of the style, dp_ pixels each.
    f32 dp_ = 1;
    u32 view_px() const;
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
    i32 part_block_ = -1;
    bool timings_open_ = false, curves_made_ = false;

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
    enum class Drag : u8 { None, Ruler, From, To, Flag, BlockEnd, BlockStart, PauseEnd };
    Drag tl_drag_ = Drag::None;
    i32 drag_block_ = -1;
    usize drag_pause_ = 0;
    f64 drag_edge_ = 0; // where the edge is dragged to
    usize drag_event_ = 0;
    bool drag_only_ = false;
    pixel::MotionLengths drag_lengths_, drag_base_lengths_;
    pixel::Recording drag_rec_;
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

    std::vector<Row> m_list_;
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
    Rml::String m_block_value_, m_block_length_, m_block_timing_, m_timing_icon_, m_block_why_, m_part_name_, m_part_code_;
    bool m_can_value_ = false, m_can_length_ = false, m_can_timing_ = false, m_timings_open_ = false, m_has_part_ = false,
         m_part_can_code_ = false;
    Rml::String m_time_, m_shown_fps_, m_head_, m_range_, m_range_from_, m_range_to_, m_more_blocks_, m_tracks_height_, m_code_name_,
        m_inspector_, m_inspector_call_, m_clip_cut_;
    bool m_code_unsaved_ = false, m_code_conflict_ = false, m_has_range_ = false, m_loop_view_ = true, m_stale_ = false,
         m_has_rec_ = false, m_code_clip_ = false, m_has_block_ = false;
    Rml::String m_name_, m_vector_, m_pixel_, m_one_, m_problem_, m_note_, m_frame_label_, m_clip_name_, m_seed_, m_fw_, m_fh_,
        m_outline_color_, m_picture_, m_size_, m_total_, m_limit_;
    int m_clip_frames_ = 0;
    bool m_has_ = false, m_has_clip_ = false, m_playing_ = true, m_onion_ = false, m_game_palette_ = false, m_outline_ = false,
         m_clip_loop_ = true, m_edited_ = false, m_giving_ = false, m_can_give_ = false;
};

} // namespace forge::editor_app
