#include "forge/pixel/rig.h"

#include "forge/assets/image_edit.h"
#include "forge/pixel/code_sites.h"
#include "forge/vector/morph.h"
#include "forge/vector/svg_shape.h"
#include "forge/vector/trig.h"

#include <thorvg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace forge::pixel {

Affine operator*(const Affine& m, const Affine& n) {
    return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
            m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

Affine inverse(const Affine& m) {
    const f64 det = m.a * m.d - m.b * m.c;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return {};
    Affine r{m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
    r.e = -(r.a * m.e + r.c * m.f);
    r.f = -(r.b * m.e + r.d * m.f);
    return r;
}

Affine translation(f64 x, f64 y) { return {1, 0, 0, 1, x, y}; }

Affine pose_matrix(const PartPose& pp, vector::Point pv, f64 snap) {
    f64 x = pp.x, y = pp.y;
    if (snap > 0) x = std::round(x / snap) * snap, y = std::round(y / snap) * snap;
    f64 s = 0, c = 1;
    vector::trig::sin_cos(pp.rot * vector::trig::kPi / 180.0, s, c);
    const f64 kx = pp.scale * pp.sx, ky = pp.scale * pp.sy;
    // T(x, y) · T(pivot) · R · S · Kx · Ky · T(−pivot), Kx and Ky SVG's skewX and skewY
    Affine rs{c * kx, s * kx, -s * ky, c * ky, 0, 0};
    if (pp.skew_x != 0 || pp.skew_y != 0) {
        auto tangent = [](f64 deg) {
            f64 ts = 0, tc = 1;
            vector::trig::sin_cos(std::clamp(deg, -kMaxSkew, kMaxSkew) * vector::trig::kPi / 180.0, ts, tc);
            return ts / tc;
        };
        rs = rs * Affine{1, 0, tangent(pp.skew_x), 1, 0, 0} * Affine{1, tangent(pp.skew_y), 0, 1, 0, 0};
    }
    return translation(x + pv.x, y + pv.y) * rs * translation(-pv.x, -pv.y);
}

namespace {

Affine from_tvg(const tvg::Matrix& m) { return {m.e11, m.e21, m.e12, m.e22, m.e13, m.e23}; }
tvg::Matrix to_tvg(const Affine& a) {
    return {static_cast<f32>(a.a), static_cast<f32>(a.c), static_cast<f32>(a.e),
            static_cast<f32>(a.b), static_cast<f32>(a.d), static_cast<f32>(a.f), 0, 0, 1};
}

u8 to_u8(f64 v) { return static_cast<u8>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

// ThorVG's engine, held while a rig lives (its init and term count).
struct Engine {
    Engine() { tvg::Initializer::init(0); }
    ~Engine() { tvg::Initializer::term(); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
};

// A shape's paint as ThorVG holds it: a colour or a gradient (an own copy) for the fill and for the stroke, the
// stroke's width and dash.
struct Look {
    u8 fr = 0, fg = 0, fb = 0, fa = 0;
    std::unique_ptr<tvg::Fill> fill_grad;
    u8 sr = 0, sg = 0, sb = 0, sa = 0;
    std::unique_ptr<tvg::Fill> stroke_grad;
    f32 stroke_width = 0;
    std::vector<f32> dash;
    f32 dash_offset = 0;
    u8 opacity = 255;

    void take(tvg::Shape* s) {
        fill_grad.reset(), stroke_grad.reset();
        if (const tvg::Fill* g = s->fill()) fill_grad.reset(g->duplicate());
        else s->fill(&fr, &fg, &fb, &fa);
        if (const tvg::Fill* g = s->strokeFill()) stroke_grad.reset(g->duplicate());
        else s->strokeFill(&sr, &sg, &sb, &sa);
        stroke_width = s->strokeWidth();
        const f32* pattern = nullptr;
        const u32 n = s->strokeDash(&pattern, &dash_offset);
        dash.assign(pattern, pattern + (pattern ? n : 0));
        opacity = s->opacity();
    }
    void put(tvg::Shape* s) const {
        if (fill_grad) s->fill(fill_grad->duplicate());
        else s->fill(fr, fg, fb, fa);
        if (stroke_grad) s->strokeFill(stroke_grad->duplicate());
        else s->strokeFill(sr, sg, sb, sa); // a stroke the pose gave a shape without one goes too
        s->strokeWidth(stroke_width);
        s->strokeDash(dash.empty() ? nullptr : dash.data(), static_cast<u32>(dash.size()), dash_offset);
        s->opacity(opacity);
    }
    bool has_fill() const { return fill_grad || fa > 0; }
    bool has_stroke() const { return stroke_width > 0 && (stroke_grad || sa > 0); }
    vector::Look vector_look() const {
        vector::Look l;
        l.fill = fill_grad ? vector::Paint{true, 0, 0, 0, 1} : vector::rgb8(fr, fg, fb, fa / 255.0);
        if (!has_fill()) l.fill.on = false;
        l.stroke = stroke_grad ? vector::Paint{true, 0, 0, 0, 1} : vector::rgb8(sr, sg, sb, sa / 255.0);
        if (!has_stroke()) l.stroke.on = false;
        l.stroke_width = stroke_width;
        l.opacity = opacity / 255.0;
        return l;
    }
};

struct Leaf {
    tvg::Shape* shape = nullptr;
    Look rest;
    std::vector<tvg::PathCommand> cmds;
    std::vector<tvg::Point> pts;
    bool visible = true;
    f32 own_width = 1; // its stroke width, as SVG has one for a shape without a stroke too (stroke-width, else 1)
    bool path_changed = false, look_changed = false, trim_changed = false;
    bool hidden = false; // a part the pose hid (its own paint, for the baker that shows shapes one by one)
    i32 part = -1;       // the innermost part it is in, -1 for none
    Affine rest_global;  // its path's points to picture units at rest
    bool morphed = false; // in a swap (part 4д): the pose morphed it, into `now` (its path's units)
    vector::Shape now;
};

// The effects a pose put on a part's scene (part 4б), in ThorVG's terms, kept so the baker can put them back.
struct Fx {
    f64 blur = 0; // sigma, 0 for none
    bool shadow = false;
    u8 r = 0, g = 0, b = 0, a = 0;
    f64 angle = 0, distance = 0, sigma = 0;
};

// How the effects of a part go on: all, the blur alone (the baker's passes of shapes), or with the shadow opaque (its
// pass of the shadow).
enum class FxMode : u8 { Full, BlurOnly, ShadowOpaque };

struct PartRec {
    tvg::Paint* paint = nullptr;
    tvg::Matrix rest_local{};
    u8 rest_opacity = 255;
    bool rest_visible = true;
    bool hidden = false;  // by the last pose
    Affine parent_global; // the rest matrix from the paint's parent to picture units
    Affine rest_global;   // and from the paint itself
    Affine carry;         // the last pose's link (part 4в): C over its groups, identity for none
    std::vector<i32> leaves; // the shapes it draws, in order
    i32 leaf = -1;           // when it is a shape itself
    i32 group = -1;          // its parent's scene in Impl::groups
    tvg::Scene* wrap = nullptr; // a shape's own scene around it, for effects (ThorVG puts them on scenes only)
    Fx fx;
    bool fx_on = false;  // the last pose put effects on its scene
    bool masked = false; // the last pose gave it a mask
};

// What stands for a part among its neighbours: its own scene round a shape, else its paint.
tvg::Paint* slot(const PartRec& r) { return r.wrap ? static_cast<tvg::Paint*>(r.wrap) : r.paint; }

// The scene a part's effects go on: a group's own, a shape's wrap; null for neither.
tvg::Scene* fx_scene(const PartRec& r) {
    if (r.wrap) return r.wrap;
    return r.paint->type() == tvg::Type::Scene ? static_cast<tvg::Scene*>(r.paint) : nullptr;
}

void put_fx(const PartRec& r, FxMode mode) {
    tvg::Scene* s = fx_scene(r);
    if (!s) return;
    s->add(tvg::SceneEffect::Clear);
    if (r.fx.blur > 0) s->add(tvg::SceneEffect::GaussianBlur, r.fx.blur, 0, 0, 100);
    if (r.fx.shadow && mode != FxMode::BlurOnly)
        s->add(tvg::SceneEffect::DropShadow, int(r.fx.r), int(r.fx.g), int(r.fx.b), mode == FxMode::ShadowOpaque ? 255 : int(r.fx.a),
               r.fx.angle, r.fx.distance, r.fx.sigma, 100);
}

// A tint, then a flash, on one colour (part 4б): each channel times the tint's, the red, green and blue `flash` of the
// way to its colour; each step rounded.
struct Tone {
    bool tint = false;
    f64 m[4] = {255, 255, 255, 255};
    f64 flash = 0;
    f64 f[3] = {255, 255, 255};

    void apply(u8& r, u8& g, u8& b, u8& a) const {
        u8* c[4] = {&r, &g, &b, &a};
        auto byte = [](f64 v) { return static_cast<u8>(std::clamp(std::lround(v), 0L, 255L)); };
        if (tint)
            for (int i = 0; i < 4; ++i) *c[i] = byte(*c[i] * m[i] / 255.0);
        if (flash > 0)
            for (int i = 0; i < 3; ++i) *c[i] = byte(*c[i] + (f[i] - *c[i]) * flash);
    }
};

// A scene with parts in it: its paints in the SVG's order, put back when a pose changed it.
struct GroupRec {
    tvg::Scene* scene = nullptr;
    std::vector<tvg::Paint*> rest;
};

// Moves a paint of a scene before `at` (to the top for null): out and in again, kept alive meanwhile.
void reinsert(tvg::Scene* scene, tvg::Paint* p, tvg::Paint* at) {
    p->ref();
    scene->remove(p);
    scene->add(p, at);
    p->unref(false);
}

struct Box {
    f64 x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    bool empty() const { return x0 > x1; }
    void add(vector::Point p) {
        x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    }
};

// Where a cubic goes along one axis: its ends and the turning points between them.
void cubic_extent(f64 p0, f64 p1, f64 p2, f64 p3, f64& lo, f64& hi) {
    lo = std::min(p0, p3), hi = std::max(p0, p3);
    const f64 a = -p0 + 3 * p1 - 3 * p2 + p3, b = 2 * (p0 - 2 * p1 + p2), c = p1 - p0;
    auto at = [&](f64 t) {
        if (!(t > 0 && t < 1)) return;
        const f64 u = 1 - t;
        const f64 v = u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
        lo = std::min(lo, v), hi = std::max(hi, v);
    };
    if (std::abs(a) < 1e-12) {
        if (std::abs(b) > 1e-12) at(-c / b);
        return;
    }
    const f64 disc = b * b - 4 * a * c;
    if (disc < 0) return;
    const f64 s = std::sqrt(disc);
    at((-b + s) / (2 * a));
    at((-b - s) / (2 * a));
}

// The bounds of a shape's path drawn through m.
void path_box(const tvg::Shape* shape, const Affine& m, Box& box) {
    const tvg::PathCommand* cmds = nullptr;
    const tvg::Point* pts = nullptr;
    u32 nc = 0, np = 0;
    if (shape->path(&cmds, &nc, &pts, &np) != tvg::Result::Success) return;
    u32 k = 0;
    vector::Point cur{};
    auto pt = [&](u32 i) { return m.apply({pts[i].x, pts[i].y}); };
    for (u32 i = 0; i < nc; ++i) {
        switch (cmds[i]) {
        case tvg::PathCommand::MoveTo:
        case tvg::PathCommand::LineTo:
            if (k >= np) return;
            cur = pt(k++);
            box.add(cur);
            break;
        case tvg::PathCommand::CubicTo: {
            if (k + 3 > np) return;
            const vector::Point c1 = pt(k), c2 = pt(k + 1), to = pt(k + 2);
            k += 3;
            f64 lo, hi;
            cubic_extent(cur.x, c1.x, c2.x, to.x, lo, hi);
            box.add({lo, to.y}), box.add({hi, to.y});
            cubic_extent(cur.y, c1.y, c2.y, to.y, lo, hi);
            box.add({to.x, lo}), box.add({to.x, hi});
            cur = to;
            break;
        }
        case tvg::PathCommand::Close: break;
        }
    }
}

// ThorVG's own matrix of an SVG transform attribute, as it reads one: from a group of a small SVG of its own. False
// for anything but the words and numbers of a transform.
bool tvg_transform(const std::string& text, tvg::Matrix& out) {
    if (text.empty()) return false;
    for (const char ch : text)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && !std::strchr(" ,.()+-\t\r\n", ch)) return false;
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"><g id=\"t\" transform=\"" + text +
                            "\"><rect width=\"1\" height=\"1\"/></g></svg>";
    tvg::Picture* pic = tvg::Picture::gen();
    pic->ref();
    bool ok = pic->load(svg.data(), static_cast<u32>(svg.size()), "svg", nullptr, true) == tvg::Result::Success;
    if (ok) {
        const tvg::Paint* g = pic->paint(tvg::Accessor::id("t"));
        ok = g != nullptr;
        if (ok) out = const_cast<tvg::Paint*>(g)->transform();
    }
    pic->unref();
    return ok;
}

// A cubic as a broken line: halved until its control points lie within tol of its chord.
void flatten_cubic(vector::Point p0, vector::Point p1, vector::Point p2, vector::Point p3, f64 tol, int depth,
                   std::vector<vector::Point>& out) {
    const f64 dx = p3.x - p0.x, dy = p3.y - p0.y, len = std::hypot(dx, dy);
    auto off = [&](vector::Point q) {
        return len < 1e-12 ? std::hypot(q.x - p0.x, q.y - p0.y) : std::abs((q.x - p0.x) * dy - (q.y - p0.y) * dx) / len;
    };
    if (depth >= 16 || std::max(off(p1), off(p2)) <= tol) {
        out.push_back(p3);
        return;
    }
    auto mid = [](vector::Point a, vector::Point b) { return vector::Point{(a.x + b.x) / 2, (a.y + b.y) / 2}; };
    const vector::Point a = mid(p0, p1), b = mid(p1, p2), c = mid(p2, p3), ab = mid(a, b), bc = mid(b, c), m = mid(ab, bc);
    flatten_cubic(p0, a, ab, m, tol, depth + 1, out);
    flatten_cubic(m, bc, c, p3, tol, depth + 1, out);
}

bool drawable_tag(const std::string& t) {
    return t == "g" || t == "use" || t == "path" || t == "rect" || t == "circle" || t == "ellipse" || t == "line" || t == "polyline" ||
           t == "polygon";
}

// The viewBox attribute of the root: four numbers, the last two above 0.
bool view_box(const SvgOutline& svg, f64 out[4]) {
    if (svg.elements.empty()) return false;
    const std::string* v = svg.elements[0].attr("viewBox");
    if (!v) return false;
    std::string s = *v;
    std::replace(s.begin(), s.end(), ',', ' ');
    const char* p = s.c_str();
    for (int i = 0; i < 4; ++i) {
        char* end = nullptr;
        out[i] = std::strtod(p, &end);
        if (end == p || !std::isfinite(out[i])) return false;
        p = end;
    }
    return out[2] > 0 && out[3] > 0;
}

// The paint ThorVG leaves off a shape with display="none" (it draws nothing of it), for the copy of it that is shown
// (part 4г): its colours and their opacities, the width, ends, corners and dash of its stroke, its fill rule and its
// opacity, as the SVG gives them. A gradient is not read: a fill or a stroke of one stays off.
void put_svg_paint(tvg::Shape* s, const SvgOutline& svg, i32 e) {
    vector::Look base;
    base.fill.on = false;
    const vector::Look l = vector::look_from_svg(svg, e, base);
    s->fill(to_u8(l.fill.r), to_u8(l.fill.g), to_u8(l.fill.b), l.fill.on ? to_u8(l.fill.opacity) : 0);
    s->strokeFill(to_u8(l.stroke.r), to_u8(l.stroke.g), to_u8(l.stroke.b), l.stroke.on ? to_u8(l.stroke.opacity) : 0);
    s->strokeWidth(l.stroke.on ? static_cast<f32>(l.stroke_width) : 0);
    s->opacity(to_u8(l.opacity));
    const std::string cap = vector::inherited(svg, e, "stroke-linecap"), join = vector::inherited(svg, e, "stroke-linejoin");
    s->strokeCap(cap == "round" ? tvg::StrokeCap::Round : cap == "square" ? tvg::StrokeCap::Square : tvg::StrokeCap::Butt);
    s->strokeJoin(join == "round" ? tvg::StrokeJoin::Round : join == "bevel" ? tvg::StrokeJoin::Bevel : tvg::StrokeJoin::Miter);
    s->strokeMiterlimit(static_cast<f32>(std::max(1.0, vector::svg_number(vector::inherited(svg, e, "stroke-miterlimit"), 4.0))));
    s->fillRule(vector::inherited(svg, e, "fill-rule") == "evenodd" ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero);
    std::string dash = vector::inherited(svg, e, "stroke-dasharray");
    std::replace(dash.begin(), dash.end(), ',', ' ');
    std::vector<f32> pattern;
    for (const char* q = dash.c_str();;) {
        char* end = nullptr;
        const f64 v = std::strtod(q, &end);
        if (end == q || !std::isfinite(v) || v < 0) break;
        pattern.push_back(static_cast<f32>(v));
        q = end;
    }
    if (pattern.size() % 2) pattern.insert(pattern.end(), pattern.begin(), pattern.end()); // SVG repeats an odd list
    const bool any = std::any_of(pattern.begin(), pattern.end(), [](f32 v) { return v > 0; });
    if (any) s->strokeDash(pattern.data(), static_cast<u32>(pattern.size()),
                           static_cast<f32>(vector::svg_number(vector::inherited(svg, e, "stroke-dashoffset"), 0)));
}

// A path through a matrix (part 4д: a shape of another drawing in the units of this one's).
vector::Path transformed(const vector::Path& p, const Affine& m) {
    vector::Path out = p;
    for (vector::Contour& c : out.contours) {
        c.start = m.apply(c.start);
        for (vector::Cubic& q : c.curves) q.c1 = m.apply(q.c1), q.c2 = m.apply(q.c2), q.to = m.apply(q.to);
    }
    return out;
}

// A shape of another drawing in this one's units (part 4д): its path through the matrix, its stroke as wide as it
// looks there.
vector::Shape in_units(vector::Shape shape, const Affine& m) {
    shape.path = transformed(shape.path, m);
    shape.look.stroke_width *= std::sqrt(std::abs(m.a * m.d - m.b * m.c));
    return shape;
}

// A morphed shape put on a leaf's paint: its path, and its look but for a gradient it keeps.
void put_shape(Leaf& leaf, const vector::Shape& shape) {
    tvg::Shape* s = leaf.shape;
    s->reset();
    for (const vector::Contour& c : shape.path.contours) {
        s->moveTo(static_cast<f32>(c.start.x), static_cast<f32>(c.start.y));
        for (const vector::Cubic& q : c.curves)
            s->cubicTo(static_cast<f32>(q.c1.x), static_cast<f32>(q.c1.y), static_cast<f32>(q.c2.x), static_cast<f32>(q.c2.y),
                       static_cast<f32>(q.to.x), static_cast<f32>(q.to.y));
        if (c.closed) s->close();
    }
    const vector::Look& look = shape.look;
    if (!leaf.rest.fill_grad)
        s->fill(to_u8(look.fill.r), to_u8(look.fill.g), to_u8(look.fill.b), look.fill.on ? to_u8(look.fill.opacity) : 0);
    if (!leaf.rest.stroke_grad)
        s->strokeFill(to_u8(look.stroke.r), to_u8(look.stroke.g), to_u8(look.stroke.b), look.stroke.on ? to_u8(look.stroke.opacity) : 0);
    s->strokeWidth(static_cast<f32>(look.stroke.on ? look.stroke_width : 0));
    s->opacity(to_u8(look.opacity));
    leaf.path_changed = leaf.look_changed = true;
}

// Straight RGBA b over a, pixel by pixel (a swap's frame, part 4д: the new drawing over the old).
void over(std::vector<u8>& a, const std::vector<u8>& b) {
    for (usize i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
        const u32 sa = b[i + 3];
        if (sa == 0) continue;
        if (sa == 255) {
            std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(i), 4, a.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const u32 da = a[i + 3];
        const u32 oa = sa * 255 + da * (255 - sa); // alpha times 255²
        for (int c = 0; c < 3; ++c)
            a[i + c] = static_cast<u8>((b[i + c] * sa * 255 + a[i + c] * da * (255 - sa) + oa / 2) / oa);
        a[i + 3] = static_cast<u8>((oa + 127) / 255);
    }
}

} // namespace

// Which picture of a swap a pose is of (part 4д): the old one morphs its pairs into the new one's shapes and fades
// the rest by 1 − k; the new one hides its pairs and brings the rest in by k.
struct Rig::SwapSide {
    bool from = false;
    const Rig* other = nullptr;
    std::string other_name;
    f64 k = 0;
};

struct Rig::Impl {
    Engine engine; // first: the last to go
    SvgOutline outline;
    tvg::Picture* pic = nullptr;
    tvg::Paint* doc = nullptr; // the scene of the SVG's own coordinates (picture units)
    Affine doc_matrix;         // doc's own matrix: picture units to ThorVG's viewport
    f64 vb[4] = {0, 0, 0, 0};  // the picture's area in its units: x, y, width, height
    std::vector<Leaf> leaves;
    std::unordered_map<const tvg::Paint*, i32> leaf_of;
    std::vector<Part> parts; // the picture's first, then the copies poses made (part 4г)
    usize picture = 0;       // how many are the picture's
    std::vector<i32> source; // by part: the part a copy is of; -1 for the picture's
    std::vector<u32> number; // by part: a copy's number
    std::vector<char> drawn; // by part: a copy the last pose drew
    std::vector<PartRec> recs;
    std::unordered_map<const tvg::Paint*, i32> part_of;
    std::vector<vector::Point> pivots;
    std::vector<vector::Point> moves; // each part's own move in the last pose, as applied (snapped)
    std::vector<GroupRec> groups;
    bool reordered = false; // the last pose changed the order of some group
    std::map<std::string, vector::Morph> morphs;
    vector::Shape scratch;

    tvg::SwCanvas* canvas = nullptr;
    std::vector<u32> buffer;
    u32 cw = 0, ch = 0;

    // Part 4д: the other drawings, by name, and what the last pose showed of them ("" the main picture).
    std::map<std::string, Rig> drawings;
    std::string shown;
    bool swap = false;
    std::string swap_from;

    ~Impl() {
        if (canvas) {
            canvas->remove(pic);
            delete canvas;
        }
        if (pic) pic->unref();
    }

    // Any part by id, the copies too; -1 for none.
    i32 find(std::string_view id) const {
        for (usize i = 0; i < parts.size(); ++i)
            if (parts[i].id == id) return static_cast<i32>(i);
        return -1;
    }

    i32 add_copy(const std::string& id, i32 src, u32 n);

    // The matrix from a paint to picture units as the paints stand now.
    Affine global(const tvg::Paint* p) const {
        Affine m;
        for (; p && p != doc; p = p->parent()) m = from_tvg(const_cast<tvg::Paint*>(p)->transform()) * m;
        return m;
    }

    void target(u32 w, u32 h) {
        if (!canvas) {
            canvas = tvg::SwCanvas::gen();
            canvas->add(pic);
        }
        if (w != cw || h != ch) {
            buffer.assign(static_cast<usize>(w) * h, 0);
            canvas->target(buffer.data(), w, w, h, tvg::ColorSpace::ABGR8888S);
            cw = w, ch = h;
        }
    }

    // Draws the picture as it stands into the buffer (straight RGBA bytes).
    void render(const Affine& to_pixels) {
        pic->transform(to_tvg(to_pixels * inverse(doc_matrix)));
        canvas->update();
        canvas->draw(true);
        canvas->sync();
    }

    const u8* bytes() const { return reinterpret_cast<const u8*>(buffer.data()); }
};

// A copy of part src from its rest (part 4г): its paint duplicated, with a scene of its own round a shape as every
// shape part has, put in src's group above src and above src's copies with smaller numbers. Its shapes and the part
// itself come after all others; it turns round src's pivot.
i32 Rig::Impl::add_copy(const std::string& id, i32 src, u32 n) {
    const PartRec& sr = recs[static_cast<usize>(src)];
    tvg::Paint* dup = sr.paint->duplicate(); // seen: made after the rest, its part's paint is too
    const i32 element = parts[static_cast<usize>(src)].element;
    if (sr.leaf >= 0 && vector::display_none(outline, element)) put_svg_paint(static_cast<tvg::Shape*>(dup), outline, element);
    tvg::Scene* wrap = nullptr;
    if (sr.wrap) {
        wrap = tvg::Scene::gen();
        wrap->add(dup);
    }
    tvg::Paint* own = wrap ? static_cast<tvg::Paint*>(wrap) : dup;
    GroupRec& g = groups[static_cast<usize>(sr.group)];
    const std::list<tvg::Paint*>& list = g.scene->paints();
    auto it = std::find(list.begin(), list.end(), slot(sr));
    if (it != list.end()) ++it;
    for (; it != list.end(); ++it) {
        bool lower = false;
        for (usize c = picture; c < recs.size(); ++c)
            if (slot(recs[c]) == *it) lower = source[c] == src && number[c] < n;
        if (!lower) break;
    }
    g.scene->add(own, it == list.end() ? nullptr : *it);
    g.rest.assign(list.begin(), list.end());

    const i32 index = static_cast<i32>(parts.size());
    Part part = parts[static_cast<usize>(src)];
    part.id = id;
    PartRec r;
    r.paint = dup;
    r.rest_local = sr.rest_local;
    r.rest_opacity = dup->opacity(); // as made: a hidden shape's copy has the opacity the SVG gives it
    r.rest_visible = true;
    r.parent_global = sr.parent_global;
    r.rest_global = sr.rest_global;
    r.group = sr.group;
    r.wrap = wrap;
    // Its shapes, in the order drawn, each as the source's shape in the same place.
    std::vector<tvg::Paint*> all;
    {
        tvg::Accessor* acc = tvg::Accessor::gen();
        acc->set(dup, [](const tvg::Paint* p, void* data) {
            static_cast<std::vector<tvg::Paint*>*>(data)->push_back(const_cast<tvg::Paint*>(p));
            return true;
        }, &all);
        delete acc;
    }
    const std::vector<i32> theirs = sr.leaves;
    usize k = 0;
    for (tvg::Paint* p : all) {
        if (p->type() != tvg::Type::Shape || k >= theirs.size()) continue;
        const Leaf& like = leaves[static_cast<usize>(theirs[k++])];
        auto* s = static_cast<tvg::Shape*>(p);
        Leaf l;
        l.shape = s;
        l.rest.take(s);
        l.cmds = like.cmds;
        l.pts = like.pts;
        l.visible = s->visible();
        l.own_width = like.own_width;
        l.part = index;
        l.rest_global = like.rest_global;
        const i32 li = static_cast<i32>(leaves.size());
        leaf_of[s] = li;
        r.leaves.push_back(li);
        if (p == dup) r.leaf = li;
        leaves.push_back(std::move(l));
    }
    part_of[dup] = index;
    const vector::Point pv = pivots[static_cast<usize>(src)];
    parts.push_back(std::move(part));
    recs.push_back(std::move(r));
    pivots.push_back(pv);
    moves.push_back({});
    source.push_back(src);
    number.push_back(n);
    drawn.push_back(0);
    return index;
}

Rig::Rig() : impl_(std::make_unique<Impl>()) {}
Rig::~Rig() = default;
Rig::Rig(Rig&&) noexcept = default;
Rig& Rig::operator=(Rig&&) noexcept = default;

bool Rig::load(std::string_view svg, std::string& why) {
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.outline = scan_svg(svg);
    if (!m.outline.error.empty()) {
        why = m.outline.error;
        return false;
    }
    if (m.outline.elements.empty() || m.outline.elements[0].tag != "svg") {
        why = "это не SVG: нет корневого <svg>";
        return false;
    }
    m.pic = tvg::Picture::gen();
    m.pic->ref();
    if (m.pic->load(svg.data(), static_cast<u32>(svg.size()), "svg", nullptr, true) != tvg::Result::Success) {
        why = "ThorVG не открыл этот SVG";
        impl_ = std::make_unique<Impl>();
        return false;
    }

    // Every paint, parents first (the order ThorVG draws them): the picture, its scene, the viewport's clip, the SVG.
    std::vector<tvg::Paint*> all;
    {
        tvg::Accessor* acc = tvg::Accessor::gen();
        acc->set(m.pic, [](const tvg::Paint* p, void* data) {
            static_cast<std::vector<tvg::Paint*>*>(data)->push_back(const_cast<tvg::Paint*>(p));
            return true;
        }, &all);
        delete acc;
    }
    if (all.size() < 4 || all[0] != m.pic || all[1]->parent() != m.pic || all[2]->parent() != all[1] || all[3]->parent() != all[2]) {
        why = "в SVG нечего рисовать";
        impl_ = std::make_unique<Impl>();
        return false;
    }
    m.doc = all[3];
    m.doc_matrix = from_tvg(m.doc->transform());
    f32 pw = 0, ph = 0;
    m.pic->size(&pw, &ph);
    if (!view_box(m.outline, m.vb)) {
        const Affine back = inverse(m.doc_matrix);
        const vector::Point a = back.apply({0, 0}), b = back.apply({pw, ph});
        m.vb[0] = std::min(a.x, b.x), m.vb[1] = std::min(a.y, b.y);
        m.vb[2] = std::abs(b.x - a.x), m.vb[3] = std::abs(b.y - a.y);
    }

    // A shape with display="none" comes from ThorVG without its own transform (part 4в of the motion model): a path or
    // a mask is often hidden, so the transform of its SVG element is put on it here, as ThorVG reads one.
    {
        std::unordered_map<u32, tvg::Paint*> first;
        for (usize i = 4; i < all.size(); ++i)
            if (all[i]->id) first.emplace(all[i]->id, all[i]);
        for (usize i = 1; i < m.outline.elements.size(); ++i) {
            const SvgElement& e = m.outline.elements[i];
            const std::string* tr = e.attr("transform");
            if (e.id.empty() || !tr || e.tag == "g" || e.tag == "use" || !drawable_tag(e.tag) || e.property("display") != "none") continue;
            auto it = first.find(tvg::Accessor::id(e.id.c_str()));
            tvg::Matrix mx;
            if (it == first.end() || it->second->type() != tvg::Type::Shape || !tvg_transform(*tr, mx)) continue;
            it->second->transform(mx);
        }
    }

    // Rest matrices to picture units, the shapes and their looks.
    std::unordered_map<const tvg::Paint*, Affine> rest_global;
    rest_global[m.doc] = {};
    for (usize i = 4; i < all.size(); ++i) {
        tvg::Paint* p = all[i];
        rest_global[p] = rest_global[p->parent()] * from_tvg(p->transform());
        if (p->type() == tvg::Type::Shape) {
            auto* s = static_cast<tvg::Shape*>(p);
            m.leaf_of[p] = static_cast<i32>(m.leaves.size());
            Leaf& l = m.leaves.emplace_back();
            l.shape = s;
            l.rest.take(s);
            l.visible = s->visible();
            l.rest_global = rest_global[p];
            const tvg::PathCommand* cmds = nullptr;
            const tvg::Point* pts = nullptr;
            u32 nc = 0, np = 0;
            if (s->path(&cmds, &nc, &pts, &np) == tvg::Result::Success) {
                l.cmds.assign(cmds, cmds + nc);
                l.pts.assign(pts, pts + np);
            }
        }
    }

    // Parts: the elements with an id that ThorVG drew, by the id's hash (the first paint with it, as ThorVG finds one).
    std::unordered_map<u32, const tvg::Paint*> by_hash;
    for (usize i = 4; i < all.size(); ++i)
        if (all[i]->id) by_hash.emplace(all[i]->id, all[i]);
    for (usize i = 1; i < m.outline.elements.size(); ++i) {
        const SvgElement& e = m.outline.elements[i];
        if (e.id.empty() || !drawable_tag(e.tag)) continue;
        auto it = by_hash.find(tvg::Accessor::id(e.id.c_str()));
        if (it == by_hash.end() || m.part_of.count(it->second)) continue;
        tvg::Paint* p = const_cast<tvg::Paint*>(it->second);
        m.part_of[p] = static_cast<i32>(m.parts.size());
        Part& part = m.parts.emplace_back();
        part.id = e.id;
        part.element = static_cast<i32>(i);
        part.shape = p->type() == tvg::Type::Shape;
        PartRec& r = m.recs.emplace_back();
        r.paint = p;
        r.rest_local = p->transform();
        r.rest_opacity = p->opacity();
        r.rest_visible = p->visible();
        r.parent_global = rest_global[p->parent()];
        r.rest_global = rest_global[p];
        if (auto l = m.leaf_of.find(p); l != m.leaf_of.end()) r.leaf = l->second;
        if (const tvg::Paint* parent = p->parent(); parent && parent->type() == tvg::Type::Scene) {
            auto* scene = static_cast<tvg::Scene*>(const_cast<tvg::Paint*>(parent));
            auto g = std::find_if(m.groups.begin(), m.groups.end(), [&](const GroupRec& x) { return x.scene == scene; });
            if (g == m.groups.end()) {
                m.groups.push_back({scene, std::vector<tvg::Paint*>(scene->paints().begin(), scene->paints().end())});
                g = m.groups.end() - 1;
            }
            r.group = static_cast<i32>(g - m.groups.begin());
        }
    }
    // A shape without a stroke has a width all the same in SVG (stroke-width, else 1), which ThorVG drops: it is read
    // from the nearest element with an id, the shape's own or its group's.
    {
        std::unordered_map<u32, i32> element_of;
        for (usize i = 1; i < m.outline.elements.size(); ++i)
            if (!m.outline.elements[i].id.empty()) element_of.emplace(tvg::Accessor::id(m.outline.elements[i].id.c_str()), static_cast<i32>(i));
        for (Leaf& l : m.leaves) {
            if (l.rest.stroke_width > 0) {
                l.own_width = l.rest.stroke_width;
                continue;
            }
            i32 e = 0;
            for (const tvg::Paint* q = l.shape; q && q != m.doc; q = q->parent())
                if (auto it = q->id ? element_of.find(q->id) : element_of.end(); it != element_of.end()) {
                    e = it->second;
                    break;
                }
            l.own_width = static_cast<f32>(std::max(0.0, vector::svg_number(vector::inherited(m.outline, e, "stroke-width"), 1.0)));
        }
    }
    for (usize i = 0; i < m.parts.size(); ++i)
        for (const tvg::Paint* q = m.recs[i].paint->parent(); q && q != m.doc; q = q->parent())
            if (auto it = m.part_of.find(q); it != m.part_of.end()) {
                m.parts[i].parent = it->second;
                break;
            }
    for (usize l = 0; l < m.leaves.size(); ++l)
        for (const tvg::Paint* q = m.leaves[l].shape; q && q != m.doc; q = q->parent())
            if (auto it = m.part_of.find(q); it != m.part_of.end()) {
                m.recs[static_cast<usize>(it->second)].leaves.push_back(static_cast<i32>(l));
                if (m.leaves[l].part < 0) m.leaves[l].part = it->second;
            }

    // Every shape that is a part gets a scene of its own round it, for the blur and the shadow of part 4б. With no
    // effect it draws the shape just as before. The groups' orders are taken again with these scenes in them.
    for (PartRec& r : m.recs) {
        const tvg::Paint* parent = r.paint->parent();
        if (r.paint->type() != tvg::Type::Shape || !parent || parent->type() != tvg::Type::Scene) continue;
        auto* scene = static_cast<tvg::Scene*>(const_cast<tvg::Paint*>(parent));
        tvg::Scene* w = tvg::Scene::gen();
        scene->add(w, r.paint);
        r.paint->ref();
        scene->remove(r.paint);
        w->add(r.paint);
        r.paint->unref(false);
        r.wrap = w;
    }
    for (GroupRec& g : m.groups) g.rest.assign(g.scene->paints().begin(), g.scene->paints().end());

    // Rest bounds (of what the part draws; of all its shapes when it draws nothing) and the pivots at their centres.
    m.pivots.resize(m.parts.size());
    for (usize i = 0; i < m.parts.size(); ++i) {
        Box drawn, any;
        for (i32 l : m.recs[i].leaves) {
            const Leaf& leaf = m.leaves[static_cast<usize>(l)];
            const Affine g = rest_global[leaf.shape];
            path_box(leaf.shape, g, any);
            if (leaf.visible && (leaf.rest.has_fill() || leaf.rest.has_stroke())) path_box(leaf.shape, g, drawn);
        }
        const Box& b = drawn.empty() ? any : drawn;
        Part& part = m.parts[i];
        if (!b.empty()) part.x0 = b.x0, part.y0 = b.y0, part.x1 = b.x1, part.y1 = b.y1;
        m.pivots[i] = {(part.x0 + part.x1) / 2, (part.y0 + part.y1) / 2};
    }
    m.picture = m.parts.size();
    m.source.assign(m.picture, -1);
    m.number.assign(m.picture, 0);
    m.drawn.assign(m.picture, 0);
    return true;
}

bool Rig::loaded() const { return impl_ && impl_->pic != nullptr; }
const SvgOutline& Rig::outline() const { return impl_->outline; }
std::span<const Part> Rig::parts() const { return {impl_->parts.data(), impl_->picture}; }

i32 Rig::part(std::string_view id) const {
    for (usize i = 0; i < impl_->picture; ++i)
        if (impl_->parts[i].id == id) return static_cast<i32>(i);
    return -1;
}

f64 Rig::width() const { return impl_->vb[2]; }
f64 Rig::height() const { return impl_->vb[3]; }

void Rig::set_pivot(std::string_view id, vector::Point at) {
    if (const i32 i = part(id); i >= 0) impl_->pivots[static_cast<usize>(i)] = at;
    for (auto& [name, d] : impl_->drawings) d.set_pivot(id, at);
}

vector::Point Rig::pivot(i32 i) const {
    if (i < 0 || static_cast<usize>(i) >= impl_->pivots.size()) return {};
    return impl_->pivots[static_cast<usize>(i)];
}

bool Rig::pose_alone(const Pose& pose, std::string& why, f64 snap, const Rig& family, const SwapSide* side) {
    Impl& m = *impl_;
    if (!m.pic) {
        if (why.empty()) why = "SVG не загружен";
        return false;
    }
    bool failed = false;
    auto fail = [&](std::string text) {
        failed = true;
        if (why.empty()) why = std::move(text);
    };

    // Back to rest.
    m.moves.assign(m.recs.size(), {});
    for (PartRec& r : m.recs) {
        r.paint->transform(r.rest_local);
        r.paint->opacity(r.rest_opacity);
        r.carry = {};
        if (r.hidden) {
            r.paint->visible(r.rest_visible);
            r.hidden = false;
            if (r.leaf >= 0) m.leaves[static_cast<usize>(r.leaf)].hidden = false;
        }
        if (r.fx_on) {
            if (tvg::Scene* sc = fx_scene(r)) sc->add(tvg::SceneEffect::Clear);
            r.fx = {};
            r.fx_on = false;
        }
        if (r.masked) {
            slot(r)->mask(nullptr, tvg::MaskMethod::None);
            r.masked = false;
        }
    }
    if (m.reordered) {
        for (GroupRec& g : m.groups)
            for (tvg::Paint* p : g.rest) reinsert(g.scene, p, nullptr);
        m.reordered = false;
    }
    for (Leaf& l : m.leaves) {
        l.morphed = false;
        if (l.path_changed) {
            l.shape->reset();
            l.shape->appendPath(l.cmds.data(), static_cast<u32>(l.cmds.size()), l.pts.data(), static_cast<u32>(l.pts.size()));
            l.path_changed = false;
        }
        if (l.look_changed) {
            l.rest.put(l.shape);
            l.look_changed = false;
        }
        if (l.trim_changed) {
            l.shape->trimpath(0, 1);
            l.trim_changed = false;
        }
    }

    // Copies (part 4г): made from their parts at rest the first time a pose has them, hidden in a pose without them.
    std::fill(m.drawn.begin(), m.drawn.end(), 0);
    for (const PartCopy& c : pose.copies) {
        const i32 si = part(c.source);
        i32 ci = m.find(c.id);
        if (si < 0) {
            fail("копия «" + c.id + "»: в рисунке нет части «" + c.source + "»");
        } else if (ci >= 0 && static_cast<usize>(ci) < m.picture) {
            fail("копия «" + c.id + "»: так зовут часть рисунка");
        } else if (ci >= 0 && m.source[static_cast<usize>(ci)] != si) {
            fail("копия «" + c.id + "» уже есть, но не части «" + c.source + "»");
        } else if (ci < 0 && m.recs[static_cast<usize>(si)].leaves.empty()) {
            fail("копия «" + c.id + "»: у части «" + c.source + "» нет фигур");
        } else if (ci < 0 && m.recs[static_cast<usize>(si)].group < 0) {
            fail("копия «" + c.id + "»: часть «" + c.source + "» не в группе");
        } else {
            if (ci < 0) ci = m.add_copy(c.id, si, c.number);
            m.drawn[static_cast<usize>(ci)] = 1;
            m.pivots[static_cast<usize>(ci)] = m.pivots[static_cast<usize>(si)];
        }
    }
    for (usize i = m.picture; i < m.recs.size(); ++i) {
        if (m.drawn[i]) continue;
        PartRec& r = m.recs[i];
        r.paint->visible(false);
        r.hidden = true;
        if (r.leaf >= 0) m.leaves[static_cast<usize>(r.leaf)].hidden = true;
    }
    // A part of the pose: the picture's or a copy it draws.
    auto posed_part = [&](std::string_view id) {
        const i32 i = m.find(id);
        return i >= 0 && (static_cast<usize>(i) < m.picture || m.drawn[static_cast<usize>(i)]) ? i : -1;
    };

    // Morphs: the shape's path and look become the pair's at k.
    for (const auto& [id, morph] : pose.morphs) {
        const i32 pi = posed_part(id);
        if (pi < 0) {
            fail("морфинг: в рисунке нет фигуры «" + id + "»");
            continue;
        }
        const PartRec& r = m.recs[static_cast<usize>(pi)];
        if (r.leaf < 0) {
            fail("морфинг: «" + id + "» — группа, морфинг бывает только у фигуры");
            continue;
        }
        // The target: a shape of this picture, else of the drawing that has it (part 4д), in this shape's units.
        const Rig* tp_rig = this;
        std::string tp_name;
        i32 ti = m.outline.find(morph.target);
        if (ti < 0 && &family != this) {
            tp_name = family.holder_name(morph.target);
            tp_rig = family.drawing(tp_name);
            ti = tp_rig ? tp_rig->impl_->outline.find(morph.target) : -1;
        }
        if (ti < 0 || !tp_rig) {
            fail("морфинг «" + id + "»: в рисунке нет фигуры «" + morph.target + "»");
            continue;
        }
        if (!std::isfinite(morph.k)) {
            fail("морфинг «" + id + "»: k — не число");
            continue;
        }
        const Impl& tm = *tp_rig->impl_;
        const i32 si = m.parts[static_cast<usize>(pi)].element;
        if (tp_rig == this && transform_chain(m.outline, si) != transform_chain(m.outline, ti)) {
            fail("морфинг «" + id + "» в «" + morph.target + "»: у фигур разные transform, их точки не в одних координатах");
            continue;
        }
        const i32 tp = tp_rig->part(morph.target);
        const PartRec* tr = tp >= 0 ? &tm.recs[static_cast<usize>(tp)] : nullptr;
        if (tp_rig != this && (!tr || tr->leaf < 0)) {
            fail("морфинг «" + id + "»: в рисунке нет фигуры «" + morph.target + "»");
            continue;
        }
        Leaf& leaf = m.leaves[static_cast<usize>(r.leaf)];
        const std::string key = id + '\n' + morph.target + (tp_rig == this ? std::string() : "\n@" + tp_name);
        auto it = m.morphs.find(key);
        if (it == m.morphs.end()) {
            vector::Shape from, to;
            std::string err;
            if (!shape_path(m.outline, si, from.path, err)) {
                fail("морфинг «" + id + "»: " + err);
                continue;
            }
            if (!shape_path(tm.outline, ti, to.path, err)) {
                fail("морфинг «" + id + "» в «" + morph.target + "»: " + err);
                continue;
            }
            from.look = leaf.rest.vector_look();
            if (tr && tr->leaf >= 0 && !vector::display_none(tm.outline, ti)) to.look = tm.leaves[static_cast<usize>(tr->leaf)].rest.vector_look();
            else to.look = vector::look_from_svg(tm.outline, ti, from.look);
            if (tp_rig != this) to = in_units(std::move(to), inverse(leaf.rest_global) * tm.leaves[static_cast<usize>(tr->leaf)].rest_global);
            it = m.morphs.emplace(key, vector::prepare(from, to)).first;
        }
        vector::mix(it->second, morph.k, m.scratch);
        put_shape(leaf, m.scratch);
        if (side) {
            leaf.morphed = true;
            leaf.now = m.scratch;
        }
    }

    // A swap (part 4д). The old picture: each shape part with a pair in the new one morphs from what it is now (its
    // own shape, or its morph) to what the pair is now, in this shape's units.
    std::vector<char> paired(m.parts.size(), 0);
    if (side && side->other) {
        const Rig& o = *side->other;
        const Impl& om = *o.impl_;
        for (usize pi = 0; pi < m.parts.size(); ++pi) {
            if (pi >= m.picture && !m.drawn[pi]) continue;
            const std::string& id = m.parts[pi].id;
            if (!swap_shape(pose, id) || !o.swap_shape(pose, id)) continue;
            paired[pi] = 1;
            // The new picture is posed first: its copies are made by now.
            const i32 oi = om.find(id);
            if (!side->from || oi < 0) continue;
            Leaf& a = m.leaves[static_cast<usize>(m.recs[pi].leaf)];
            const Leaf& b = om.leaves[static_cast<usize>(om.recs[static_cast<usize>(oi)].leaf)];
            auto now = [](const Impl& im, const Leaf& l, i32 element, vector::Shape& out, std::string& err) {
                if (l.morphed) {
                    out = l.now;
                    return true;
                }
                out.look = l.rest.vector_look();
                return shape_path(im.outline, element, out.path, err);
            };
            vector::Shape from, to;
            std::string err;
            if (!now(m, a, m.parts[pi].element, from, err) || !now(om, b, om.parts[static_cast<usize>(oi)].element, to, err)) {
                fail("смена рисунка, «" + id + "»: " + err);
                continue;
            }
            to = in_units(std::move(to), inverse(a.rest_global) * b.rest_global);
            // The same pair at rest again and again: prepared once.
            const vector::Morph* mo = nullptr;
            vector::Morph fresh;
            if (!a.morphed && !b.morphed) {
                const std::string key = "\x01" + id + '\n' + side->other_name;
                auto it = m.morphs.find(key);
                if (it == m.morphs.end()) it = m.morphs.emplace(key, vector::prepare(from, to)).first;
                mo = &it->second;
            } else {
                fresh = vector::prepare(from, to);
                mo = &fresh;
            }
            vector::mix(*mo, side->k, m.scratch);
            put_shape(a, m.scratch);
        }
    }

    // The parts the pose gives, by index, each checked.
    std::vector<const PartPose*> posed(m.parts.size(), nullptr);
    std::vector<i32> carrier(m.parts.size(), -1); // the part a link carries it by (part 4в)
    for (const auto& [id, pp] : pose.parts) {
        const i32 pi = posed_part(id);
        if (pi < 0) {
            fail("в рисунке нет части «" + id + "»");
            continue;
        }
        const f64 nums[] = {pp.x,       pp.y,         pp.rot,          pp.scale,          pp.sx,      pp.sy,
                            pp.skew_x,  pp.skew_y,    pp.opacity,      pp.stroke_width,   pp.stroke_width_k,
                            pp.dash,    pp.draw_from, pp.draw_to,      pp.tint_k,         pp.flash,   pp.flash_color_k,
                            pp.blur,    pp.shadow_k,  pp.shadow_x,     pp.shadow_y,       pp.shadow_blur,
                            pp.carry.a, pp.carry.b,   pp.carry.c,      pp.carry.d,        pp.carry.e, pp.carry.f};
        if (!std::all_of(std::begin(nums), std::end(nums), [](f64 v) { return std::isfinite(v); })) {
            fail("часть «" + id + "»: не число в позе");
            continue;
        }
        posed[static_cast<usize>(pi)] = &pp;
        if (pp.carry_by.empty()) continue;
        const i32 ci = part(pp.carry_by);
        if (ci < 0) fail("крепление части «" + id + "»: в рисунке нет части «" + pp.carry_by + "»");
        else carrier[static_cast<usize>(pi)] = ci;
    }
    // What a part's place waits for: the posed parts it is inside, and the part that carries it (which waits for its own
    // groups in turn). A link that comes back round to its own part is dropped.
    auto deps = [&](i32 p, std::vector<i32>& out) {
        out.clear();
        for (i32 q = m.parts[static_cast<usize>(p)].parent; q >= 0; q = m.parts[static_cast<usize>(q)].parent)
            if (posed[static_cast<usize>(q)]) out.push_back(q);
        if (const i32 c = carrier[static_cast<usize>(p)]; c >= 0) out.push_back(c);
    };
    std::vector<i32> work, next;
    for (usize p = 0; p < m.parts.size(); ++p) {
        if (carrier[p] < 0) continue;
        std::vector<char> seen(m.parts.size(), 0);
        bool back = false;
        deps(static_cast<i32>(p), work);
        while (!work.empty() && !back) {
            const i32 q = work.back();
            work.pop_back();
            if (static_cast<usize>(q) == p) back = true;
            if (back || seen[static_cast<usize>(q)]) continue;
            seen[static_cast<usize>(q)] = 1;
            deps(q, next);
            work.insert(work.end(), next.begin(), next.end());
        }
        if (back) {
            fail("крепление по кругу: «" + m.parts[p].id + "» и «" + m.parts[static_cast<usize>(carrier[p])].id + "» несут друг друга");
            carrier[p] = -1;
        }
    }
    // Each part after what it waits for (a group before the parts in it, so a part's own look goes over its group's).
    std::vector<i32> order;
    std::vector<char> placed(m.parts.size(), 0);
    std::vector<std::pair<i32, usize>> stack;
    for (usize p = 0; p < m.parts.size(); ++p) {
        if (!posed[p] || placed[p]) continue;
        stack.push_back({static_cast<i32>(p), 0});
        while (!stack.empty()) {
            auto& [q, i] = stack.back();
            deps(q, work);
            while (i < work.size() && (placed[static_cast<usize>(work[i])] || !posed[static_cast<usize>(work[i])])) ++i;
            if (i < work.size()) {
                const i32 d = work[i];
                placed[static_cast<usize>(d)] = 2; // on the way: not taken again
                stack.push_back({d, 0});
                continue;
            }
            if (placed[static_cast<usize>(q)] != 1) order.push_back(q);
            placed[static_cast<usize>(q)] = 1;
            stack.pop_back();
        }
    }

    // Parts: moved, turned and scaled around their pivots in rest picture units, then carried by their links, then
    // their opacity and looks.
    for (const i32 pi : order) {
        const PartPose& pp = *posed[static_cast<usize>(pi)];
        PartRec& r = m.recs[static_cast<usize>(pi)];
        f64 x = pp.x, y = pp.y;
        if (snap > 0) x = std::round(x / snap) * snap, y = std::round(y / snap) * snap;
        m.moves[static_cast<usize>(pi)] = {x, y};
        const Affine a = pose_matrix(pp, m.pivots[static_cast<usize>(pi)], snap);
        Affine local = inverse(r.parent_global) * a * r.parent_global * from_tvg(r.rest_local);
        const i32 ci = carrier[static_cast<usize>(pi)];
        if (ci >= 0 || pp.carry != Affine{}) {
            // C over its groups: the part's place in picture units as it would stand, then C.
            Affine c = pp.carry;
            if (ci >= 0) {
                const PartRec& cr = m.recs[static_cast<usize>(ci)];
                c = m.global(cr.paint) * inverse(cr.rest_global) * c;
            }
            const Affine pg = m.global(r.paint->parent());
            local = inverse(pg) * c * pg * local;
            r.carry = c;
        }
        r.paint->transform(to_tvg(local));
        r.paint->opacity(to_u8(r.paint->opacity() / 255.0 * std::clamp(pp.opacity, 0.0, 1.0)));
        const f64 fk = std::isfinite(pp.fill_k) ? std::clamp(pp.fill_k, 0.0, 1.0) : 1.0;
        if (pp.has_fill && fk > 0)
            for (i32 li : r.leaves) {
                Leaf& l = m.leaves[static_cast<usize>(li)];
                u8 cr, cg, cb, ca = 0;
                const bool grad = l.shape->fill() != nullptr;
                if (!grad) l.shape->fill(&cr, &cg, &cb, &ca);
                if (!grad && ca == 0) continue; // a shape without a fill stays without one
                if (grad && fk < 0.5) continue; // a gradient does not mix: it changes halfway
                const u8 base = grad ? 255 : ca;
                const u8 fa = static_cast<u8>((base * pp.fill.a + 127) / 255);
                if (grad || fk >= 1) {
                    l.shape->fill(pp.fill.r, pp.fill.g, pp.fill.b, fa);
                } else {
                    auto lerp = [fk](u8 c0, u8 c1) { return static_cast<u8>(std::lround(c0 + (c1 - c0) * fk)); };
                    l.shape->fill(lerp(cr, pp.fill.r), lerp(cg, pp.fill.g), lerp(cb, pp.fill.b), lerp(ca, fa));
                }
                l.look_changed = true;
            }
        // The stroke as the fill, from each shape's own colour; a shape without one gets it at its own width, coming in
        // from transparent.
        const f64 sk = std::clamp(pp.stroke_k, 0.0, 1.0);
        if (pp.has_stroke && sk > 0)
            for (i32 li : r.leaves) {
                Leaf& l = m.leaves[static_cast<usize>(li)];
                u8 cr, cg, cb, ca = 0;
                const bool grad = l.shape->strokeFill() != nullptr;
                if (!grad) l.shape->strokeFill(&cr, &cg, &cb, &ca);
                if (grad && sk < 0.5) continue; // a gradient does not mix: it changes halfway
                const bool none = !grad && (ca == 0 || l.shape->strokeWidth() <= 0);
                const u8 base = grad || none ? 255 : ca;
                const u8 sa = static_cast<u8>((base * pp.stroke.a + 127) / 255);
                if (none) {
                    if (l.shape->strokeWidth() <= 0) l.shape->strokeWidth(l.own_width);
                    l.shape->strokeFill(pp.stroke.r, pp.stroke.g, pp.stroke.b, static_cast<u8>(std::lround(sa * sk)));
                } else if (grad || sk >= 1) {
                    l.shape->strokeFill(pp.stroke.r, pp.stroke.g, pp.stroke.b, sa);
                } else {
                    auto lerp = [sk](u8 c0, u8 c1) { return static_cast<u8>(std::lround(c0 + (c1 - c0) * sk)); };
                    l.shape->strokeFill(lerp(cr, pp.stroke.r), lerp(cg, pp.stroke.g), lerp(cb, pp.stroke.b), lerp(ca, sa));
                }
                l.look_changed = true;
            }
        // The width from each shape's own: what it draws now, else the width SVG gives it.
        if (pp.has_stroke_width && pp.stroke_width_k > 0)
            for (i32 li : r.leaves) {
                Leaf& l = m.leaves[static_cast<usize>(li)];
                const f64 own = l.shape->strokeWidth() > 0 ? l.shape->strokeWidth() : l.own_width;
                l.shape->strokeWidth(static_cast<f32>(std::max(0.0, own + (pp.stroke_width - own) * pp.stroke_width_k)));
                l.look_changed = true;
            }
        // The dash: each shape's own pattern moved along its line.
        if (pp.dash != 0)
            for (i32 li : r.leaves) {
                Leaf& l = m.leaves[static_cast<usize>(li)];
                if (l.rest.dash.empty()) continue;
                l.shape->strokeDash(l.rest.dash.data(), static_cast<u32>(l.rest.dash.size()),
                                    static_cast<f32>(static_cast<f64>(l.rest.dash_offset) + pp.dash));
                l.look_changed = true;
            }
        // The share of each shape's path drawn, its contours one line after another; nothing when from ≥ to.
        if (pp.draw_from != 0 || pp.draw_to != 1) {
            const f64 from = std::clamp(pp.draw_from, 0.0, 1.0), to = std::clamp(pp.draw_to, 0.0, 1.0);
            for (i32 li : r.leaves) {
                Leaf& l = m.leaves[static_cast<usize>(li)];
                if (from < to) l.shape->trimpath(static_cast<f32>(from), static_cast<f32>(to), false);
                else l.shape->trimpath(0, 0, false); // ThorVG draws nothing of an empty share (and wraps a reversed one)
                l.trim_changed = true;
            }
        }
        if (!pp.visible) {
            r.paint->visible(false);
            r.hidden = true;
            if (r.leaf >= 0) m.leaves[static_cast<usize>(r.leaf)].hidden = true;
        }
    }

    // A swap (part 4д): the new picture hides its pairs (the old one draws them), every other shape of either fades:
    // the old by 1 − k, the new by k.
    if (side && side->other) {
        const f64 k = side->from ? 1 - side->k : side->k;
        for (usize pi = 0; pi < m.parts.size(); ++pi) {
            if (!paired[pi] || side->from) continue;
            PartRec& r = m.recs[pi];
            r.paint->visible(false);
            r.hidden = true;
            m.leaves[static_cast<usize>(r.leaf)].hidden = true;
        }
        for (usize li = 0; li < m.leaves.size(); ++li) {
            Leaf& l = m.leaves[li];
            const i32 own = l.part >= 0 && m.recs[static_cast<usize>(l.part)].leaf == static_cast<i32>(li) ? l.part : -1;
            if (l.hidden || (own >= 0 && paired[static_cast<usize>(own)])) continue;
            l.shape->opacity(to_u8(l.shape->opacity() / 255.0 * k));
            l.look_changed = true;
        }
    }

    // Tints and flashes: the colours of each shape, its own part's first, then its groups' (part 4б).
    auto tone = [&](i32 q, Tone& t) {
        const PartPose* pp = q >= 0 ? posed[static_cast<usize>(q)] : nullptr;
        if (!pp) return false;
        const f64 tk = std::clamp(pp->tint_k, 0.0, 1.0), fl = std::clamp(pp->flash, 0.0, 1.0);
        t = {};
        if (pp->has_tint && tk > 0) {
            t.tint = true;
            const u8 c[4] = {pp->tint.r, pp->tint.g, pp->tint.b, pp->tint.a};
            for (int i = 0; i < 4; ++i) t.m[i] = 255 + (c[i] - 255) * tk;
        }
        if (fl > 0) {
            t.flash = fl;
            const f64 ck = std::clamp(pp->flash_color_k, 0.0, 1.0);
            const u8 c[3] = {pp->flash_color.r, pp->flash_color.g, pp->flash_color.b};
            if (pp->has_flash_color && ck > 0)
                for (int i = 0; i < 3; ++i) t.f[i] = 255 + (c[i] - 255) * ck;
        }
        return t.tint || t.flash > 0;
    };
    for (Leaf& l : m.leaves) {
        std::vector<Tone> tones;
        for (i32 q = l.part; q >= 0; q = m.parts[static_cast<usize>(q)].parent)
            if (Tone t; tone(q, t)) tones.push_back(t);
        if (tones.empty()) continue;
        auto colour = [&](u8& r, u8& g, u8& b, u8& a) {
            for (const Tone& t : tones) t.apply(r, g, b, a);
        };
        auto gradient = [&](const tvg::Fill* f) {
            tvg::Fill* d = f->duplicate();
            const tvg::Fill::ColorStop* stops = nullptr;
            const u32 n = d->colorStops(&stops);
            std::vector<tvg::Fill::ColorStop> v(stops, stops + n);
            for (tvg::Fill::ColorStop& c : v) colour(c.r, c.g, c.b, c.a);
            d->colorStops(v.data(), n);
            return d;
        };
        tvg::Shape* s = l.shape;
        if (const tvg::Fill* f = s->fill()) {
            s->fill(gradient(f));
        } else {
            u8 r, g, b, a = 0;
            s->fill(&r, &g, &b, &a);
            if (a > 0) colour(r, g, b, a), s->fill(r, g, b, a); // a shape without a fill stays without one
        }
        if (s->strokeWidth() > 0) {
            if (const tvg::Fill* f = s->strokeFill()) {
                s->strokeFill(gradient(f));
            } else {
                u8 r, g, b, a = 0;
                s->strokeFill(&r, &g, &b, &a);
                if (a > 0) colour(r, g, b, a), s->strokeFill(r, g, b, a);
            }
        }
        l.look_changed = true;
    }

    // Blur and shadow on each part's scene, in picture units: ThorVG takes its sigma and offset through the scene's
    // matrix (its scale from e11 and e12), so they go in through its inverse.
    for (usize pi = 0; pi < posed.size(); ++pi) {
        const PartPose* pp = posed[pi];
        if (!pp) continue;
        const u8 sa = pp->has_shadow ? to_u8(pp->shadow.a / 255.0 * std::clamp(pp->shadow_k, 0.0, 1.0)) : 0;
        if (!(pp->blur > 0) && sa == 0) continue;
        PartRec& r = m.recs[pi];
        tvg::Scene* sc = fx_scene(r);
        if (!sc) continue;
        const Affine g = m.global(sc);
        const f64 scale = std::sqrt(g.a * g.a + g.c * g.c), det = g.a * g.d - g.b * g.c;
        if (!(scale > 1e-12) || !(std::abs(det) > 1e-12)) continue;
        Fx fx;
        if (pp->blur > 0) fx.blur = pp->blur / scale;
        if (sa > 0) {
            fx.shadow = true;
            fx.r = pp->shadow.r, fx.g = pp->shadow.g, fx.b = pp->shadow.b, fx.a = sa;
            // ThorVG: x = distance · sin(angle), y = −distance · cos(angle), then through the matrix.
            const f64 x = (g.d * pp->shadow_x - g.c * pp->shadow_y) / det, y = (-g.b * pp->shadow_x + g.a * pp->shadow_y) / det;
            fx.distance = std::hypot(x, y);
            fx.angle = std::atan2(x, -y) * 180.0 / vector::trig::kPi;
            if (fx.angle < 0) fx.angle += 360;
            fx.sigma = std::max(0.0, pp->shadow_blur) / scale;
        }
        r.fx = fx;
        r.fx_on = true;
        put_fx(r, FxMode::Full);
    }

    // Masks: a copy of the mask's shapes as they stand, opaque, where they are drawn. ThorVG draws a mask through the
    // masked paint's parent, so the copy goes from there.
    for (usize pi = 0; pi < posed.size(); ++pi) {
        const PartPose* pp = posed[pi];
        if (!pp || pp->mask.empty()) continue;
        const std::string& id = m.parts[pi].id;
        const i32 mi = part(pp->mask);
        if (mi < 0) {
            fail("маска части «" + id + "»: в рисунке нет части «" + pp->mask + "»");
            continue;
        }
        if (static_cast<usize>(mi) == pi) {
            fail("часть «" + id + "» не маска сама себе");
            continue;
        }
        const PartRec& mr = m.recs[static_cast<usize>(mi)];
        if (mr.leaves.empty()) {
            fail("у маски «" + pp->mask + "» нет фигур");
            continue;
        }
        PartRec& r = m.recs[pi];
        tvg::Paint* sl = slot(r);
        const Affine from_parent = inverse(m.global(sl->parent()));
        tvg::Scene* target = tvg::Scene::gen();
        for (i32 li : mr.leaves) {
            const Leaf& l = m.leaves[static_cast<usize>(li)];
            auto* d = static_cast<tvg::Shape*>(l.shape->duplicate());
            u8 cr, cg, cb, fa = 0, sa2 = 0;
            const bool fill_grad = d->fill() != nullptr, stroke_grad = d->strokeFill() != nullptr;
            if (!fill_grad) d->fill(&cr, &cg, &cb, &fa);
            if (!stroke_grad) d->strokeFill(&cr, &cg, &cb, &sa2);
            const bool fill = fill_grad || fa > 0, stroke = d->strokeWidth() > 0 && (stroke_grad || sa2 > 0);
            d->fill(255, 255, 255, fill || !stroke ? 255 : 0); // a hidden shape (display="none") has neither: its area
            d->strokeFill(255, 255, 255, stroke ? 255 : 0);
            d->opacity(255);
            d->visible(true);
            d->transform(to_tvg(from_parent * m.global(l.shape)));
            target->add(d);
        }
        sl->mask(target, pp->mask_invert ? tvg::MaskMethod::InvAlpha : tvg::MaskMethod::Alpha);
        r.masked = true;
    }

    // The order: each step among the part's neighbours in its group.
    for (const OrderStep& step : pose.order) {
        const i32 pi = posed_part(step.part);
        if (pi < 0) {
            fail("порядок: в рисунке нет части «" + step.part + "»");
            continue;
        }
        const PartRec& r = m.recs[static_cast<usize>(pi)];
        if (r.group < 0) continue;
        tvg::Scene* scene = m.groups[static_cast<usize>(r.group)].scene;
        tvg::Paint* at = nullptr;
        if (step.kind == OrderStep::Kind::Above || step.kind == OrderStep::Kind::Below) {
            const i32 oi = part(step.other);
            if (oi < 0) {
                fail("порядок: в рисунке нет части «" + step.other + "»");
                continue;
            }
            if (!siblings(pi, oi)) {
                fail(step.part + " и " + step.other + " в разных группах: порядок меняется только среди соседей");
                continue;
            }
            if (oi == pi) continue;
            tvg::Paint* other = slot(m.recs[static_cast<usize>(oi)]);
            if (step.kind == OrderStep::Kind::Below) {
                at = other;
            } else {
                const std::list<tvg::Paint*>& list = scene->paints();
                auto it = std::find(list.begin(), list.end(), other);
                if (it != list.end()) ++it;
                at = it == list.end() ? nullptr : *it;
                if (at == slot(r)) continue; // already right above it
            }
        } else if (step.kind == OrderStep::Kind::Bottom) {
            at = scene->paints().empty() ? nullptr : scene->paints().front();
            if (at == slot(r)) continue;
        }
        reinsert(scene, slot(r), at);
        m.reordered = true;
    }
    m.pic->transform(m.pic->transform()); // ThorVG redraws a picture only when the picture itself changed
    return !failed;
}

bool Rig::pose(const Pose& pose, std::string& why, f64 snap) {
    Impl& m = *impl_;
    why.clear();
    m.shown.clear();
    m.swap = false;
    m.swap_from.clear();
    if (m.drawings.empty() && pose.drawing.empty() && !pose.swap) return pose_alone(pose, why, snap, *this, nullptr);
    // Part 4д: the picture shown takes the pose of its parts; in a swap, the new one first, then the old one under it.
    Rig* to = drawing(pose.drawing);
    if (!to) {
        why = "нет рисунка «" + pose.drawing + "»";
        pose_alone(for_drawing(pose, *this), why, snap, *this, nullptr);
        return false;
    }
    Rig* from = pose.swap ? drawing(pose.swap_from) : nullptr;
    if (pose.swap && !from) why = "нет рисунка «" + pose.swap_from + "»";
    if (from == to) from = nullptr;
    m.shown = to == this ? std::string() : pose.drawing;
    bool ok = why.empty();
    if (!from) return to->pose_alone(for_drawing(pose, *to), why, snap, *this, nullptr) && ok;
    m.swap = true;
    m.swap_from = from == this ? std::string() : pose.swap_from;
    const f64 k = std::isfinite(pose.swap_k) ? std::clamp(pose.swap_k, 0.0, 1.0) : 0;
    const SwapSide in{false, from, m.swap_from, k}, out{true, to, m.shown, k};
    ok = to->pose_alone(for_drawing(pose, *to), why, snap, *this, &in) && ok;
    return from->pose_alone(for_drawing(pose, *from), why, snap, *this, &out) && ok;
}

Pose Rig::for_drawing(const Pose& pose, const Rig& d) const {
    if (impl_->drawings.empty()) return pose;
    // Whether some picture of the character has the part: the main one or a drawing.
    auto known = [&](const std::string& id) { return !holder_name(id).empty() || part(id) >= 0; };
    auto source_of = [&](const std::string& id) -> const std::string* {
        for (const PartCopy& c : pose.copies)
            if (c.id == id) return &c.source;
        return nullptr;
    };
    // A part d has, or one no picture has (its error stays).
    auto takes = [&](const std::string& id) {
        const std::string* src = source_of(id);
        const std::string& own = src ? *src : id;
        return d.part(own) >= 0 || !known(own);
    };
    auto lacks = [&](const std::string& id) { return d.part(id) < 0 && known(id); };
    Pose out;
    for (const PartCopy& c : pose.copies)
        if (takes(c.id)) out.copies.push_back(c);
    for (const auto& [id, pp] : pose.parts) {
        if (!takes(id)) continue;
        PartPose q = pp;
        if (!q.mask.empty() && lacks(q.mask)) q.mask.clear(); // no mask in a drawing without it
        if (!q.carry_by.empty() && lacks(q.carry_by)) q.carry_by.clear(); // carried as if its carrier were at rest
        out.parts.emplace(id, std::move(q));
    }
    for (const auto& [id, mo] : pose.morphs)
        if (takes(id)) out.morphs.emplace(id, mo);
    for (const OrderStep& step : pose.order)
        if (takes(step.part) && (step.other.empty() || !lacks(step.other))) out.order.push_back(step);
    return out;
}

bool Rig::swap_shape(const Pose& pose, const std::string& id) const {
    const Impl& m = *impl_;
    if (const i32 p = part(id); p >= 0)
        return m.recs[static_cast<usize>(p)].leaf >= 0 && !vector::display_none(m.outline, m.parts[static_cast<usize>(p)].element);
    for (const PartCopy& c : pose.copies)
        if (c.id == id) {
            const i32 src = part(c.source);
            return src >= 0 && m.recs[static_cast<usize>(src)].leaf >= 0; // a copy of a hidden shape is seen
        }
    return false;
}

bool Rig::add_drawing(const std::string& name, std::string_view svg, std::string& why) {
    Impl& m = *impl_;
    if (!m.pic) return why = "основной рисунок не загружен", false;
    if (name.empty() || name == "draw") return why = "рисунок «" + name + "»: так зовут основной рисунок", false;
    Rig d;
    if (!d.load(svg, why)) return why = "рисунок «" + name + "»: " + why, false;
    if (format_number(d.width()) != format_number(width()) || format_number(d.height()) != format_number(height()))
        return why = "у рисунка «" + name + "» другой холст: " + format_number(d.width()) + " × " + format_number(d.height()) +
                     ", а у основного " + format_number(width()) + " × " + format_number(height()),
               false;
    m.drawings.insert_or_assign(name, std::move(d));
    // Swaps prepared against the drawing before are gone with it.
    m.morphs.clear();
    for (auto& [n, other] : m.drawings) other.impl_->morphs.clear();
    return true;
}

std::vector<std::string> Rig::drawings() const {
    std::vector<std::string> out;
    for (const auto& [name, d] : impl_->drawings) out.push_back(name);
    return out;
}

Rig* Rig::drawing(std::string_view name) {
    if (name.empty() || name == "draw") return this;
    const auto it = impl_->drawings.find(std::string(name));
    return it == impl_->drawings.end() ? nullptr : &it->second;
}

const Rig* Rig::drawing(std::string_view name) const { return const_cast<Rig*>(this)->drawing(name); }

std::string Rig::holder_name(std::string_view id) const {
    auto has = [&](const Rig& r) { return r.part(id) >= 0 || r.impl_->outline.find(std::string(id)) >= 0; };
    if (!impl_->pic || has(*this)) return {};
    for (const auto& [name, d] : impl_->drawings)
        if (has(d)) return name;
    return {};
}

const Rig& Rig::holder(std::string_view id) const { return *drawing(holder_name(id)); }

Rig& Rig::shown() {
    Rig* r = drawing(impl_->shown);
    return r ? *r : *this;
}

const Rig& Rig::shown() const { return const_cast<Rig*>(this)->shown(); }

bool Rig::swapping() const { return impl_->swap; }

Affine Rig::to_pixels(u32 w, u32 h) const {
    const f64* vb = impl_->vb;
    if (vb[2] <= 0 || vb[3] <= 0) return {};
    const f64 s = std::min(w / vb[2], h / vb[3]);
    return {s, 0, 0, s, (w - vb[2] * s) / 2 - vb[0] * s, (h - vb[3] * s) / 2 - vb[1] * s};
}

assets::CookedTexture Rig::draw(u32 w, u32 h) { return draw(w, h, to_pixels(w, h)); }

assets::CookedTexture Rig::draw(u32 w, u32 h, const Affine& px) {
    // A swap's frame (part 4д): the new picture over the old one.
    Rig& to = shown();
    Rig* from = impl_->swap ? drawing(impl_->swap_from) : nullptr;
    if (!from) return to.draw_alone(w, h, px);
    assets::CookedTexture out = from->draw_alone(w, h, px);
    const assets::CookedTexture top = to.draw_alone(w, h, px);
    if (out.rgba8.size() != top.rgba8.size()) return top;
    over(out.rgba8, top.rgba8);
    return out;
}

assets::CookedTexture Rig::draw_alone(u32 w, u32 h, const Affine& px) {
    assets::CookedTexture out;
    Impl& m = *impl_;
    if (!m.pic || w == 0 || h == 0) return out;
    m.target(w, h);
    m.render(px);
    out.width = w, out.height = h;
    out.rgba8.assign(m.bytes(), m.bytes() + static_cast<usize>(w) * h * 4);
    return out;
}

assets::CookedTexture Rig::bake(u32 w, u32 h, const BakeOptions& options) {
    assets::CookedTexture out;
    if (!impl_->pic || w == 0 || h == 0) return out;
    out.width = w, out.height = h;
    // A swap's frame (part 4д): the old picture, the new one's pixels over it.
    Rig& to = shown();
    Rig* from = impl_->swap ? drawing(impl_->swap_from) : nullptr;
    if (from) {
        std::vector<u8> top;
        from->bake_alone(w, h, out.rgba8);
        to.bake_alone(w, h, top);
        over(out.rgba8, top);
    } else {
        to.bake_alone(w, h, out.rgba8);
    }
    if (!options.palette.empty())
        for (usize i = 0; i < out.rgba8.size(); i += 4) {
            if (out.rgba8[i + 3] == 0) continue;
            const Rgba8* best = nullptr;
            i64 best_d = 0;
            for (const Rgba8& c : options.palette) {
                const i64 dr = i64(out.rgba8[i]) - c.r, dg = i64(out.rgba8[i + 1]) - c.g, db = i64(out.rgba8[i + 2]) - c.b;
                const i64 d = dr * dr + dg * dg + db * db;
                if (!best || d < best_d) best = &c, best_d = d;
            }
            out.rgba8[i] = best->r, out.rgba8[i + 1] = best->g, out.rgba8[i + 2] = best->b;
        }
    if (options.outline) {
        assets::Effect e;
        e.kind = assets::EffectKind::Outline;
        e.color = {options.outline_color.r, options.outline_color.g, options.outline_color.b, 255};
        assets::apply_effect(out, assets::whole_rect(out), e);
    }
    return out;
}

void Rig::bake_alone(u32 w, u32 h, std::vector<u8>& out) {
    Impl& m = *impl_;
    out.assign(static_cast<usize>(w) * h * 4, 0);
    if (!m.pic) return;
    m.target(w, h);
    const Affine px = to_pixels(w, h);

    // One pass: what is drawn now, cut at half cover; a colour given, or each pixel's own (a gradient).
    auto pass = [&](bool own, u8 r, u8 g, u8 b) {
        m.render(px);
        const u8* src = m.bytes();
        u8* dst = out.data();
        for (usize i = 0, n = static_cast<usize>(w) * h * 4; i < n; i += 4) {
            if (src[i + 3] < 128) continue;
            dst[i] = own ? src[i] : r;
            dst[i + 1] = own ? src[i + 1] : g;
            dst[i + 2] = own ? src[i + 2] : b;
            dst[i + 3] = 255;
        }
    };

    // The shapes in the order they are drawn now: the SVG's, or as the pose's order steps left the groups.
    std::vector<i32> order;
    if (m.reordered || m.recs.size() > m.picture) { // copies stand among the shapes of their groups
        std::vector<tvg::Paint*> all;
        tvg::Accessor* acc = tvg::Accessor::gen();
        acc->set(m.pic, [](const tvg::Paint* p, void* data) {
            static_cast<std::vector<tvg::Paint*>*>(data)->push_back(const_cast<tvg::Paint*>(p));
            return true;
        }, &all);
        delete acc;
        for (tvg::Paint* p : all)
            if (auto it = m.leaf_of.find(p); it != m.leaf_of.end()) order.push_back(it->second);
    } else {
        for (usize i = 0; i < m.leaves.size(); ++i) order.push_back(static_cast<i32>(i));
    }
    // Effects (part 4б): the blur stays in every pass; a part's shadow has a pass of its own before its first shape,
    // the part whole with only its shadow on, opaque; where it covers half a pixel, the shadow's colour.
    for (const PartRec& r : m.recs)
        if (r.fx_on) put_fx(r, FxMode::BlurOnly);
    std::vector<char> shadow_done(m.recs.size(), 0);
    auto shadow_pass = [&](PartRec& r) {
        put_fx(r, FxMode::ShadowOpaque);
        for (i32 li : r.leaves)
            if (const Leaf& l = m.leaves[static_cast<usize>(li)]; l.visible && !l.hidden) l.shape->visible(true);
        pass(false, r.fx.r, r.fx.g, r.fx.b);
        for (i32 li : r.leaves) m.leaves[static_cast<usize>(li)].shape->visible(false);
        put_fx(r, FxMode::BlurOnly);
    };

    for (Leaf& l : m.leaves) l.shape->visible(false);
    for (i32 li : order) {
        Leaf& l = m.leaves[static_cast<usize>(li)];
        std::vector<i32> chain; // the parts it is in, the outermost first
        for (i32 q = l.part; q >= 0; q = m.parts[static_cast<usize>(q)].parent) chain.insert(chain.begin(), q);
        for (i32 q : chain) {
            PartRec& r = m.recs[static_cast<usize>(q)];
            if (!r.fx_on || !r.fx.shadow || shadow_done[static_cast<usize>(q)]) continue;
            shadow_done[static_cast<usize>(q)] = 1;
            shadow_pass(r);
        }
        if (!l.visible || l.hidden) continue;
        tvg::Shape* s = l.shape;
        s->visible(true);
        u8 fr = 0, fg = 0, fb = 0, fa = 0, sr = 0, sg = 0, sb = 0, sa = 0;
        const tvg::Fill* fill_grad = s->fill();
        const tvg::Fill* stroke_grad = s->strokeFill();
        if (!fill_grad) s->fill(&fr, &fg, &fb, &fa);
        if (!stroke_grad) s->strokeFill(&sr, &sg, &sb, &sa);
        const f32 sw = s->strokeWidth();
        if (fill_grad || fa > 0) {
            if (sw > 0) s->strokeWidth(0);
            pass(fill_grad != nullptr, fr, fg, fb);
            if (sw > 0) s->strokeWidth(sw);
        }
        if (sw > 0 && (stroke_grad || sa > 0)) {
            tvg::Fill* kept = fill_grad ? fill_grad->duplicate() : nullptr;
            s->fill(0, 0, 0, 0);
            pass(stroke_grad != nullptr, sr, sg, sb);
            if (kept) s->fill(kept);
            else s->fill(fr, fg, fb, fa);
        }
        s->visible(false);
    }
    for (Leaf& l : m.leaves) l.shape->visible(l.visible && !l.hidden);
    for (const PartRec& r : m.recs)
        if (r.fx_on) put_fx(r, FxMode::Full);
    m.pic->transform(m.pic->transform());
}

void Rig::bounds(i32 part, u32 w, u32 h, f64 out[4]) { bounds(part, to_pixels(w, h), out); }

void Rig::bounds(i32 part, const Affine& px, f64 out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return;
    Box drawn, any;
    for (i32 li : m.recs[static_cast<usize>(part)].leaves) {
        const Leaf& l = m.leaves[static_cast<usize>(li)];
        const Affine g = px * m.global(l.shape);
        path_box(l.shape, g, any);
        if (l.visible && (l.rest.has_fill() || l.rest.has_stroke())) path_box(l.shape, g, drawn);
    }
    const Box& b = drawn.empty() ? any : drawn;
    if (b.empty()) return;
    out[0] = b.x0, out[1] = b.y0, out[2] = b.x1, out[3] = b.y1;
}

Affine Rig::carried(i32 part) const {
    const Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return {};
    const PartRec& r = m.recs[static_cast<usize>(part)];
    const vector::Point mv = m.moves.empty() ? vector::Point{} : m.moves[static_cast<usize>(part)];
    // The paint's parent as it stands against its rest, then the part's own move; its link's carry over all.
    return r.carry * m.global(r.paint->parent()) * inverse(r.parent_global) * translation(mv.x, mv.y);
}

Affine Rig::matrix(i32 part) const {
    const Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return {};
    return from_tvg(m.recs[static_cast<usize>(part)].paint->transform());
}

Affine Rig::rest_frame(i32 part) const {
    const Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return {};
    return m.recs[static_cast<usize>(part)].parent_global;
}

Affine Rig::rest_local(i32 part) const {
    const Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return {};
    return from_tvg(m.recs[static_cast<usize>(part)].rest_local);
}

std::vector<DrawnCopy> Rig::drawn_copies() const {
    const Impl& m = *impl_;
    std::vector<DrawnCopy> out;
    for (usize i = m.picture; i < m.parts.size(); ++i)
        if (m.drawn[i]) out.push_back({m.parts[i].id, static_cast<i32>(i)});
    return out;
}

i32 Rig::copy(std::string_view id) const {
    const i32 i = impl_->find(id);
    return i >= 0 && static_cast<usize>(i) >= impl_->picture ? i : -1;
}

bool Rig::siblings(i32 a, i32 b) const {
    const Impl& m = *impl_;
    if (a < 0 || b < 0 || static_cast<usize>(a) >= m.recs.size() || static_cast<usize>(b) >= m.recs.size()) return false;
    const PartRec& ra = m.recs[static_cast<usize>(a)];
    return ra.group >= 0 && ra.group == m.recs[static_cast<usize>(b)].group;
}

bool Rig::dashed(i32 part) const {
    const Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return false;
    for (i32 li : m.recs[static_cast<usize>(part)].leaves)
        if (!m.leaves[static_cast<usize>(li)].rest.dash.empty()) return true;
    return false;
}

bool Rig::has_shapes(i32 part) const {
    const Impl& m = *impl_;
    return part >= 0 && static_cast<usize>(part) < m.recs.size() && !m.recs[static_cast<usize>(part)].leaves.empty();
}

bool Rig::path_of(i32 part, PathLine& out, std::string& why) const {
    const Impl& m = *impl_;
    out = {};
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return why = "в рисунке нет такой части", false;
    const std::string& id = m.parts[static_cast<usize>(part)].id;
    out.id = id;
    const PartRec& r = m.recs[static_cast<usize>(part)];
    if (r.leaf < 0) return why = "путь «" + id + "» — группа, а нужна фигура", false;
    const Leaf& l = m.leaves[static_cast<usize>(r.leaf)];
    // One contour: a move that more lines follow, once.
    u32 contours = 0;
    for (usize i = 0; i < l.cmds.size(); ++i)
        if (l.cmds[i] == tvg::PathCommand::MoveTo && i + 1 < l.cmds.size() &&
            (l.cmds[i + 1] == tvg::PathCommand::LineTo || l.cmds[i + 1] == tvg::PathCommand::CubicTo))
            ++contours;
    if (contours > 1) return why = "у пути «" + id + "» больше одного контура: нужна одна линия", false;
    // Flat to within a hundred-thousandth of the picture.
    const f64 tol = std::max(1e-9, std::max(m.vb[2], m.vb[3]) * 1e-5);
    auto pt = [&](usize k) { return l.rest_global.apply({l.pts[k].x, l.pts[k].y}); };
    std::vector<vector::Point> line;
    usize k = 0;
    bool started = false;
    for (const tvg::PathCommand cmd : l.cmds) {
        switch (cmd) {
        case tvg::PathCommand::MoveTo:
            if (k >= l.pts.size()) break;
            if (!started) line.push_back(pt(k)), started = true;
            ++k;
            break;
        case tvg::PathCommand::LineTo:
            if (k >= l.pts.size()) break;
            line.push_back(pt(k++));
            break;
        case tvg::PathCommand::CubicTo:
            if (k + 3 > l.pts.size() || line.empty()) break;
            flatten_cubic(line.back(), pt(k), pt(k + 1), pt(k + 2), tol, 0, line);
            k += 3;
            break;
        case tvg::PathCommand::Close:
            if (!line.empty()) out.closed = true;
            break;
        }
    }
    if (out.closed && !line.empty()) line.push_back(line.front());
    // Without points on top of each other: each segment has a direction.
    for (const vector::Point& q : line)
        if (out.points.empty() || std::hypot(q.x - out.points.back().x, q.y - out.points.back().y) > 1e-12) out.points.push_back(q);
    out.lengths.assign(out.points.size(), 0);
    for (usize i = 1; i < out.points.size(); ++i) {
        const f64 dx = out.points[i].x - out.points[i - 1].x, dy = out.points[i].y - out.points[i - 1].y;
        out.lengths[i] = out.lengths[i - 1] + std::hypot(dx, dy);
        f64 a = std::atan2(dy, dx) * 180.0 / vector::trig::kPi;
        if (!out.angles.empty()) a = out.angles.back() + std::remainder(a - out.angles.back(), 360.0);
        out.angles.push_back(a);
    }
    if (!(out.length() > 0)) return why = "у пути «" + id + "» длина 0", false;
    return true;
}

bool parse_color(std::string_view text, Rgba8& out) {
    u8 c[3];
    if (!vector::parse_svg_color(text, c)) return false;
    out = {c[0], c[1], c[2], 255};
    return true;
}

bool parse_part_color(std::string_view text, Rgba8& out) {
    if (text.size() == 9 && text[0] == '#') {
        u8 c[4];
        for (int i = 0; i < 4; ++i) {
            int v = 0;
            for (int k = 1; k <= 2; ++k) {
                const char ch = text[static_cast<usize>(i * 2 + k)];
                const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (d < 0) return false;
                v = v * 16 + d;
            }
            c[i] = static_cast<u8>(v);
        }
        out = {c[0], c[1], c[2], c[3]};
        return true;
    }
    return parse_color(text, out);
}

std::string color_hex(const Rgba8& c) {
    char buf[12];
    if (c.a == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

} // namespace forge::pixel
