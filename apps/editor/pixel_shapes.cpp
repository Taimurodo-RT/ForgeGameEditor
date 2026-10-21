// «Векторная», part 4е of step 4 of docs/models/движение-модель.md, «Фигуры мышью»: the tools of the row over the views
// draw shapes into the code of the drawing the frame shows, the selected part's «Фигура» writes the fields of the call
// that drew it, «Привязка» puts the points on the grid's lines (pixel_editor.h). The calls and their rewrites are
// forge/pixel/draw_edit.h; here they are taken from the mouse, the keys and the fields and written as steps of the
// history.

#include "pixel_editor.h"
#include "pixel_files.h"

#include "asset_library.h"

#include "forge/core/path.h"
#include "forge/vector/svg_shape.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace forge::editor_app {

using namespace pixel_files;

namespace {

constexpr f32 kPenClose = 4;     // how near the pen's first point a click closes it (view pixels)
constexpr int kEllipseSteps = 96; // the outline of an ellipse being dragged
constexpr const char* kShapeMidSwap = "посреди смены рисунка фигура не рисуется: выберите кадр до или после";
constexpr const char* kSceneShapes = "в сцене фигуры мышью не рисуются: их пишет stage в коде сцены";

std::string thousandths(f64 v) { return number_words(std::round(v * 1000) / 1000); }

std::string trimmed(std::string t) {
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    return t;
}

const char* tool_word(PixelEditor::Tool t) {
    switch (t) {
    case PixelEditor::Tool::Select: return "select";
    case PixelEditor::Tool::Rect: return "rect";
    case PixelEditor::Tool::Ellipse: return "ellipse";
    case PixelEditor::Tool::Line: return "line";
    case PixelEditor::Tool::Pen: return "pen";
    case PixelEditor::Tool::Hand: return "hand";
    }
    return "select";
}

// What a step's label calls a new shape: «Ларец»: прямоугольник rect_1».
const char* shape_word(pixel::NewShape::Kind k) {
    switch (k) {
    case pixel::NewShape::Kind::Rect: return "прямоугольник";
    case pixel::NewShape::Kind::Ellipse: return "эллипс";
    case pixel::NewShape::Kind::Line: return "линия";
    case pixel::NewShape::Kind::Path: return "контур";
    case pixel::NewShape::Kind::Poly: return "многоугольник";
    }
    return "фигура";
}

using Color = std::array<u8, 4>;

void put(assets::CookedTexture& t, i64 x, i64 y, const Color& c) {
    if (x < 0 || y < 0 || x >= t.width || y >= t.height) return;
    std::copy(c.begin(), c.end(), &t.rgba8[(static_cast<usize>(y) * t.width + static_cast<usize>(x)) * 4]);
}

// A line of the texture's texels from a to b, k texels thick (the view's px).
void segment(assets::CookedTexture& t, vector::Point a, vector::Point b, i64 k, const Color& c) {
    const f64 n = std::max({1.0, std::ceil(std::fabs(b.x - a.x)), std::ceil(std::fabs(b.y - a.y))});
    const f64 lo = -(static_cast<f64>(k) - 1) / 2;
    for (f64 i = 0; i <= n; ++i) {
        const f64 x = a.x + (b.x - a.x) * i / n, y = a.y + (b.y - a.y) * i / n;
        for (i64 dy = 0; dy < k; ++dy)
            for (i64 dx = 0; dx < k; ++dx) put(t, std::lround(x + lo + static_cast<f64>(dx)), std::lround(y + lo + static_cast<f64>(dy)), c);
    }
}

void disc(assets::CookedTexture& t, vector::Point p, f64 r, const Color& c) {
    const i64 ri = static_cast<i64>(std::ceil(r)), cx = std::lround(p.x), cy = std::lround(p.y);
    for (i64 dy = -ri; dy <= ri; ++dy)
        for (i64 dx = -ri; dx <= ri; ++dx)
            if (static_cast<f64>(dx * dx + dy * dy) <= r * r) put(t, cx + dx, cy + dy, c);
}

} // namespace

// --- the model -------------------------------------------------------------------------------------

void PixelEditor::bind_shapes(Rml::DataModelConstructor& model) {
    if (auto s = model.RegisterStruct<ShapeFieldRow>()) {
        s.RegisterMember("name", &ShapeFieldRow::name);
        s.RegisterMember("value", &ShapeFieldRow::value);
    }
    model.RegisterArray<std::vector<ShapeFieldRow>>();
    m_shape_swatch_ = "background-color: " + shape_color_ + ";";
    model.Bind("px_tool", &m_tool_);
    model.Bind("px_shape_color", &m_shape_color_);
    model.Bind("px_shape_swatch", &m_shape_swatch_);
    model.Bind("px_snapping", &m_snapping_);
    model.Bind("px_has_shape", &m_has_shape_);
    model.Bind("px_shape_fields", &m_shape_fields_);
    model.Bind("px_shape_why", &m_shape_why_);
    model.Bind("px_shape_count", &m_shape_count_);

    auto on = [&](const char* name, auto fn) {
        model.BindEventCallback(name, [fn](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList& args) { fn(ev, args); });
    };
    auto arg_str = [](const Rml::VariantList& a, usize i) { return i < a.size() ? a[i].Get<Rml::String>() : Rml::String(); };
    auto arg_enter = [](const Rml::VariantList& a, usize i) { return i < a.size() && a[i].Get<bool>(); };
    auto field = [](Rml::Event& ev) -> std::optional<std::string> {
        Rml::Element* e = ev.GetTargetElement();
        if (!e || e->GetTagName() != "input") return std::nullopt;
        return static_cast<Rml::ElementFormControl*>(e)->GetValue();
    };
    on("px_tool", [this, arg_str](Rml::Event&, const Rml::VariantList& a) {
        const std::string w = arg_str(a, 0);
        for (Tool t : {Tool::Select, Tool::Rect, Tool::Ellipse, Tool::Line, Tool::Pen, Tool::Hand})
            if (w == tool_word(t)) set_tool(t);
    });
    // A field left as it shows writes nothing (and saves no panel).
    auto color = [this](const std::string& text) {
        if (text != m_shape_color_) set_shape_color(text);
    };
    on("px_shape_color", [color, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 1)) color(arg_str(a, 0));
    });
    on("px_shape_color_commit", [color, field](Rml::Event& ev, const Rml::VariantList&) {
        if (auto text = field(ev)) color(*text);
    });
    on("px_snap_toggle", [this](Rml::Event&, const Rml::VariantList&) { set_snapping(!snapping_); });
    auto shape = [this](const std::string& name, const std::string& text) {
        for (const ShapeField& f : shape_fields_)
            if (f.name == name && f.value == text) return;
        set_shape_field(name, text);
    };
    on("px_shape_field", [shape, arg_str, arg_enter](Rml::Event&, const Rml::VariantList& a) {
        if (arg_enter(a, 2)) shape(arg_str(a, 0), arg_str(a, 1));
    });
    on("px_shape_field_commit", [shape, arg_str, field](Rml::Event& ev, const Rml::VariantList& a) {
        if (auto text = field(ev)) shape(arg_str(a, 0), *text);
    });
}

// --- the tools ---------------------------------------------------------------------------------------

void PixelEditor::set_tool(Tool t) {
    if (scene_mode_ && t != Tool::Select && t != Tool::Hand) { // part 6а: the stage's shapes are typed in its code
        refuse_edit(kSceneShapes);
        t = Tool::Select;
    }
    // Another tool finishes the pen as Enter does; too few points for a line are dropped.
    if (t != tool_ && !pen_.empty()) {
        if (pen_.size() >= 2) finish_pen();
        drop_pen();
    }
    if (t != Tool::Select && picking_) set_picking(false);
    tooling_ = false;
    tool_ = t;
    m_tool_ = tool_word(t);
    dirty("px_tool");
    view_dirty_ = true;
}

bool PixelEditor::finish_pen() {
    if (tool_ != Tool::Pen || pen_.empty()) return false;
    return write_shape({pixel::NewShape::Kind::Path, pen_, shape_color_}); // refused: the pen goes on
}

void PixelEditor::drop_pen() {
    if (pen_.empty()) return;
    pen_.clear();
    view_dirty_ = true;
}

bool PixelEditor::set_shape_color(const std::string& text) {
    const std::string t = trimmed(text);
    u8 rgb[3];
    if (t == "none" || !vector::parse_svg_color(t, rgb)) {
        m_shape_color_ = shape_color_;
        dirty("px_shape_color");
        return refuse_edit("цвет фигур «" + t + "» не читается: #rrggbb, rgb(r, g, b) или имя вроде red");
    }
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    shape_color_ = m_shape_color_ = hex;
    m_shape_swatch_ = "background-color: " + shape_color_ + ";";
    dirty("px_shape_color");
    dirty("px_shape_swatch");
    view_dirty_ = true;
    return true;
}

void PixelEditor::set_snapping(bool on) {
    snapping_ = m_snapping_ = on;
    dirty("px_snapping");
}

f64 PixelEditor::snap_step() const { return pixel::grid_step(to_view().a); }

std::string PixelEditor::shown_drawing() const {
    const pixel::Rig& shown = rig().shown();
    for (const std::string& n : rig().drawings())
        if (rig().drawing(n) == &shown) return n;
    return "draw";
}

void PixelEditor::code_error(std::string& why) const {
    const u32 line = !script_problem_.empty() ? error_line_ : draw_line_;
    why = "Код с ошибкой: фигура мышью не рисуется, пока он не рисует" +
          (line > 0 ? " (строка " + std::to_string(line) + ", см. консоль)" : std::string(" (см. консоль)"));
}

bool PixelEditor::can_draw(const std::string& target, std::string& why) const {
    why.clear();
    if (!has_ || !rig().loaded()) return why = "Нет рисунка: фигуре некуда лечь", false;
    if (scene_mode_) return why = kSceneShapes, false;
    if (target == "draw" && !drawn_) {
        std::string svg = "SVG-файла";
        if (const std::optional<Guid> id = Guid::parse(doc_.svg))
            if (const assets::AssetRecord* r = assets_.find_id(*id)) svg = "«" + path_to_utf8(utf8_path(r->path).filename()) + "»";
        return why = "рисунок из " + svg + ": фигуры мышью пишутся в draw кода — допишите draw", false;
    }
    if (rig().swapping()) return why = kShapeMidSwap, false;
    // The code's own refusals when it is the file's: unsaved text in the panel is saved first, and they wait for that.
    if (code_unsaved()) return true;
    if (!script_problem_.empty() || !draw_problem_.empty()) return code_error(why), false;
    std::string probe;
    return pixel::insert_drawing_line(script().code(), script_text_, target, "", probe, why);
}

bool PixelEditor::write_shape(const pixel::NewShape& s) {
    note_.clear();
    std::string call, why;
    // No size: nothing written, the panel's text not saved either.
    if (!pixel::shape_call(s, "x", call, why)) return refuse_edit(why);
    // The drawing the frame shows now: the panel saved may put a fresh rig on until the frame is posed again.
    const std::string target = shown_drawing();
    if (!can_draw(target, why)) return refuse_edit(why);
    if (code_unsaved()) save_code(); // a step of its own (3б)
    if (!can_draw(target, why)) return refuse_edit(why);
    // Its number: the least no id of any drawing has (the rig's other drawings are put on it from others_).
    std::vector<const pixel::Drawing*> drawings;
    if (drawn_) drawings.push_back(&drawing_);
    for (const pixel::NamedDrawing& n : others_) drawings.push_back(&n.drawing);
    const std::string id = pixel::new_shape_id(s.kind, pixel::taken_shape_ids(rig(), drawings));
    std::string out;
    if (!pixel::shape_call(s, id, call, why) || !pixel::insert_drawing_line(script().code(), script_text_, target, call, out, why))
        return refuse_edit(why);
    // The pen: the code read again drops its points.
    write_script(out, "«" + name() + "»: " + shape_word(s.kind) + " " + id);
    tool_ = Tool::Select;
    m_tool_ = tool_word(tool_);
    dirty("px_tool");
    select_part(id);
    set_side("props"); // part 7а: its «Фигура»
    view_dirty_ = true;
    return true;
}

// --- the mouse ---------------------------------------------------------------------------------------

void PixelEditor::shape_press(f32 x, f32 y) {
    std::string why;
    if (!can_draw(shown_drawing(), why)) {
        refuse_edit(why);
        return;
    }
    play(false);
    snap_step_ = snapping_ ? snap_step() : 0;
    const vector::Point p = pixel::tool_point(view_to_picture(x, y), snap_step_);
    if (tool_ == Tool::Pen) {
        f32 fx = 0, fy = 0;
        if (pen_.size() >= 3 && screen_of(pen_.front(), fx, fy) && std::hypot(fx - x, fy - y) <= kPenClose * std::max(1.0f, dp_)) {
            write_shape({pixel::NewShape::Kind::Poly, pen_, shape_color_});
            return;
        }
        pixel::add_pen_point(pen_, p);
        view_dirty_ = true;
        return;
    }
    tooling_ = true;
    tool_from_ = tool_to_ = p;
    view_dirty_ = true;
}

void PixelEditor::shape_drag(f32 x, f32 y) {
    if (!tooling_) return;
    const vector::Point p = pixel::tool_point(view_to_picture(x, y), snap_step_);
    if (p.x == tool_to_.x && p.y == tool_to_.y) return;
    tool_to_ = p;
    view_dirty_ = true;
}

void PixelEditor::shape_release() {
    if (!tooling_) return;
    tooling_ = false;
    view_dirty_ = true;
    const pixel::NewShape::Kind kind = tool_ == Tool::Ellipse ? pixel::NewShape::Kind::Ellipse
                                       : tool_ == Tool::Line  ? pixel::NewShape::Kind::Line
                                                              : pixel::NewShape::Kind::Rect;
    write_shape({kind, {tool_from_, tool_to_}, shape_color_});
}

void PixelEditor::draw_tool(assets::CookedTexture& t, const pixel::Affine& texels) const {
    if (tool_ == Tool::Select || tool_ == Tool::Hand || t.rgba8.empty()) return;
    u8 rgb[3] = {0x59, 0x68, 0xe8};
    vector::parse_svg_color(shape_color_, rgb);
    const Color c{rgb[0], rgb[1], rgb[2], 255}, white{255, 255, 255, 255};
    const i64 k = std::max<i64>(1, std::lround(dp_));
    auto seg = [&](vector::Point a, vector::Point b) { segment(t, texels.apply(a), texels.apply(b), k, c); };
    if (tooling_) {
        const vector::Point a = tool_from_, b = tool_to_;
        if (tool_ == Tool::Rect) {
            const vector::Point q[4] = {a, {b.x, a.y}, b, {a.x, b.y}};
            for (usize i = 0; i < 4; ++i) seg(q[i], q[(i + 1) % 4]);
        } else if (tool_ == Tool::Ellipse) {
            const f64 cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, rx = std::fabs(b.x - a.x) / 2, ry = std::fabs(b.y - a.y) / 2;
            constexpr f64 kTurn = 6.283185307179586;
            for (int i = 0; i < kEllipseSteps; ++i) {
                const f64 u = kTurn * i / kEllipseSteps, v = kTurn * (i + 1) / kEllipseSteps;
                seg({cx + rx * std::cos(u), cy + ry * std::sin(u)}, {cx + rx * std::cos(v), cy + ry * std::sin(v)});
            }
        } else if (tool_ == Tool::Line) {
            seg(a, b);
        }
    }
    if (tool_ == Tool::Pen && !pen_.empty()) {
        for (usize i = 1; i < pen_.size(); ++i) seg(pen_[i - 1], pen_[i]);
        // To the mouse: where its click would put the next point.
        if (view_w_ > 0 && mouse_x_ >= view_x_ && mouse_y_ >= view_y_ && mouse_x_ < view_x_ + view_w_ && mouse_y_ < view_y_ + view_h_)
            seg(pen_.back(), pixel::tool_point(view_to_picture(mouse_x_, mouse_y_), snapping_ ? snap_step() : 0));
        for (const vector::Point& p : pen_) {
            disc(t, texels.apply(p), 3.0 * static_cast<f64>(k), c);
            disc(t, texels.apply(p), 1.5 * static_cast<f64>(k), white);
        }
    }
}

// --- «Фигура» ----------------------------------------------------------------------------------------

std::string PixelEditor::shape_drawing(const std::string& id) const {
    // The drawing shown when it has the part, else the one that has it (part 4д).
    const std::string where = rig().shown().part(id) >= 0 ? shown_drawing() : rig().holder_name(id);
    return where.empty() ? "draw" : where;
}

const pixel::DrawShape* PixelEditor::shape_of(const std::string& id, const std::string& where, bool& from_svg, usize& count) const {
    from_svg = false;
    count = 0;
    const pixel::Drawing* d = nullptr;
    if (where == "draw") {
        if (!drawn_) return from_svg = true, nullptr;
        d = &drawing_;
    }
    for (const pixel::NamedDrawing& n : others_)
        if (where != "draw" && n.name == where) d = &n.drawing;
    if (!d) return nullptr;
    const auto it = std::find_if(d->shapes.begin(), d->shapes.end(), [&](const pixel::DrawShape& x) { return x.id == id; });
    if (it == d->shapes.end()) return nullptr;
    // The shapes its call drew, in every drawing: one number in the code is all of theirs.
    if (it->site < 0 || it->module) return count = 1, &*it;
    auto add = [&](const pixel::Drawing& x) {
        for (const pixel::DrawShape& s : x.shapes) count += s.site == it->site && !s.module;
    };
    if (drawn_) add(drawing_);
    for (const pixel::NamedDrawing& n : others_) add(n.drawing);
    return &*it;
}

void PixelEditor::rebuild_shape() {
    std::vector<ShapeField> fields;
    std::string why, count;
    const bool has = !scene_mode_ && has_ && !part_.empty() && rig().holder(part_).part(part_) >= 0;
    if (has) {
        bool from_svg = false;
        usize n = 0;
        const pixel::DrawShape* s = shape_of(part_, shape_drawing(part_), from_svg, n);
        if (from_svg) {
            why = "рисунок из файла: фигура правится в SVG";
        } else if (!script_problem_.empty() || !draw_problem_.empty()) {
            code_error(why); // the shapes are of the code that drew last
        } else if (!s) {
            why = "фигуры «" + part_ + "» нет в рисунке кода";
        } else {
            if (n > 1) count = "из цикла: " + count_words(static_cast<u32>(n), "фигура", "фигуры", "фигур");
            // Each reason once: the fields of a module's shape are refused alike.
            std::vector<std::string> whys;
            for (const std::string& f : pixel::shape_fields(s->kind)) {
                std::string w;
                if (!pixel::can_edit_shape(script().code(), *s, f, w)) {
                    if (std::find(whys.begin(), whys.end(), w) == whys.end()) whys.push_back(w);
                    continue;
                }
                f64 v = 0;
                fields.push_back({f, pixel::shape_field_is_color(f) ? pixel::shape_field_text(*s, f)
                                     : pixel::shape_field_number(*s, f, v) ? thousandths(v)
                                                                            : std::string()});
            }
            for (const std::string& w : whys) why += (why.empty() ? "" : "\n") + w;
        }
    }
    const bool same = fields.size() == shape_fields_.size() &&
                      std::equal(fields.begin(), fields.end(), shape_fields_.begin(), [](const ShapeField& a, const ShapeField& b) { return a.name == b.name && a.value == b.value; });
    if (!same) {
        shape_fields_ = std::move(fields);
        m_shape_fields_.clear();
        for (const ShapeField& f : shape_fields_) m_shape_fields_.push_back({f.name, f.value});
        dirty("px_shape_fields");
    }
    if (has != m_has_shape_ || why != shape_why_ || count != m_shape_count_) {
        m_has_shape_ = has;
        shape_why_ = m_shape_why_ = why;
        m_shape_count_ = count;
        for (const char* var : {"px_has_shape", "px_shape_why", "px_shape_count"}) dirty(var);
    }
}

bool PixelEditor::set_shape_field(const std::string& field, const std::string& text) {
    note_.clear();
    dirty("px_shape_fields"); // whatever comes of it, the fields show the shape as drawn
    if (!has_ || part_.empty()) return refuse_edit("Выберите часть");
    if (scene_mode_) return refuse_edit(kSceneShapes);
    const std::string id = part_, where = shape_drawing(id); // as the frame shows it now, before the panel is saved
    bool from_svg = false;
    usize n = 0;
    if (shape_of(id, where, from_svg, n) == nullptr && from_svg) return refuse_edit("рисунок из файла: фигура правится в SVG");
    // The panel first, a step of its own (3б): the shape as the saved code draws it.
    if (code_unsaved()) save_code();
    std::string why;
    if (!script_problem_.empty() || !draw_problem_.empty()) return code_error(why), refuse_edit(why);
    const pixel::DrawShape* s = shape_of(id, where, from_svg, n);
    if (from_svg) return refuse_edit("рисунок из файла: фигура правится в SVG");
    if (!s) return refuse_edit("фигуры «" + id + "» нет в рисунке кода");
    std::string out;
    if (!pixel::edit_shape_field(script().code(), script_text_, *s, field, text, out, why)) return refuse_edit("Фигура «" + id + "»: " + why);
    if (out == script_text_) return rebuild(), false; // the same
    std::string shown = trimmed(text);
    if (!pixel::shape_field_is_color(field)) {
        std::replace(shown.begin(), shown.end(), ',', '.');
        shown = thousandths(std::strtod(shown.c_str(), nullptr));
    }
    return write_script(out, "«" + name() + "»: " + id + " · " + field + " = " + shown);
}

} // namespace forge::editor_app
