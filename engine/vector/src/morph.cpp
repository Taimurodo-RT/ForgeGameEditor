#include "forge/vector/morph.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace forge::vector {

namespace {

f64 lerp(f64 a, f64 b, f64 k) { return a * (1 - k) + b * k; } // exact at both ends

Point lerp(Point a, Point b, f64 k) { return {lerp(a.x, b.x, k), lerp(a.y, b.y, k)}; }

f64 dist2(Point a, Point b) {
    const f64 dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

f64 dist(Point a, Point b) { return std::sqrt(dist2(a, b)); }

Point end_of(const Contour& c) { return c.curves.empty() ? c.start : c.curves.back().to; }

// The other side's stand-in for a contour it lacks: every point at that contour's centre, as many curves.
Contour point_like(const Contour& c) {
    Point lo = c.start, hi = c.start;
    auto grow = [&](Point p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    };
    for (const Cubic& q : c.curves) {
        grow(q.c1);
        grow(q.c2);
        grow(q.to);
    }
    const Point centre{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2};
    Contour out;
    out.start = centre;
    out.curves.assign(c.curves.size(), Cubic{centre, centre, centre});
    out.closed = c.closed;
    return out;
}

void reverse(Contour& c) {
    if (c.curves.empty()) return;
    std::vector<Cubic> back;
    back.reserve(c.curves.size());
    for (usize i = c.curves.size(); i-- > 0;) back.push_back({c.curves[i].c2, c.curves[i].c1, c.from(i)});
    c.start = end_of(c);
    c.curves = std::move(back);
}

// How long a curve is, near enough to share pieces out: the mean of its chord and its control polygon.
f64 length(Point from, const Cubic& q) {
    return (dist(from, q.to) + dist(from, q.c1) + dist(q.c1, q.c2) + dist(q.c2, q.to)) / 2;
}

// Cuts curves into pieces until the contour has `count` of them. Each new piece goes to the curve whose pieces are
// the longest now (ties: the first), and a curve is cut into equal steps of t.
void cut_to(Contour& c, usize count) {
    const usize n = c.curves.size();
    if (n == 0 || count <= n) return;
    std::vector<f64> len(n);
    for (usize i = 0; i < n; ++i) len[i] = length(c.from(i), c.curves[i]);
    std::vector<usize> pieces(n, 1);
    for (usize added = n; added < count; ++added) {
        usize best = 0;
        for (usize i = 1; i < n; ++i)
            if (len[i] * static_cast<f64>(pieces[best]) > len[best] * static_cast<f64>(pieces[i])) best = i;
        ++pieces[best];
    }
    std::vector<Cubic> out;
    out.reserve(count);
    for (usize i = 0; i < n; ++i) {
        Point from = c.from(i);
        Cubic rest = c.curves[i];
        for (usize j = pieces[i]; j > 1; --j) {
            Cubic first, second;
            split(from, rest, 1.0 / static_cast<f64>(j), first, second);
            out.push_back(first);
            from = first.to;
            rest = second;
        }
        out.push_back(rest);
    }
    c.curves = std::move(out);
}

// How far apart two contours of as many curves are when b starts at its curve r: all their points, squared.
f64 cost(const Contour& a, const Contour& b, usize r) {
    const usize n = a.curves.size();
    f64 sum = 0;
    for (usize i = 0; i < n; ++i) {
        const Cubic& p = a.curves[i];
        const Cubic& q = b.curves[(i + r) % n];
        sum += dist2(p.c1, q.c1) + dist2(p.c2, q.c2) + dist2(p.to, q.to);
    }
    return sum;
}

usize best_start(const Contour& a, const Contour& b, f64& best_cost) {
    usize best = 0;
    best_cost = cost(a, b, 0);
    for (usize r = 1; r < b.curves.size(); ++r) {
        const f64 c = cost(a, b, r);
        if (c < best_cost) best_cost = c, best = r;
    }
    return best;
}

void start_at(Contour& c, usize r) {
    if (r == 0) return;
    c.start = c.curves[r - 1].to;
    std::rotate(c.curves.begin(), c.curves.begin() + static_cast<std::ptrdiff_t>(r), c.curves.end());
}

// A contour without curves (a lone point) facing one with them: as many curves, all at that point.
void fill_point(Contour& c, const Contour& other) {
    if (c.curves.empty()) c.curves.assign(other.curves.size(), Cubic{c.start, c.start, c.start});
}

void pair_up(Contour& a, Contour& b) {
    fill_point(a, b);
    fill_point(b, a);
    const bool both_closed = a.closed && b.closed;
    if (both_closed) {
        const f64 fa = area(a), fb = area(b);
        if (fa != 0 && fb != 0 && (fa > 0) != (fb > 0)) reverse(b);
    } else {
        const Point a0 = a.start, a1 = end_of(a), b0 = b.start, b1 = end_of(b);
        if (dist2(a0, b1) + dist2(a1, b0) < dist2(a0, b0) + dist2(a1, b1)) reverse(b);
    }
    if (a.curves.size() < b.curves.size()) cut_to(a, b.curves.size());
    if (b.curves.size() < a.curves.size()) cut_to(b, a.curves.size());
    if (!both_closed || b.curves.empty()) return;
    f64 c_same;
    const usize r = best_start(a, b, c_same);
    if (area(a) == 0 || area(b) == 0) { // a flat contour has no way round of its own: the nearer of both ways
        Contour turned = b;
        reverse(turned);
        f64 c_turned;
        const usize rt = best_start(a, turned, c_turned);
        if (c_turned < c_same) {
            b = std::move(turned);
            start_at(b, rt);
            return;
        }
    }
    start_at(b, r);
}

void close_up(Path& p) {
    for (Contour& c : p.contours)
        if (c.closed && !c.curves.empty() && !(end_of(c) == c.start)) c.curves.push_back(line(end_of(c), c.start));
}

Paint mix_paint(const Paint& a, const Paint& b, f64 k) {
    if (k <= 0) return a;
    if (k >= 1) return b;
    if (!a.on && !b.on) return a;
    const Paint pa = a.on ? a : Paint{true, b.r, b.g, b.b, 0};
    const Paint pb = b.on ? b : Paint{true, a.r, a.g, a.b, 0};
    return {true, lerp(pa.r, pb.r, k), lerp(pa.g, pb.g, k), lerp(pa.b, pb.b, k), lerp(pa.opacity, pb.opacity, k)};
}

u8 channel(f64 v) { return static_cast<u8>(std::floor(std::clamp(v, 0.0, 1.0) * 255 + 0.5)); }

} // namespace

Paint rgb8(u8 r, u8 g, u8 b, f64 opacity) { return {true, r / 255.0, g / 255.0, b / 255.0, opacity}; }

std::string hex(const Paint& p) {
    if (!p.on) return "none";
    static const char kDigits[] = "0123456789abcdef";
    std::string s = "#";
    for (f64 v : {p.r, p.g, p.b}) {
        const u8 c = channel(v);
        s += kDigits[c >> 4];
        s += kDigits[c & 15];
    }
    return s;
}

Morph prepare(const Shape& from, const Shape& to) {
    Morph m{from, to};
    close_up(m.from.path);
    close_up(m.to.path);
    auto& a = m.from.path.contours;
    auto& b = m.to.path.contours;
    while (a.size() < b.size()) a.push_back(point_like(b[a.size()]));
    while (b.size() < a.size()) b.push_back(point_like(a[b.size()]));
    for (usize i = 0; i < a.size(); ++i) pair_up(a[i], b[i]);
    return m;
}

void mix(const Morph& m, f64 k, Shape& out) {
    const auto& a = m.from.path.contours;
    const auto& b = m.to.path.contours;
    // A pair from prepare() has as many of each on both sides; anything else mixes only what both have.
    out.path.contours.resize(std::min(a.size(), b.size()));
    for (usize i = 0; i < out.path.contours.size(); ++i) {
        const Contour& ca = a[i];
        const Contour& cb = b[i];
        Contour& c = out.path.contours[i];
        c.start = lerp(ca.start, cb.start, k);
        c.closed = k < 0.5 ? ca.closed : cb.closed;
        c.curves.resize(std::min(ca.curves.size(), cb.curves.size()));
        for (usize j = 0; j < c.curves.size(); ++j) {
            const Cubic& p = ca.curves[j];
            const Cubic& q = cb.curves[j];
            c.curves[j] = {lerp(p.c1, q.c1, k), lerp(p.c2, q.c2, k), lerp(p.to, q.to, k)};
        }
    }
    const f64 kc = std::clamp(k, 0.0, 1.0);
    const Look& la = m.from.look;
    const Look& lb = m.to.look;
    out.look.fill = mix_paint(la.fill, lb.fill, kc);
    out.look.stroke = mix_paint(la.stroke, lb.stroke, kc);
    // A stroke that is off on one side takes the other side's width there: it only fades.
    const f64 wa = la.stroke.on || !lb.stroke.on ? la.stroke_width : lb.stroke_width;
    const f64 wb = lb.stroke.on || !la.stroke.on ? lb.stroke_width : la.stroke_width;
    out.look.stroke_width = kc <= 0 ? la.stroke_width : kc >= 1 ? lb.stroke_width : lerp(wa, wb, kc);
    out.look.opacity = lerp(la.opacity, lb.opacity, kc);
}

Shape mix(const Morph& m, f64 k) {
    Shape out;
    mix(m, k, out);
    return out;
}

} // namespace forge::vector
