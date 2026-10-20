#include "forge/vector/path.h"

#include "forge/vector/trig.h"

#include <cfloat>
#include <charconv>
#include <cmath>

namespace forge::vector {

namespace {

Point mix(Point a, Point b, f64 t) { return {a.x * (1 - t) + b.x * t, a.y * (1 - t) + b.y * t}; }

// 4/3 (√2 − 1): the control points of a quarter of a circle of radius 1 are this far along the tangents.
const f64 kKappa = 4.0 / 3.0 * (std::sqrt(2.0) - 1);

// A quarter of an ellipse from `from` to `to` around the corner `corner` (the point where their tangents meet).
Cubic quarter(Point from, Point corner, Point to) {
    return {{from.x + (corner.x - from.x) * kKappa, from.y + (corner.y - from.y) * kKappa},
            {to.x + (corner.x - to.x) * kKappa, to.y + (corner.y - to.y) * kKappa},
            to};
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Numbers and flags of SVG's path grammar, one after another.
struct Reader {
    std::string_view s;
    usize i = 0;

    bool done() const { return i >= s.size(); }
    void skip_space() {
        while (!done() && is_space(s[i])) ++i;
    }
    // Spaces, at most one comma, spaces.
    void skip_separator() {
        skip_space();
        if (!done() && s[i] == ',') {
            ++i;
            skip_space();
        }
    }
    bool at_number() const {
        return !done() && (is_digit(s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '+');
    }
    // A number: [+-] digits [. digits] [e [+-] digits], at least one digit before or after the point. 1.5.5 is 1.5
    // and .5, 1-2 is 1 and −2.
    bool number(f64& out) {
        const usize begin = i;
        usize j = i;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        usize digits = 0;
        while (j < s.size() && is_digit(s[j])) ++j, ++digits;
        if (j < s.size() && s[j] == '.') {
            ++j;
            while (j < s.size() && is_digit(s[j])) ++j, ++digits;
        }
        if (digits == 0) return false;
        if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
            usize k = j + 1;
            if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
            if (k < s.size() && is_digit(s[k])) {
                while (k < s.size() && is_digit(s[k])) ++k;
                j = k;
            }
        }
        const usize from = s[begin] == '+' ? begin + 1 : begin; // from_chars takes no plus sign
        const auto r = std::from_chars(s.data() + from, s.data() + j, out);
        if (r.ec != std::errc() || r.ptr != s.data() + j) return false;
        i = j;
        return true;
    }
    // An arc's flag: a single 0 or 1, which may run straight into the next number ("a1 1 0 00 1 1").
    bool flag(bool& out) {
        if (done() || (s[i] != '0' && s[i] != '1')) return false;
        out = s[i] == '1';
        ++i;
        return true;
    }
};

void set_why(std::string* why, const std::string& text, usize at) {
    if (why) *why = text + " (знак " + std::to_string(at + 1) + ")";
}

// An SVG arc from p0 to p1 as cubics of a quarter turn or less (SVG 2, appendix B.2.4: from the ends to the centre).
void arc(std::vector<Cubic>& out, Point p0, f64 rx, f64 ry, f64 degrees, bool large, bool sweep, Point p1) {
    if (p0 == p1) return;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0 || ry == 0) {
        out.push_back(line(p0, p1));
        return;
    }
    f64 sp, cp;
    trig::sin_cos(degrees * (trig::kPi / 180), sp, cp);
    const f64 dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
    const f64 x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    // Radii too small to reach: grown evenly until they just do.
    const f64 lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
        const f64 g = std::sqrt(lambda);
        rx *= g;
        ry *= g;
    }
    const f64 rx2 = rx * rx, ry2 = ry * ry;
    const f64 den = rx2 * y1 * y1 + ry2 * x1 * x1;
    const f64 num = rx2 * ry2 - den;
    f64 coef = den > 0 && num > 0 ? std::sqrt(num / den) : 0;
    if (large == sweep) coef = -coef;
    const f64 cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
    const f64 cx = cp * cx1 - sp * cy1 + (p0.x + p1.x) / 2, cy = sp * cx1 + cp * cy1 + (p0.y + p1.y) / 2;
    const f64 ux = (x1 - cx1) / rx, uy = (y1 - cy1) / ry, vx = (-x1 - cx1) / rx, vy = (-y1 - cy1) / ry;
    const f64 theta = trig::atan2(uy, ux);
    f64 delta = trig::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0) delta -= 2 * trig::kPi;
    if (sweep && delta < 0) delta += 2 * trig::kPi;
    // Pieces of at most a quarter turn; a hair over one (rounding) does not make another.
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (trig::kPi / 2) - 1e-9)));
    const f64 step = delta / n;
    f64 s4, c4;
    trig::sin_cos(step / 4, s4, c4);
    const f64 k = 4.0 / 3.0 * s4 / c4; // 4/3 tan(step / 4)
    // A point of the unit circle onto the ellipse.
    auto place = [&](f64 ux_, f64 uy_) {
        return Point{cp * rx * ux_ - sp * ry * uy_ + cx, sp * rx * ux_ + cp * ry * uy_ + cy};
    };
    f64 sa, ca;
    trig::sin_cos(theta, sa, ca);
    for (int i = 0; i < n; ++i) {
        f64 sb, cb;
        trig::sin_cos(theta + step * (i + 1), sb, cb);
        const Point c1 = place(ca - k * sa, sa + k * ca);
        const Point c2 = place(cb + k * sb, sb - k * cb);
        out.push_back({c1, c2, i == n - 1 ? p1 : place(cb, sb)});
        sa = sb;
        ca = cb;
    }
}

void write_number(std::string& out, f64 v) {
    if (v == 0) v = 0; // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

void write_point(std::string& out, Point p) {
    out += ' ';
    write_number(out, p.x);
    out += ' ';
    write_number(out, p.y);
}

} // namespace

Cubic line(Point from, Point to) { return {mix(from, to, 1.0 / 3), mix(from, to, 2.0 / 3), to}; }

Point point_at(Point from, const Cubic& c, f64 t) {
    const f64 u = 1 - t;
    const f64 b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    return {b0 * from.x + b1 * c.c1.x + b2 * c.c2.x + b3 * c.to.x,
            b0 * from.y + b1 * c.c1.y + b2 * c.c2.y + b3 * c.to.y};
}

void split(Point from, const Cubic& c, f64 t, Cubic& first, Cubic& second) {
    const Point p01 = mix(from, c.c1, t), p12 = mix(c.c1, c.c2, t), p23 = mix(c.c2, c.to, t);
    const Point p012 = mix(p01, p12, t), p123 = mix(p12, p23, t);
    const Point m = mix(p012, p123, t);
    first = {p01, p012, m};
    second = {p123, p23, c.to};
}

f64 area(const Contour& c) {
    // Green's theorem on each curve: ½ ∮ (x dy − y dx), in closed form for a cubic (as Paper.js's Curve.getArea).
    f64 sum = 0;
    for (usize i = 0; i < c.curves.size(); ++i) {
        const Point p0 = c.from(i);
        const Cubic& q = c.curves[i];
        const f64 x0 = p0.x, y0 = p0.y, x1 = q.c1.x, y1 = q.c1.y, x2 = q.c2.x, y2 = q.c2.y, x3 = q.to.x, y3 = q.to.y;
        sum += 3 *
               ((y3 - y0) * (x1 + x2) - (x3 - x0) * (y1 + y2) + y1 * (x0 - x2) - x1 * (y0 - y2) + y3 * (x2 + x0 / 3) -
                x3 * (y2 + y0 / 3)) /
               20;
    }
    if (!c.curves.empty()) { // the straight way back of an open contour (0 for a closed one)
        const Point a = c.curves.back().to, b = c.start;
        sum += (a.x * b.y - b.x * a.y) / 2;
    }
    return sum;
}

namespace {

// ThorVG's trimming (src/renderer/tvgRender.cpp, RenderTrimPath; src/common/tvgMath.cpp), in its own single precision
// and order of operations.
struct TP {
    f32 x = 0, y = 0;
};
enum class TCmd : u8 { Move, Line, Cubic, Close };
struct TPath {
    std::vector<TCmd> cmds;
    std::vector<TP> pts;
};

constexpr f32 kTrimEpsilon = 1e-4f;
constexpr f32 kBezierEpsilon = 1e-2f;

f32 exact_length(TP a, TP b) {
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}
// tvg::length(Point, Point): alpha max plus beta min.
f32 quick_length(TP a, TP b) {
    f32 x = b.x - a.x, y = b.y - a.y;
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    return x > y ? x + 0.375f * y : y + 0.375f * x;
}

struct TLine {
    TP pt1, pt2;
    void split(f32 at, TLine& left, TLine& right) const {
        const f32 len = exact_length(pt1, pt2);
        const f32 dx = ((pt2.x - pt1.x) / len) * at, dy = ((pt2.y - pt1.y) / len) * at;
        left.pt1 = pt1;
        left.pt2 = {left.pt1.x + dx, left.pt1.y + dy};
        right.pt1 = left.pt2;
        right.pt2 = pt2;
    }
};

struct TBez {
    TP start, ctrl1, ctrl2, end;
    void halve(TBez& left, TBez& right) const {
        f32 c = (ctrl1.x + ctrl2.x) * 0.5f;
        left.ctrl1.x = (start.x + ctrl1.x) * 0.5f;
        right.ctrl2.x = (ctrl2.x + end.x) * 0.5f;
        left.start.x = start.x;
        right.end.x = end.x;
        left.ctrl2.x = (left.ctrl1.x + c) * 0.5f;
        right.ctrl1.x = (right.ctrl2.x + c) * 0.5f;
        left.end.x = right.start.x = (left.ctrl2.x + right.ctrl1.x) * 0.5f;
        c = (ctrl1.y + ctrl2.y) * 0.5f;
        left.ctrl1.y = (start.y + ctrl1.y) * 0.5f;
        right.ctrl2.y = (ctrl2.y + end.y) * 0.5f;
        left.start.y = start.y;
        right.end.y = end.y;
        left.ctrl2.y = (left.ctrl1.y + c) * 0.5f;
        right.ctrl1.y = (right.ctrl2.y + c) * 0.5f;
        left.end.y = right.start.y = (left.ctrl2.y + right.ctrl1.y) * 0.5f;
    }
    f32 length() const {
        const f32 len = exact_length(start, ctrl1) + exact_length(ctrl1, ctrl2) + exact_length(ctrl2, end);
        const f32 chord = exact_length(start, end);
        if (std::fabs(len - chord) > kBezierEpsilon) {
            TBez left, right;
            halve(left, right);
            return left.length() + right.length();
        }
        return len;
    }
    // Cuts at t: this becomes the part after it, `left` the part before.
    void split(f32 t, TBez& left) {
        auto lerp = [t](TP a, TP b) { return TP{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)}; };
        left.start = start;
        left.ctrl1 = lerp(start, ctrl1);
        left.ctrl2 = lerp(ctrl1, ctrl2); // a temporary holding spot
        ctrl2 = lerp(ctrl2, end);
        ctrl1 = lerp(left.ctrl2, ctrl2);
        left.ctrl2 = lerp(left.ctrl1, left.ctrl2);
        left.end = start = lerp(left.ctrl2, ctrl1);
    }
    // The t where the length from its start is `want`, by halving (Bezier::at).
    f32 t_at(f32 want, f32 length) const {
        f32 biggest = 1.0f, smallest = 0.0f, t = 0.5f;
        if (want <= 0) return 0.0f;
        if (want >= length) return 1.0f;
        while (true) {
            TBez right = *this, left;
            right.split(t, left);
            length = left.length();
            if (std::fabs(length - want) < kBezierEpsilon || std::fabs(smallest - biggest) < 1e-3f) break;
            if (length < want) {
                smallest = t;
                t = (t + biggest) * 0.5f;
            } else {
                biggest = t;
                t = (smallest + t) * 0.5f;
            }
        }
        return t;
    }
    // Cuts at the length `at` from its start.
    void split(f32 at, TBez& left, TBez& right) const {
        right = *this;
        const f32 t = right.t_at(at, right.length());
        right.split(t, left);
    }
};

void trim_at(const TCmd* cmd, const TP* pts, TP& move_to, f32 at1, f32 at2, bool start, TPath& out) {
    switch (*cmd) {
    case TCmd::Line:
    case TCmd::Close: {
        TLine tmp, left, right;
        TLine{*(pts - 1), *cmd == TCmd::Line ? *pts : move_to}.split(at1, left, tmp);
        tmp.split(at2, left, right);
        if (start) {
            out.pts.push_back(left.pt1);
            move_to = left.pt1;
            out.cmds.push_back(TCmd::Move);
        }
        out.pts.push_back(left.pt2);
        out.cmds.push_back(TCmd::Line);
        break;
    }
    case TCmd::Cubic: {
        TBez tmp, left, right;
        TBez{*(pts - 1), *pts, *(pts + 1), *(pts + 2)}.split(at1, left, tmp);
        tmp.split(at2, left, right);
        if (start) {
            move_to = left.start;
            out.pts.push_back(left.start);
            out.cmds.push_back(TCmd::Move);
        }
        out.pts.insert(out.pts.end(), {left.ctrl1, left.ctrl2, left.end});
        out.cmds.push_back(TCmd::Cubic);
        break;
    }
    case TCmd::Move: break;
    }
}

void trim_add(const TCmd* cmd, const TP* pts, TP move_to, bool& start, TPath& out) {
    switch (*cmd) {
    case TCmd::Move:
        out.cmds.push_back(TCmd::Move);
        out.pts.push_back(*pts);
        start = false;
        break;
    case TCmd::Line:
    case TCmd::Cubic:
        if (start) {
            out.cmds.push_back(TCmd::Move);
            out.pts.push_back(*(pts - 1));
        }
        out.cmds.push_back(*cmd);
        out.pts.insert(out.pts.end(), pts, pts + (*cmd == TCmd::Line ? 1 : 3));
        start = false;
        break;
    case TCmd::Close:
        if (start) {
            out.cmds.push_back(TCmd::Move);
            out.pts.push_back(*(pts - 1));
        }
        out.cmds.push_back(TCmd::Line);
        out.pts.push_back(move_to);
        start = true;
        break;
    }
}

f32 trim_total(const TPath& in) {
    f32 length = 0;
    const TP* pts = in.pts.data();
    const TP* start = pts;
    for (TCmd c : in.cmds) {
        switch (c) {
        case TCmd::Close: length += quick_length(*(pts - 1), *start); break;
        case TCmd::Move: start = pts++; break;
        case TCmd::Line:
            length += quick_length(*(pts - 1), *pts);
            ++pts;
            break;
        case TCmd::Cubic:
            length += TBez{*(pts - 1), *pts, *(pts + 1), *(pts + 2)}.length();
            pts += 3;
            break;
        }
    }
    return length;
}

void trim_path(const TPath& in, f32 trim_start, f32 trim_end, TPath& out) {
    const TCmd* cmds = in.cmds.data();
    const TP* pts = in.pts.data();
    TP move_to_trimmed = *pts, move_to = *pts;
    f32 len = 0;
    auto seg_length = [&]() -> f32 {
        switch (*cmds) {
        case TCmd::Move: return 0.0f;
        case TCmd::Line: return quick_length(*(pts - 1), *pts);
        case TCmd::Cubic: return TBez{*(pts - 1), *pts, *(pts + 1), *(pts + 2)}.length();
        case TCmd::Close: return quick_length(*(pts - 1), move_to);
        }
        return 0.0f;
    };
    auto shift = [&]() {
        switch (*cmds) {
        case TCmd::Move:
            move_to = move_to_trimmed = *pts;
            ++pts;
            break;
        case TCmd::Line: ++pts; break;
        case TCmd::Cubic: pts += 3; break;
        case TCmd::Close: break;
        }
        ++cmds;
    };
    bool start = true;
    for (usize i = 0; i < in.cmds.size(); ++i) {
        const f32 d_len = seg_length();
        if (len <= trim_start) {
            if (len + d_len > trim_end) {
                trim_at(cmds, pts, move_to_trimmed, trim_start - len, trim_end - trim_start, start, out);
                start = false;
            } else if (len + d_len > trim_start + kTrimEpsilon) {
                trim_at(cmds, pts, move_to_trimmed, trim_start - len, len + d_len - trim_start, start, out);
                start = false;
            }
        } else if (len <= trim_end - kTrimEpsilon) {
            if (len + d_len > trim_end) {
                trim_at(cmds, pts, move_to, 0.0f, trim_end - len, start, out);
                start = true;
            } else if (len + d_len > trim_start + kTrimEpsilon) {
                trim_add(cmds, pts, move_to, start, out);
            }
        }
        len += d_len;
        shift();
    }
}

bool is_line(Point from, const Cubic& c) {
    const Cubic l = line(from, c.to);
    return l.c1 == c.c1 && l.c2 == c.c2;
}

TP tp(Point p) { return {static_cast<f32>(p.x), static_cast<f32>(p.y)}; }
Point pt(TP p) { return {p.x, p.y}; }

} // namespace

Path trim(const Path& path, f64 from, f64 to, bool lines) {
    Path out;
    if (!(from >= 0 && to <= 1 && from < to)) return out;
    // The path as ThorVG holds it: a closed contour's last line is its Close.
    TPath in;
    for (const Contour& c : path.contours) {
        in.cmds.push_back(TCmd::Move);
        in.pts.push_back(tp(c.start));
        for (usize i = 0; i < c.curves.size(); ++i) {
            const Cubic& q = c.curves[i];
            const bool straight = lines && is_line(c.from(i), q);
            if (straight && c.closed && i + 1 == c.curves.size()) break;
            in.cmds.push_back(straight ? TCmd::Line : TCmd::Cubic);
            if (!straight) in.pts.insert(in.pts.end(), {tp(q.c1), tp(q.c2)});
            in.pts.push_back(tp(q.to));
        }
        if (c.closed) in.cmds.push_back(TCmd::Close);
    }
    const f32 begin = static_cast<f32>(from), end = static_cast<f32>(to);
    if (in.pts.size() < 2 || std::fabs(begin - end) < FLT_EPSILON) return out;
    const f32 total = trim_total(in);
    TPath cut;
    trim_path(in, begin * total, end * total, cut);
    if (cut.pts.size() < 2) return out;
    const TP* p = cut.pts.data();
    for (TCmd c : cut.cmds) {
        switch (c) {
        case TCmd::Move: out.contours.push_back({pt(*p++), {}, false}); break;
        case TCmd::Line:
            out.contours.back().curves.push_back(line(out.contours.back().from(out.contours.back().curves.size()), pt(*p)));
            ++p;
            break;
        case TCmd::Cubic:
            out.contours.back().curves.push_back({pt(p[0]), pt(p[1]), pt(p[2])});
            p += 3;
            break;
        case TCmd::Close: break; // trimming makes none
        }
    }
    return out;
}

bool parse_path(std::string_view d, Path& out, std::string* why) {
    out.contours.clear();
    Reader r{d};
    Contour cur;
    bool open = false;       // cur has begun (an M, or a drawing command after Z)
    Point pos, start;        // the current point; where the contour began
    Point last_c2, last_q;   // for S and T: the control point to mirror
    char prev = 0;           // the command before, for S and T
    char cmd = 0;

    auto finish = [&] {
        if (open && !cur.curves.empty()) out.contours.push_back(std::move(cur));
        cur = Contour{};
        open = false;
    };
    auto begin_at = [&](Point p) {
        finish();
        cur.start = p;
        start = p;
        pos = p;
        open = true;
    };
    auto add = [&](const Cubic& c) {
        if (!open) begin_at(pos); // drawing straight after Z: from the same start (SVG)
        cur.curves.push_back(c);
        pos = c.to;
    };

    r.skip_space();
    while (!r.done()) {
        const usize at = r.i;
        const char c = r.s[r.i];
        if (std::string_view("MmLlHhVvCcSsQqTtAaZz").find(c) != std::string_view::npos) {
            cmd = c;
            ++r.i;
        } else if (cmd != 0 && cmd != 'Z' && cmd != 'z' && r.at_number()) {
            // More numbers for the same command; after M they are lines (SVG).
            if (cmd == 'M') cmd = 'L';
            if (cmd == 'm') cmd = 'l';
        } else {
            set_why(why, std::string("Неожиданный знак «") + c + "» в пути", at);
            finish();
            return false;
        }
        if (out.contours.empty() && !open && cmd != 'M' && cmd != 'm' && prev == 0) {
            set_why(why, "Путь должен начинаться с M", at);
            return false;
        }
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = rel ? static_cast<char>(cmd - 'a' + 'A') : cmd;
        auto need = [&](f64* v, int count) {
            for (int k = 0; k < count; ++k) {
                r.skip_separator();
                if (!r.number(v[k])) return false;
            }
            return true;
        };
        auto abs_point = [&](f64 x, f64 y) { return rel ? Point{pos.x + x, pos.y + y} : Point{x, y}; };
        const char* missing = "Не хватает чисел у команды ";
        if (up == 'Z') {
            if (open) {
                if (!(pos == start)) add(line(pos, start));
                if (!cur.curves.empty()) cur.closed = true;
                finish();
            }
            pos = start;
            prev = 'Z';
            r.skip_space();
            continue;
        }
        f64 v[7];
        switch (up) {
        case 'M': {
            if (!need(v, 2)) break;
            begin_at(abs_point(v[0], v[1]));
            prev = 'M';
            r.skip_separator();
            continue;
        }
        case 'L': {
            if (!need(v, 2)) break;
            add(line(pos, abs_point(v[0], v[1])));
            prev = 'L';
            r.skip_separator();
            continue;
        }
        case 'H': {
            if (!need(v, 1)) break;
            add(line(pos, {rel ? pos.x + v[0] : v[0], pos.y}));
            prev = 'H';
            r.skip_separator();
            continue;
        }
        case 'V': {
            if (!need(v, 1)) break;
            add(line(pos, {pos.x, rel ? pos.y + v[0] : v[0]}));
            prev = 'V';
            r.skip_separator();
            continue;
        }
        case 'C': {
            if (!need(v, 6)) break;
            const Cubic q{abs_point(v[0], v[1]), abs_point(v[2], v[3]), abs_point(v[4], v[5])};
            last_c2 = q.c2;
            add(q);
            prev = 'C';
            r.skip_separator();
            continue;
        }
        case 'S': {
            if (!need(v, 4)) break;
            const Point c1 = prev == 'C' || prev == 'S' ? Point{2 * pos.x - last_c2.x, 2 * pos.y - last_c2.y} : pos;
            const Cubic q{c1, abs_point(v[0], v[1]), abs_point(v[2], v[3])};
            last_c2 = q.c2;
            add(q);
            prev = 'S';
            r.skip_separator();
            continue;
        }
        case 'Q':
        case 'T': {
            Point q, to;
            if (up == 'Q') {
                if (!need(v, 4)) break;
                q = abs_point(v[0], v[1]);
                to = abs_point(v[2], v[3]);
            } else {
                if (!need(v, 2)) break;
                q = prev == 'Q' || prev == 'T' ? Point{2 * pos.x - last_q.x, 2 * pos.y - last_q.y} : pos;
                to = abs_point(v[0], v[1]);
            }
            // A quadratic curve is the cubic with control points two thirds of the way to its one.
            add({mix(pos, q, 2.0 / 3), mix(to, q, 2.0 / 3), to});
            last_q = q;
            prev = up;
            r.skip_separator();
            continue;
        }
        case 'A': {
            bool large = false, sweep = false;
            if (!need(v, 3)) break;
            r.skip_separator();
            if (!r.flag(large)) break;
            r.skip_separator();
            if (!r.flag(sweep)) break;
            if (!need(v + 3, 2)) break;
            const Point to = abs_point(v[3], v[4]);
            if (!open) begin_at(pos);
            arc(cur.curves, pos, v[0], v[1], v[2], large, sweep, to);
            pos = to;
            prev = 'A';
            r.skip_separator();
            continue;
        }
        default: break;
        }
        set_why(why, std::string(missing) + cmd, r.i);
        finish();
        return false;
    }
    finish();
    return true;
}

std::string to_d(const Path& path) { return to_d(path, false); }

std::string to_d(const Path& path, bool lines) {
    std::string out;
    for (const Contour& c : path.contours) {
        if (!out.empty()) out += ' ';
        out += 'M';
        write_point(out, c.start);
        for (usize i = 0; i < c.curves.size(); ++i) {
            const Cubic& q = c.curves[i];
            if (lines && is_line(c.from(i), q)) {
                out += " L";
            } else {
                out += " C";
                write_point(out, q.c1);
                write_point(out, q.c2);
            }
            write_point(out, q.to);
        }
        if (c.closed) out += " Z";
    }
    return out;
}

Path rect(f64 x, f64 y, f64 w, f64 h, f64 rx, f64 ry) {
    Path p;
    if (!(w > 0 && h > 0)) return p;
    rx = std::fmin(std::fabs(rx), w / 2);
    ry = std::fmin(std::fabs(ry), h / 2);
    if (rx == 0 || ry == 0) rx = ry = 0;
    // SVG 2: from (x + rx, y) right along the top, then clockwise; a side of no length is left out.
    Contour c;
    c.start = {x + rx, y};
    c.closed = true;
    Point pos = c.start;
    auto to_point = [&](Point p_) {
        if (!(p_ == pos)) c.curves.push_back(line(pos, p_));
        pos = p_;
    };
    auto corner = [&](Point corner_, Point to) {
        if (rx > 0) c.curves.push_back(quarter(pos, corner_, to));
        pos = to;
    };
    to_point({x + w - rx, y});
    corner({x + w, y}, {x + w, y + ry});
    to_point({x + w, y + h - ry});
    corner({x + w, y + h}, {x + w - rx, y + h});
    to_point({x + rx, y + h});
    corner({x, y + h}, {x, y + h - ry});
    to_point({x, y + ry});
    corner({x, y}, {x + rx, y});
    if (!(pos == c.start)) c.curves.push_back(line(pos, c.start));
    p.contours.push_back(std::move(c));
    return p;
}

Path ellipse(f64 cx, f64 cy, f64 rx, f64 ry) {
    Path p;
    if (!(rx > 0 && ry > 0)) return p;
    // SVG 2: from (cx + rx, cy) the positive way, which is clockwise on the screen.
    const Point e{cx + rx, cy}, s{cx, cy + ry}, w{cx - rx, cy}, n{cx, cy - ry};
    Contour c;
    c.start = e;
    c.closed = true;
    c.curves = {quarter(e, {cx + rx, cy + ry}, s), quarter(s, {cx - rx, cy + ry}, w), quarter(w, {cx - rx, cy - ry}, n),
                quarter(n, {cx + rx, cy - ry}, e)};
    p.contours.push_back(std::move(c));
    return p;
}

Path circle(f64 cx, f64 cy, f64 r) { return ellipse(cx, cy, r, r); }

Path polyline(std::span<const Point> points, bool closed) {
    Path p;
    if (points.size() < 2) return p;
    Contour c;
    c.start = points[0];
    c.closed = closed;
    for (usize i = 1; i < points.size(); ++i) c.curves.push_back(line(points[i - 1], points[i]));
    if (closed && !(points.back() == points[0])) c.curves.push_back(line(points.back(), points[0]));
    p.contours.push_back(std::move(c));
    return p;
}

bool parse_points(std::string_view points, std::vector<Point>& out, std::string* why) {
    out.clear();
    Reader r{points};
    std::vector<f64> v;
    r.skip_space();
    while (!r.done()) {
        f64 n;
        if (!r.number(n)) {
            set_why(why, std::string("Неожиданный знак «") + r.s[r.i] + "» в списке точек", r.i);
            break;
        }
        v.push_back(n);
        r.skip_separator();
    }
    for (usize i = 0; i + 1 < v.size(); i += 2) out.push_back({v[i], v[i + 1]});
    if (!r.done()) return false;
    if (v.size() % 2 != 0) {
        if (why) *why = "Нечётное число чисел в списке точек: последнее без пары";
        return false;
    }
    return true;
}

} // namespace forge::vector
