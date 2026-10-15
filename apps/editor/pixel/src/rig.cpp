#include "forge/pixel/rig.h"

#include "forge/assets/image_edit.h"
#include "forge/vector/morph.h"
#include "forge/vector/svg_shape.h"
#include "forge/vector/trig.h"

#include <thorvg.h>

#include <algorithm>
#include <cmath>
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

// A shape's paint as ThorVG holds it: a colour or a gradient (an own copy) for the fill and for the stroke.
struct Look {
    u8 fr = 0, fg = 0, fb = 0, fa = 0;
    std::unique_ptr<tvg::Fill> fill_grad;
    u8 sr = 0, sg = 0, sb = 0, sa = 0;
    std::unique_ptr<tvg::Fill> stroke_grad;
    f32 stroke_width = 0;
    u8 opacity = 255;

    void take(tvg::Shape* s) {
        fill_grad.reset(), stroke_grad.reset();
        if (const tvg::Fill* g = s->fill()) fill_grad.reset(g->duplicate());
        else s->fill(&fr, &fg, &fb, &fa);
        if (const tvg::Fill* g = s->strokeFill()) stroke_grad.reset(g->duplicate());
        else s->strokeFill(&sr, &sg, &sb, &sa);
        stroke_width = s->strokeWidth();
        opacity = s->opacity();
    }
    void put(tvg::Shape* s) const {
        if (fill_grad) s->fill(fill_grad->duplicate());
        else s->fill(fr, fg, fb, fa);
        if (stroke_grad) s->strokeFill(stroke_grad->duplicate());
        else if (stroke_width > 0 || sa > 0) s->strokeFill(sr, sg, sb, sa);
        s->strokeWidth(stroke_width);
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
    bool path_changed = false, look_changed = false;
};

struct PartRec {
    tvg::Paint* paint = nullptr;
    tvg::Matrix rest_local{};
    u8 rest_opacity = 255;
    Affine parent_global; // the rest matrix from the paint's parent to picture units
    std::vector<i32> leaves; // the shapes it draws, in order
    i32 leaf = -1;           // when it is a shape itself
};

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

} // namespace

struct Rig::Impl {
    Engine engine; // first: the last to go
    SvgOutline outline;
    tvg::Picture* pic = nullptr;
    tvg::Paint* doc = nullptr; // the scene of the SVG's own coordinates (picture units)
    Affine doc_matrix;         // doc's own matrix: picture units to ThorVG's viewport
    f64 vb[4] = {0, 0, 0, 0};  // the picture's area in its units: x, y, width, height
    std::vector<Leaf> leaves;
    std::unordered_map<const tvg::Paint*, i32> leaf_of;
    std::vector<Part> parts;
    std::vector<PartRec> recs;
    std::unordered_map<const tvg::Paint*, i32> part_of;
    std::vector<vector::Point> pivots;
    std::vector<vector::Point> moves; // each part's own move in the last pose, as applied (snapped)
    std::map<std::string, vector::Morph> morphs;
    vector::Shape scratch;

    tvg::SwCanvas* canvas = nullptr;
    std::vector<u32> buffer;
    u32 cw = 0, ch = 0;

    ~Impl() {
        if (canvas) {
            canvas->remove(pic);
            delete canvas;
        }
        if (pic) pic->unref();
    }

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
        r.parent_global = rest_global[p->parent()];
        if (auto l = m.leaf_of.find(p); l != m.leaf_of.end()) r.leaf = l->second;
    }
    for (usize i = 0; i < m.parts.size(); ++i)
        for (const tvg::Paint* q = m.recs[i].paint->parent(); q && q != m.doc; q = q->parent())
            if (auto it = m.part_of.find(q); it != m.part_of.end()) {
                m.parts[i].parent = it->second;
                break;
            }
    for (usize l = 0; l < m.leaves.size(); ++l)
        for (const tvg::Paint* q = m.leaves[l].shape; q && q != m.doc; q = q->parent())
            if (auto it = m.part_of.find(q); it != m.part_of.end()) m.recs[static_cast<usize>(it->second)].leaves.push_back(static_cast<i32>(l));

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
    return true;
}

bool Rig::loaded() const { return impl_ && impl_->pic != nullptr; }
const SvgOutline& Rig::outline() const { return impl_->outline; }
const std::vector<Part>& Rig::parts() const { return impl_->parts; }

i32 Rig::part(std::string_view id) const {
    for (usize i = 0; i < impl_->parts.size(); ++i)
        if (impl_->parts[i].id == id) return static_cast<i32>(i);
    return -1;
}

f64 Rig::width() const { return impl_->vb[2]; }
f64 Rig::height() const { return impl_->vb[3]; }

void Rig::set_pivot(std::string_view id, vector::Point at) {
    if (const i32 i = part(id); i >= 0) impl_->pivots[static_cast<usize>(i)] = at;
}

vector::Point Rig::pivot(i32 i) const {
    if (i < 0 || static_cast<usize>(i) >= impl_->pivots.size()) return {};
    return impl_->pivots[static_cast<usize>(i)];
}

bool Rig::pose(const Pose& pose, std::string& why, f64 snap) {
    Impl& m = *impl_;
    why.clear();
    if (!m.pic) {
        why = "SVG не загружен";
        return false;
    }
    auto fail = [&](std::string text) {
        if (why.empty()) why = std::move(text);
    };

    // Back to rest.
    m.moves.assign(m.recs.size(), {});
    for (PartRec& r : m.recs) {
        r.paint->transform(r.rest_local);
        r.paint->opacity(r.rest_opacity);
    }
    for (Leaf& l : m.leaves) {
        if (l.path_changed) {
            l.shape->reset();
            l.shape->appendPath(l.cmds.data(), static_cast<u32>(l.cmds.size()), l.pts.data(), static_cast<u32>(l.pts.size()));
            l.path_changed = false;
        }
        if (l.look_changed) {
            l.rest.put(l.shape);
            l.look_changed = false;
        }
    }

    // Morphs: the shape's path and look become the pair's at k.
    for (const auto& [id, morph] : pose.morphs) {
        const i32 pi = part(id);
        if (pi < 0) {
            fail("морфинг: в рисунке нет фигуры «" + id + "»");
            continue;
        }
        const PartRec& r = m.recs[static_cast<usize>(pi)];
        if (r.leaf < 0) {
            fail("морфинг: «" + id + "» — группа, морфинг бывает только у фигуры");
            continue;
        }
        const i32 ti = m.outline.find(morph.target);
        if (ti < 0) {
            fail("морфинг «" + id + "»: в рисунке нет фигуры «" + morph.target + "»");
            continue;
        }
        if (!std::isfinite(morph.k)) {
            fail("морфинг «" + id + "»: k — не число");
            continue;
        }
        const i32 si = m.parts[static_cast<usize>(pi)].element;
        if (transform_chain(m.outline, si) != transform_chain(m.outline, ti)) {
            fail("морфинг «" + id + "» в «" + morph.target + "»: у фигур разные transform, их точки не в одних координатах");
            continue;
        }
        Leaf& leaf = m.leaves[static_cast<usize>(r.leaf)];
        const std::string key = id + '\n' + morph.target;
        auto it = m.morphs.find(key);
        if (it == m.morphs.end()) {
            vector::Shape from, to;
            std::string err;
            if (!shape_path(m.outline, si, from.path, err)) {
                fail("морфинг «" + id + "»: " + err);
                continue;
            }
            if (!shape_path(m.outline, ti, to.path, err)) {
                fail("морфинг «" + id + "» в «" + morph.target + "»: " + err);
                continue;
            }
            from.look = leaf.rest.vector_look();
            const i32 tp = part(morph.target);
            const PartRec* tr = tp >= 0 ? &m.recs[static_cast<usize>(tp)] : nullptr;
            if (tr && tr->leaf >= 0 && !vector::display_none(m.outline, ti)) to.look = m.leaves[static_cast<usize>(tr->leaf)].rest.vector_look();
            else to.look = vector::look_from_svg(m.outline, ti, from.look);
            it = m.morphs.emplace(key, vector::prepare(from, to)).first;
        }
        vector::mix(it->second, morph.k, m.scratch);
        tvg::Shape* s = leaf.shape;
        s->reset();
        for (const vector::Contour& c : m.scratch.path.contours) {
            s->moveTo(static_cast<f32>(c.start.x), static_cast<f32>(c.start.y));
            for (const vector::Cubic& q : c.curves)
                s->cubicTo(static_cast<f32>(q.c1.x), static_cast<f32>(q.c1.y), static_cast<f32>(q.c2.x), static_cast<f32>(q.c2.y),
                           static_cast<f32>(q.to.x), static_cast<f32>(q.to.y));
            if (c.closed) s->close();
        }
        const vector::Look& look = m.scratch.look;
        if (!leaf.rest.fill_grad)
            s->fill(to_u8(look.fill.r), to_u8(look.fill.g), to_u8(look.fill.b), look.fill.on ? to_u8(look.fill.opacity) : 0);
        if (!leaf.rest.stroke_grad)
            s->strokeFill(to_u8(look.stroke.r), to_u8(look.stroke.g), to_u8(look.stroke.b), look.stroke.on ? to_u8(look.stroke.opacity) : 0);
        s->strokeWidth(static_cast<f32>(look.stroke.on ? look.stroke_width : 0));
        s->opacity(to_u8(look.opacity));
        leaf.path_changed = leaf.look_changed = true;
    }

    // Parts: moved, turned and scaled around their pivots in rest picture units, then their opacity and fill.
    for (const auto& [id, pp] : pose.parts) {
        const i32 pi = part(id);
        if (pi < 0) {
            fail("в рисунке нет части «" + id + "»");
            continue;
        }
        const f64 nums[] = {pp.x, pp.y, pp.rot, pp.scale, pp.sx, pp.sy, pp.opacity};
        if (!std::all_of(std::begin(nums), std::end(nums), [](f64 v) { return std::isfinite(v); })) {
            fail("часть «" + id + "»: не число в позе");
            continue;
        }
        PartRec& r = m.recs[static_cast<usize>(pi)];
        f64 x = pp.x, y = pp.y;
        if (snap > 0) x = std::round(x / snap) * snap, y = std::round(y / snap) * snap;
        m.moves[static_cast<usize>(pi)] = {x, y};
        const vector::Point pv = m.pivots[static_cast<usize>(pi)];
        f64 s = 0, c = 1;
        vector::trig::sin_cos(pp.rot * vector::trig::kPi / 180.0, s, c);
        const f64 kx = pp.scale * pp.sx, ky = pp.scale * pp.sy;
        // T(x, y) · T(pivot) · R · S · T(−pivot)
        const Affine rs{c * kx, s * kx, -s * ky, c * ky, 0, 0};
        Affine a = translation(x + pv.x, y + pv.y) * rs * translation(-pv.x, -pv.y);
        const Affine local = inverse(r.parent_global) * a * r.parent_global * from_tvg(r.rest_local);
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
    }
    m.pic->transform(m.pic->transform()); // ThorVG redraws a picture only when the picture itself changed
    return why.empty();
}

Affine Rig::to_pixels(u32 w, u32 h) const {
    const f64* vb = impl_->vb;
    if (vb[2] <= 0 || vb[3] <= 0) return {};
    const f64 s = std::min(w / vb[2], h / vb[3]);
    return {s, 0, 0, s, (w - vb[2] * s) / 2 - vb[0] * s, (h - vb[3] * s) / 2 - vb[1] * s};
}

assets::CookedTexture Rig::draw(u32 w, u32 h) {
    assets::CookedTexture out;
    Impl& m = *impl_;
    if (!m.pic || w == 0 || h == 0) return out;
    m.target(w, h);
    m.render(to_pixels(w, h));
    out.width = w, out.height = h;
    out.rgba8.assign(m.bytes(), m.bytes() + static_cast<usize>(w) * h * 4);
    return out;
}

assets::CookedTexture Rig::bake(u32 w, u32 h, const BakeOptions& options) {
    assets::CookedTexture out;
    Impl& m = *impl_;
    if (!m.pic || w == 0 || h == 0) return out;
    out.width = w, out.height = h;
    out.rgba8.assign(static_cast<usize>(w) * h * 4, 0);
    m.target(w, h);
    const Affine px = to_pixels(w, h);

    // One pass: what is drawn now, cut at half cover; a colour given, or each pixel's own (a gradient).
    auto pass = [&](bool own, u8 r, u8 g, u8 b) {
        m.render(px);
        const u8* src = m.bytes();
        u8* dst = out.rgba8.data();
        for (usize i = 0, n = static_cast<usize>(w) * h * 4; i < n; i += 4) {
            if (src[i + 3] < 128) continue;
            dst[i] = own ? src[i] : r;
            dst[i + 1] = own ? src[i + 1] : g;
            dst[i + 2] = own ? src[i + 2] : b;
            dst[i + 3] = 255;
        }
    };

    for (Leaf& l : m.leaves) l.shape->visible(false);
    for (Leaf& l : m.leaves) {
        if (!l.visible) continue;
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
    for (Leaf& l : m.leaves) l.shape->visible(l.visible);
    m.pic->transform(m.pic->transform());

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

void Rig::bounds(i32 part, u32 w, u32 h, f64 out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    Impl& m = *impl_;
    if (part < 0 || static_cast<usize>(part) >= m.recs.size()) return;
    const Affine px = to_pixels(w, h);
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
    // The paint's parent as it stands against its rest, then the part's own move.
    return m.global(r.paint->parent()) * inverse(r.parent_global) * translation(mv.x, mv.y);
}

bool parse_color(std::string_view text, Rgba8& out) {
    u8 c[3];
    if (!vector::parse_svg_color(text, c)) return false;
    out = {c[0], c[1], c[2], 255};
    return true;
}

} // namespace forge::pixel
