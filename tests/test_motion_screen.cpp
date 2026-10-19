// Parts 5в and 5г of docs/models/движение-модель.md («На экране игры», «Всё рисование на экране»): «Отдать экрану»
// writes a character's clips as a file a game's screen plays on its live SVG. The file reads back as written, refuses
// what it cannot be with the field named, and its values are the tab's frame: at each 120th of a second of every piece
// and in between for its holds and transitions, for every kind of block, with the copies and the order. A part's matrix
// is the rig's after the pose; the screen's picture (lunasvg with the attributes and the stage the game gives it) is the
// tab's (ThorVG) inside the shapes and in what it covers: dashes, drawn shares, tints, flashes, masks, links, copies and
// the order too.

#include "forge/pixel/bake.h"
#include "forge/pixel/draw.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/screen.h"
#include "forge/pixel/script.h"
#include "forge/ui/live_svg.h"
#include "forge/vector/motion_file.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;
using vector::MotionClip;
using vector::MotionFile;
using vector::MotionPiece;
using vector::MotionProp;
using vector::MotionTrack;
using vector::MotionValue;

namespace {

// A body moved by its group with an arm turned inside it holding a hand, a ball, a mouth with two hidden shapes to
// morph into, a hidden broken line to go along and a box.
const char* kPicture = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <g id="body" transform="translate(2 3)">
    <rect id="torso" x="22" y="20" width="14" height="18" fill="#3060c0" stroke="#102040" stroke-width="2"/>
    <g id="arm" transform="rotate(10 40 30)"><rect id="hand" x="38" y="27" width="6" height="7" fill="#c03030"/></g>
  </g>
  <circle id="ball" cx="10" cy="50" r="4" fill="#30a060"/>
  <path id="mouth" d="M 26 44 L 38 44 L 38 48 L 26 48 Z" fill="#402010"/>
  <path id="mouth_o" d="M 27 42 L 37 42 L 35 50 L 29 50 Z" fill="#802020" display="none"/>
  <path id="mouth_w" d="M 24 45 L 40 45 L 40 47 L 24 47 Z" fill="#204080" display="none"/>
  <path id="route" d="M 10 50 L 40 50 L 58 32" display="none"/>
  <rect id="box" x="48" y="6" width="10" height="8" fill="#e0e000"/>
</svg>)svg";

// Each timing word on the ball's x, one after another.
std::string words_clip() {
    std::string s = "local b = part(\"ball\")\n";
    for (usize i = 0; i < vector::kTimingCount; ++i)
        s += "b.x(" + std::string(i % 2 ? "0" : "20") + ", 0.1, \"" + vector::timing_word(static_cast<vector::Timing>(i)) + "\")\n";
    return s;
}

std::string code() {
    return "return {\n  clips = {\n    words = function(p)\n" + words_clip() + R"(    end,
    eased = function(p)
      local t = part("torso")
      all(t.fill("#ff0000", 0.3), t.stroke("#00ff00", 0.3), t.stroke_width(5, 0.3), part("body").rot(20, 0.3, "out_back"),
          part("ball").stroke("#ff00ff", 0.3))
      t.fill(nil, 0.2)
      part("ball").visible(false, 0.2)
      part("box").opacity(0.3, 0.2, bezier(0.3, 0, 0.2, 1))
      part("hand").x(4, 0.25, function(k) return k * k end)
      part("mouth"):morph("mouth_o", 0.2)
      part("mouth"):morph("mouth_w", 0.2)
      part("arm"):to({ skew_x = 15, skew_y = -5, sx = 1.2, sy = 0.8, scale = 1.1 }, 0.2)
      sound("шаг")
      emit("удар", 0.1)
    end,
    spring = function(p) part("ball").x:spring(10) end,
    tween = function(p) tween(0.5, function(k) part("ball").y(-10 * k * k) end) end,
    along = function(p) part("ball"):along("route", 1, "smooth", { turn = true }) end,
    arc = function(p) part("ball"):to({ x = 10, y = -10 }, 0.6, "linear", "arc") end,
    binding = function(p)
      local a = part("box")
      part("ball").x(function() return a.y() * 2 end)
      a.y(5, 0.5, "linear")
    end,
    back = function(p) part("box").y(6, 0.2):back(0.3) end,
    saved = function(p)
      local b = part("box")
      b:save()
      b:to({ x = -5, y = 3 }, 0.2)
      b:restore(0.1)
      b.x(4, 0.1)
      b.x:rest(0.1)
    end,
    start = function(p)
      start_from("eased")
      part("torso").fill("#00ffff", 0.2)
    end,
    cut = function(p)
      local b = part("ball")
      spawn(b.x(10, 1, "linear"))
      wait(0.3)
      b.x(9, 0.2, "linear")
    end,
    zero = function(p) part("ball"):to({ x = 5 }, 0) end,
    posed = function(p) part("box").y(4, 0.4) end,
    over = function(p) part("box").y(6, 0.4) end,
    still = function(p) part("ball"):to({ x = 0 }, 0.3) end,
    steep = function(p) part("arm"):to({ skew_x = 95, x = 2 }, 0.3, "linear") end,
  },
  pose = function(t, p, rng, clip)
    if clip == "posed" then part("box"):set{ x = 3 * math.sin(t * 7) } end
    if clip == "over" then part("box"):set{ y = 1 + 6 * t } end
  end,
}
)";
}

Rig loaded(std::string_view svg = kPicture) {
    Rig rig;
    std::string why;
    const bool ok = rig.load(svg, why);
    INFO(why);
    REQUIRE(ok);
    return rig;
}

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

PixelDoc doc64() {
    PixelDoc doc;
    doc.frame_w = doc.frame_h = 64;
    return doc;
}

f64 seconds(i64 ns) { return static_cast<f64>(ns) / 1e9; }

// The moments a piece is looked at, as the tab chose it: 120 times a second from its start, then the moment before
// its end.
std::vector<f64> piece_moments(f64 at, f64 end) {
    if (!(end > at)) return {at};
    const usize n = vector::motion_frame_count(at, end);
    std::vector<f64> out;
    for (usize i = 0; i + 1 < n; ++i) out.push_back(vector::motion_frame_moment(at, i));
    out.push_back(seconds(std::llround(end * 1e9) - 1));
    return out;
}

std::string said(MotionProp p, const MotionValue& v) {
    return std::string(vector::motion_prop_word(p)) + " " + std::to_string(v.v) + " #" + std::to_string(v.color.r) + "," +
           std::to_string(v.color.g) + "," + std::to_string(v.color.b) + "," + std::to_string(v.color.a) + " " + v.target + " " +
           std::to_string(v.n);
}

// A clip as the screen plays it, checked against the tab's frame: every piece at its moments, its holds and transitions
// between them too, every property no track has at rest; written and read back the same.
MotionClip checked(PoseScript& s, const Rig& rig, const PixelDoc& doc, const std::string& clip, MotionFile* file_out = nullptr) {
    MotionFile file;
    MotionClip c;
    std::string why;
    const bool ok = screen_clip(s, rig, doc, clip, file, c, why);
    INFO(why);
    REQUIRE(ok);
    auto tab = [&](f64 t) {
        Pose p;
        const bool posed = s.pose(rig, t, clip, doc.params, doc.seed, p);
        INFO(s.error());
        REQUIRE(posed);
        return p;
    };
    for (const MotionTrack& tr : c.tracks) {
        for (usize k = 0; k < tr.pieces.size(); ++k) {
            const MotionPiece& pc = tr.pieces[k];
            const f64 end = k + 1 < tr.pieces.size() ? tr.pieces[k + 1].at : c.length;
            std::vector<f64> ts = piece_moments(pc.at, end);
            if (pc.kind != MotionPiece::Kind::Frames) // a hold or a transition is the frame at any moment
                for (f64 t = pc.at + 1.0 / 337; t < end; t += 1.0 / 337) ts.push_back(t);
            for (const f64 t : ts) {
                const MotionValue game = vector::motion_track_value(file, tr, t, c.length);
                const MotionValue want = screen_value(tab(t), tr.part, tr.prop);
                INFO(clip, " ", tr.part, " piece ", k, " at ", t, ": ", said(tr.prop, game), " / ", said(tr.prop, want));
                CHECK(vector::motion_same(tr.prop, game, want));
            }
        }
    }
    // What no track has stays at rest, of the picture's parts and the copies; the copies drawn and the order are the
    // tab's (part 5г).
    std::vector<std::string> ids;
    for (const Part& part : rig.parts()) ids.push_back(part.id);
    for (const vector::MotionCopy& k : c.copies) ids.push_back(k.id);
    for (const f64 t : piece_moments(0, c.length)) {
        const Pose p = tab(t);
        for (const std::string& id : ids)
            for (usize i = 0; i < vector::kMotionPropCount; ++i) {
                const auto prop = static_cast<MotionProp>(i);
                if (std::any_of(c.tracks.begin(), c.tracks.end(), [&](const MotionTrack& tr) { return tr.part == id && tr.prop == prop; }))
                    continue;
                INFO(clip, " ", id, " ", vector::motion_prop_word(prop), " at ", t);
                CHECK(vector::motion_same(prop, screen_value(p, id, prop), vector::motion_rest(prop)));
            }
        std::vector<std::string> drawn, shown;
        for (const vector::MotionCopy& k : c.copies)
            if (vector::motion_copy_drawn(k, t)) drawn.push_back(k.id);
        for (const PartCopy& k : p.copies) shown.push_back(k.id);
        std::sort(drawn.begin(), drawn.end());
        std::sort(shown.begin(), shown.end());
        INFO(clip, " copies at ", t);
        CHECK(drawn == shown);
        std::vector<OrderStep> order;
        for (const vector::MotionOrderStep& st : vector::motion_order_at(c, t))
            order.push_back({st.part, static_cast<OrderStep::Kind>(st.kind), st.other});
        CHECK(order == p.order);
    }
    file.character = "Ёжик";
    file.from = "Ёжик.pixel.json";
    file.picture = "Ёжик.svg";
    file.clips.push_back(c);
    for (const MotionTrack& tr : c.tracks)
        if ((vector::motion_prop_moves(tr.prop) || tr.prop == MotionProp::Link) && !file.part(tr.part)) {
            vector::MotionPart part;
            part.id = tr.part;
            file.parts.push_back(part);
        }
    for (const vector::MotionCopy& k : c.copies)
        if (!file.part(k.id)) {
            vector::MotionPart part;
            part.id = k.id;
            file.parts.push_back(part);
        }
    const std::string text = vector::write_motion_file(file);
    MotionFile back;
    const bool read = vector::read_motion_file(text, back, why);
    INFO(why);
    REQUIRE(read);
    CHECK(back == file);
    CHECK(vector::write_motion_file(back) == text);
    if (file_out) *file_out = file;
    return c;
}

const MotionTrack* track_of(const MotionClip& c, std::string_view part, MotionProp p) {
    for (const MotionTrack& t : c.tracks)
        if (t.part == part && t.prop == p) return &t;
    return nullptr;
}

usize count_kind(const MotionClip& c, MotionPiece::Kind k) {
    usize n = 0;
    for (const MotionTrack& t : c.tracks)
        for (const MotionPiece& p : t.pieces) n += p.kind == k;
    return n;
}

std::string read_error(std::string_view json) {
    MotionFile f;
    std::string why;
    CHECK_FALSE(vector::read_motion_file(json, f, why));
    return why;
}

// A file with one clip of one track, the rest of it given.
std::string one_track(std::string_view track, std::string_view clip_rest = "", std::string_view parts = R"("parts": {"a": {"pivot": [0, 0], "frame": [1, 0, 0, 1, 0, 0], "own": [1, 0, 0, 1, 0, 0]}},)") {
    return R"({"forge": "motion", "version": 1, "character": "Ёжик", "from": "Ёжик.pixel.json", "picture": "Ёжик.svg", )" + std::string(parts) +
           R"( "curves": [], "clips": {"c": {"length": 1, "loop": false, "tracks": [)" + std::string(track) + "]" + std::string(clip_rest) +
           R"(, "marks": [], "not_drawn": []}}})";
}


// What a screen draws a clip from at t (GameScreens does the same): the parts' pose, the copies born, the order.
ui::SvgPose screen_pose(const MotionFile& f, const MotionClip& c, f64 t) {
    ui::SvgPose p{f.picture, vector::motion_pose(f, c, t), {}, vector::motion_order_at(c, t)};
    for (const vector::MotionCopy& k : c.copies)
        if (vector::motion_copy_drawn(k, t)) p.copies.push_back(k);
    return p;
}

// The screen's picture of a clip (lunasvg with the attributes and the stage the game gives it) against the tab's
// (ThorVG) at these moments: inside the shapes (a pixel and its neighbours fully covered, one colour in the tab) the
// colours within 2; what is covered at all within 1%.
void same_as_tab(const ScreenGift& gift, PoseScript& s, Rig& rig, const PixelDoc& doc, const char* name, std::vector<f64> times,
                 usize min_inside = 500) {
    constexpr u32 kSide = 128;
    const MotionClip* c = gift.motion.clip(name);
    REQUIRE(c);
    std::string why;
    for (const f64 t : times) {
        if (t > c->length) continue;
        Pose pose;
        REQUIRE(s.pose(rig, t, name, doc.params, doc.seed, pose));
        REQUIRE(rig.pose(pose, why));
        const assets::CookedTexture tab = rig.draw(kSide, kSide);
        std::vector<u8> screen;
        const bool drew = ui::render_posed_svg(gift.svg, screen_pose(gift.motion, *c, t), kSide, kSide, screen, why);
        INFO(why);
        REQUIRE(drew);
        REQUIRE(screen.size() == tab.rgba8.size());
        f64 cover_tab = 0, cover_screen = 0;
        usize inside = 0, off = 0;
        std::string where;
        auto at = [&](const std::vector<u8>& px, u32 x, u32 y, u32 ch) { return px[(static_cast<usize>(y) * kSide + x) * 4 + ch]; };
        for (u32 y = 0; y < kSide; ++y)
            for (u32 x = 0; x < kSide; ++x) {
                cover_tab += at(tab.rgba8, x, y, 3);
                cover_screen += at(screen, x, y, 3);
                if (x == 0 || y == 0 || x + 1 == kSide || y + 1 == kSide) continue;
                bool solid = true;
                for (int dy = -1; dy <= 1 && solid; ++dy)
                    for (int dx = -1; dx <= 1 && solid; ++dx)
                        for (u32 ch = 0; ch < 4 && solid; ++ch)
                            solid = at(tab.rgba8, x + dx, y + dy, ch) == at(tab.rgba8, x, y, ch) && at(tab.rgba8, x, y, 3) == 255;
                if (!solid) continue;
                ++inside;
                for (u32 ch = 0; ch < 4; ++ch)
                    if (std::abs(int(at(tab.rgba8, x, y, ch)) - int(at(screen, x, y, ch))) > 2) {
                        if (++off <= 6)
                            where += " (" + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(at(screen, x, y, 0)) + "," +
                                     std::to_string(at(screen, x, y, 1)) + "," + std::to_string(at(screen, x, y, 2)) + "," +
                                     std::to_string(at(screen, x, y, 3)) + " / " + std::to_string(at(tab.rgba8, x, y, 0)) + "," +
                                     std::to_string(at(tab.rgba8, x, y, 1)) + "," + std::to_string(at(tab.rgba8, x, y, 2)) + ")";
                        break;
                    }
            }
        INFO(name, " at ", t, ": ", off, " of ", inside, " inside differ", where, "; cover ", cover_tab, " / ", cover_screen);
        CHECK(inside > min_inside);
        CHECK(off == 0);
        CHECK(std::abs(cover_tab - cover_screen) <= 0.01 * cover_tab);
    }
}

} // namespace

TEST_CASE("Screen clips file: every field there and back, numbers in their shortest form") {
    MotionFile f;
    f.character = "Ёжик";
    f.from = "персонажи/Ёжик.pixel.json";
    f.picture = "Ёжик.svg";
    f.parts.push_back({"body", {32.5, 40}, {1, 0, 0, 1, 2, 3}, {0.98, 0.17, -0.17, 0.98, 1.5, -2}});
    f.curves.push_back(std::vector<f64>(vector::kMotionCurvePoints, 0.1));
    MotionClip c;
    c.name = "бег";
    c.length = 1.2;
    c.loop = true;
    MotionTrack y{"body", MotionProp::Y, {}};
    MotionPiece hold;
    hold.value.v = -0.1;
    y.pieces.push_back(hold);
    MotionPiece ease;
    ease.kind = MotionPiece::Kind::Ease;
    ease.at = 0.2;
    ease.length = 1.0 / 3;
    ease.from.v = 0;
    ease.to.v = -6;
    ease.timing = vector::Timing::InOutBack;
    y.pieces.push_back(ease);
    MotionPiece fr;
    fr.kind = MotionPiece::Kind::Frames;
    fr.at = 0.6;
    fr.end = 0.62;
    fr.frames.resize(vector::motion_frame_count(fr.at, fr.end));
    for (usize i = 0; i < fr.frames.size(); ++i) fr.frames[i].v = 0.1 * static_cast<f64>(i);
    y.pieces.push_back(fr);
    MotionPiece tail;
    tail.at = 0.62;
    tail.value.v = 3;
    y.pieces.push_back(tail);
    c.tracks.push_back(y);
    MotionTrack fill{"body", MotionProp::Fill, {}};
    MotionPiece fe;
    fe.kind = MotionPiece::Kind::Ease;
    fe.length = 0.4;
    fe.to = {1, {255, 136, 0, 128}, {}, 0};
    fe.has_bezier = true;
    fe.bezier = {0.2, 0, 0.4, 1};
    fill.pieces.push_back(fe);
    c.tracks.push_back(fill);
    MotionTrack w{"body", MotionProp::StrokeWidth, {}};
    MotionPiece we;
    we.kind = MotionPiece::Kind::Ease;
    we.length = 0.5;
    we.to = {1, {}, {}, 4.5};
    we.curve = 0;
    w.pieces.push_back(we);
    c.tracks.push_back(w);
    MotionTrack m{"mouth", MotionProp::Morph, {}};
    MotionPiece mh;
    mh.value = {0.5, {}, "mouth_o", 0};
    m.pieces.push_back(mh);
    c.tracks.push_back(m);
    MotionTrack v{"mouth", MotionProp::Visible, {}};
    MotionPiece vh;
    vh.value.v = 0;
    v.pieces.push_back(vh);
    c.tracks.push_back(v);
    c.marks.push_back({true, "шаг", 0.3, 0});
    c.marks.push_back({false, "удар", 0.5, 0.1});
    c.marks.push_back({false, "сказал \"ой\" \\ и\tвсё", 0.7, 0}); // what JSON escapes
    c.not_drawn = {"dash", "mask"};
    f.clips.push_back(c);
    MotionClip still;
    still.name = "стоит";
    f.clips.push_back(still);

    const std::string text = vector::write_motion_file(f);
    MotionFile back;
    std::string why;
    REQUIRE(vector::read_motion_file(text, back, why));
    CHECK(back == f);
    CHECK(vector::write_motion_file(back) == text);
    CHECK(text.back() == '\n');
    // The shortest text that reads back as the same number.
    CHECK(text.find("\"length\": 1.2,") != std::string::npos);
    CHECK(text.find("0.3333333333333333") != std::string::npos);
    CHECK(text.find("\"at\": 0.62, \"hold\": 3}") != std::string::npos);
    CHECK(text.find("[1, \"#ff880080\"]") != std::string::npos);
    CHECK(text.find("[0.5, \"mouth_o\"]") != std::string::npos);
    CHECK(text.find("[1, 4.5]") != std::string::npos);
    CHECK(text.find("\"hold\": false") != std::string::npos);
    CHECK(text.find("\"ease\": \"in_out_back\"") != std::string::npos);
    CHECK(text.find("\"bezier\": [0.2, 0, 0.4, 1]") != std::string::npos);
    CHECK(text.find("\"curve\": 0") != std::string::npos);
    CHECK(text.find("{\"sound\": \"шаг\", \"at\": 0.3}") != std::string::npos);
    CHECK(text.find("{\"emit\": \"удар\", \"at\": 0.5, \"length\": 0.1}") != std::string::npos);
    CHECK(text.find(R"({"emit": "сказал \"ой\" \\ и\tвсё", "at": 0.7})") != std::string::npos);
    CHECK(text.find("\"not_drawn\": [\"dash\", \"mask\"]") != std::string::npos);
    CHECK(back.clip("бег") != nullptr);
    CHECK(back.clip("прыжок") == nullptr);
    CHECK(back.part("body")->pivot.x == 32.5);
}

TEST_CASE("Screen clips file: what it cannot read names the field") {
    const std::string hold = R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": 1}]})";
    MotionFile ok;
    std::string why;
    REQUIRE(vector::read_motion_file(one_track(hold), ok, why));

    CHECK(read_error("не JSON").find("JSON") != std::string::npos);
    CHECK(read_error(R"({"forge": "motion"})").find("version") != std::string::npos);
    CHECK(read_error(R"({"forge": "picture", "version": 1})").find("forge") != std::string::npos);
    CHECK(read_error(R"({"forge": "motion", "version": 3, "character": "a", "from": "", "picture": "a.svg", "parts": {}, "curves": [], "clips": {}})")
              .find("version") != std::string::npos);
    // A field not of its kind.
    std::string looped = one_track(hold);
    looped.replace(looped.find("\"loop\": false"), 13, "\"loop\": 3");
    CHECK(read_error(looped).find("clips.c: loop") != std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": "высоко"}]})")).find("clips.c.tracks[0].pieces[0]") !=
          std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "jump", "pieces": [{"at": 0, "hold": 1}]})")).find("clips.c.tracks[0]") != std::string::npos);
    // A piece without its value, pieces out of time, the first not at 0.
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0}]})")).find("clips.c.tracks[0].pieces[0]") != std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": 1}, {"at": 0.5, "hold": 2}, {"at": 0.2, "hold": 3}]})"))
              .find("clips.c.tracks[0].pieces[2]") != std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0.1, "hold": 1}]})")).find("clips.c.tracks[0].pieces[0]") !=
          std::string::npos);
    // Frames that end elsewhere than the next piece, or with another count of values.
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "end": 0.5, "frames": [0, 1]}, {"at": 0.6, "hold": 1}]})"))
              .find("clips.c.tracks[0].pieces[0]") != std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "end": 1, "frames": [0, 1]}]})"))
              .find("clips.c.tracks[0].pieces[0]") != std::string::npos);
    // A moving track of a part not in parts; the same track twice; a mark without its name.
    CHECK(read_error(one_track(hold, "", R"("parts": {},)")).find("clips.c.tracks[0]") != std::string::npos);
    CHECK(read_error(one_track(hold + ", " + hold)).find("clips.c.tracks[1]") != std::string::npos);
    std::string marks = one_track(hold);
    marks.replace(marks.find("\"marks\": []"), 11, R"("marks": [{"at": 0.2}])");
    CHECK(read_error(marks).find("clips.c.marks[0]") != std::string::npos);
    // An ease of a curve the file has not.
    CHECK(read_error(one_track(R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "length": 0.5, "from": 0, "to": 1, "curve": 3}]})"))
              .find("clips.c.tracks[0].pieces[0]") != std::string::npos);
}

TEST_CASE("Screen clips file: pieces between their moments") {
    MotionFile f;
    // ease: from, the easing on the way, to after its length; a word, a bezier, an author's curve.
    MotionPiece e;
    e.kind = MotionPiece::Kind::Ease;
    e.at = 0.5;
    e.length = 1;
    e.from.v = 2;
    e.to.v = 12;
    e.timing = vector::Timing::Linear;
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 0.5).v == 2);
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 0.75).v == doctest::Approx(4.5));
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 1.5).v == 12);
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 9).v == 12);
    e.timing = vector::Timing::OutBack; // past its end on the way
    f64 most = 0;
    for (int i = 0; i <= 100; ++i) most = std::max(most, vector::motion_piece_value(f, MotionProp::X, e, 0.5 + i / 100.0).v);
    CHECK(most > 12);
    e.has_bezier = true;
    e.bezier = {0.3, 0, 0.2, 1};
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 0.75).v == doctest::Approx(2 + 10 * vector::bezier(e.bezier, 0.25)));
    e.has_bezier = false;
    f.curves.push_back({});
    for (usize i = 0; i < vector::kMotionCurvePoints; ++i) {
        const f64 s = static_cast<f64>(i) / static_cast<f64>(vector::kMotionCurvePoints - 1);
        f.curves.back().push_back(s * s);
    }
    e.curve = 0;
    CHECK(vector::motion_piece_value(f, MotionProp::X, e, 1.0).v == doctest::Approx(2 + 10 * 0.25));

    // A colour from the part's own and back to it; between two colours each channel by the share.
    MotionPiece c;
    c.kind = MotionPiece::Kind::Ease;
    c.length = 1;
    c.timing = vector::Timing::Linear;
    c.to = {1, {255, 0, 0, 255}, {}, 0};
    MotionValue half = vector::motion_piece_value(f, MotionProp::Fill, c, 0.5);
    CHECK(half.v == doctest::Approx(0.5));
    CHECK(half.color == vector::MotionColor{255, 0, 0, 255});
    std::swap(c.from, c.to);
    half = vector::motion_piece_value(f, MotionProp::Fill, c, 0.25);
    CHECK(half.v == doctest::Approx(0.75));
    CHECK(vector::motion_same(MotionProp::Fill, vector::motion_piece_value(f, MotionProp::Fill, c, 1), vector::motion_rest(MotionProp::Fill)));
    c.from = {1, {0, 0, 255, 255}, {}, 0};
    c.to = {1, {255, 0, 0, 255}, {}, 0};
    CHECK(vector::motion_piece_value(f, MotionProp::Fill, c, 0.5).color == vector::MotionColor{128, 0, 128, 255});

    // A morph from one shape to another goes through the part's own.
    MotionPiece m;
    m.kind = MotionPiece::Kind::Ease;
    m.length = 1;
    m.timing = vector::Timing::Linear;
    m.from = {1, {}, "a", 0};
    m.to = {1, {}, "b", 0};
    MotionValue q = vector::motion_piece_value(f, MotionProp::Morph, m, 0.25);
    CHECK(q.target == "a");
    CHECK(q.v == doctest::Approx(0.5));
    q = vector::motion_piece_value(f, MotionProp::Morph, m, 0.75);
    CHECK(q.target == "b");
    CHECK(q.v == doctest::Approx(0.5));

    // Visible changes at the end, whatever the easing does on the way; two widths go as numbers.
    MotionPiece v;
    v.kind = MotionPiece::Kind::Ease;
    v.length = 1;
    v.timing = vector::Timing::InBack;
    v.from.v = 1;
    v.to.v = 0;
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, v, 0.99).v == 1);
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, v, 1).v == 0);
    v.timing = vector::Timing::OutBack; // past 1 on the way: still shown
    REQUIRE(vector::motion_ease(f, v, 0.6) > 1);
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, v, 0.6).v == 1);
    MotionPiece w;
    w.kind = MotionPiece::Kind::Ease;
    w.length = 1;
    w.timing = vector::Timing::Linear;
    w.from = {1, {}, {}, 2};
    w.to = {1, {}, {}, 6};
    CHECK(vector::motion_piece_value(f, MotionProp::StrokeWidth, w, 0.5).n == doctest::Approx(4));

    // frames: 120 a second from at, the last the moment before end; mixed between.
    MotionPiece fr;
    fr.kind = MotionPiece::Kind::Frames;
    fr.at = 0.1;
    fr.end = 0.13; // 3.6 120ths: moments at 0, 1, 2, 3 of them, then the end
    REQUIRE(vector::motion_frame_count(fr.at, fr.end) == 5);
    fr.frames = {{0}, {10}, {20}, {30}, {34}};
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.1).v == 0);
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.1 + 1.0 / 240).v == doctest::Approx(5));
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.1 + 2.0 / 120).v == doctest::Approx(20));
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.1 + 3.0 / 120).v == doctest::Approx(30));
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.1275).v == doctest::Approx(32));
    CHECK(vector::motion_piece_value(f, MotionProp::X, fr, 0.13).v == 34);
    CHECK(vector::motion_frame_count(0, 0.25) == 31); // 30 120ths before it, then 0.25 itself
    CHECK(vector::motion_frame_count(0.5, 0.5) == 1);
    // Visible has no values between: shown up to the frame that hides it.
    MotionPiece vf;
    vf.kind = MotionPiece::Kind::Frames;
    vf.end = 0.03;
    REQUIRE(vector::motion_frame_count(vf.at, vf.end) == 5);
    vf.frames = {{1}, {0}, {0}, {0}, {1}};
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, vf, 0.75 / 120).v == 1);
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, vf, 1.0 / 120).v == 0);
    CHECK(vector::motion_piece_value(f, MotionProp::Visible, vf, 2.5 / 120).v == 0);

    // A track: its piece at t, t held within the clip.
    MotionTrack tr{"a", MotionProp::X, {}};
    MotionPiece h;
    h.value.v = 1;
    tr.pieces.push_back(h);
    e.at = 0.5;
    e.curve = -1;
    e.timing = vector::Timing::Linear;
    tr.pieces.push_back(e);
    CHECK(vector::motion_track_value(f, tr, -1, 2).v == 1);
    CHECK(vector::motion_track_value(f, tr, 0.49, 2).v == 1);
    CHECK(vector::motion_track_value(f, tr, 1, 2).v == doctest::Approx(7));
    CHECK(vector::motion_track_value(f, tr, 5, 1.2).v == doctest::Approx(9));
    // On nanoseconds: a game's moment a hair before a piece's start (twelve frames of 1/60 s added up) is at it.
    MotionTrack held{"a", MotionProp::Visible, {}};
    MotionPiece h2;
    h2.at = 0.2;
    held.pieces = {h, h2};
    f64 played = 0;
    for (int i = 0; i < 12; ++i) played += 1.0 / 60;
    REQUIRE(played < 0.2);
    CHECK(vector::motion_track_value(f, held, played, 1).v == 0);
    CHECK(vector::motion_track_value(f, held, 0.2 - 1e-6, 1).v == 1);
}

TEST_CASE("Screen clips: the game's values are the tab's frame, for every kind of block") {
    const Rig rig = loaded();
    PoseScript s = script_of(code());
    const PixelDoc doc = doc64();

    // Each timing word is a transition of the file, the moments between them holds.
    const MotionClip words = checked(s, rig, doc, "words");
    const MotionTrack* x = track_of(words, "ball", MotionProp::X);
    REQUIRE(x);
    CHECK(x->pieces.size() == vector::kTimingCount);
    for (usize i = 0; i < x->pieces.size(); ++i) {
        INFO(i);
        CHECK(x->pieces[i].kind == MotionPiece::Kind::Ease);
        CHECK(x->pieces[i].timing == static_cast<vector::Timing>(i));
    }
    CHECK(words.tracks.size() == 1);

    // Colours from and to their own, a stroke on a shape without one, widths, visible, a bezier, an author's easing, a
    // morph from one shape to another, skews and scales: transitions; the marks with their times.
    MotionFile eased_file;
    const MotionClip eased = checked(s, rig, doc, "eased", &eased_file);
    for (const auto& [part, prop] : std::vector<std::pair<std::string, MotionProp>>{{"torso", MotionProp::Fill},
                                                                                  {"torso", MotionProp::Stroke},
                                                                                  {"torso", MotionProp::StrokeWidth},
                                                                                  {"body", MotionProp::Rot},
                                                                                  {"ball", MotionProp::Stroke},
                                                                                  {"box", MotionProp::Opacity},
                                                                                  {"hand", MotionProp::X},
                                                                                  {"mouth", MotionProp::Morph},
                                                                                  {"arm", MotionProp::SkewX},
                                                                                  {"arm", MotionProp::SkewY},
                                                                                  {"arm", MotionProp::Scale},
                                                                                  {"arm", MotionProp::Sx},
                                                                                  {"arm", MotionProp::Sy}}) {
        const std::string what = part + " " + vector::motion_prop_word(prop);
        INFO(what);
        const MotionTrack* t = track_of(eased, part, prop);
        REQUIRE(t);
        CHECK(std::any_of(t->pieces.begin(), t->pieces.end(), [](const MotionPiece& p) { return p.kind == MotionPiece::Kind::Ease; }));
        CHECK(std::none_of(t->pieces.begin(), t->pieces.end(), [](const MotionPiece& p) { return p.kind == MotionPiece::Kind::Frames; }));
    }
    // Visible changes at a transition's end: the moment before it is still shown.
    const MotionTrack* shown = track_of(eased, "ball", MotionProp::Visible);
    REQUIRE(shown);
    REQUIRE(shown->pieces.size() == 2);
    CHECK(shown->pieces[0].value.v == 1);
    CHECK(shown->pieces[1].value.v == 0);
    CHECK(shown->pieces[1].kind == MotionPiece::Kind::Hold);
    CHECK(track_of(eased, "box", MotionProp::Opacity)->pieces[1].has_bezier);
    const MotionTrack* hand = track_of(eased, "hand", MotionProp::X);
    CHECK(hand->pieces[1].curve == 0);
    CHECK(eased_file.curves.size() == 1);
    const MotionTrack* morph = track_of(eased, "mouth", MotionProp::Morph);
    REQUIRE(morph->pieces.size() >= 3);
    CHECK(morph->pieces[1].to.target == "mouth_o");
    CHECK(morph->pieces[2].to.target == "mouth_w");
    REQUIRE(eased.marks.size() == 2);
    CHECK(eased.marks[0].sound);
    CHECK(eased.marks[0].name == "шаг");
    CHECK_FALSE(eased.marks[1].sound);
    CHECK(eased.marks[1].length == doctest::Approx(0.1));
    CHECK(eased.marks[1].at >= eased.marks[0].at);
    CHECK(eased.not_drawn.empty());

    // What does not go by an easing is written 120 times a second: a spring, tween's frames, a path, an arc, a binding.
    for (const char* clip : {"spring", "tween", "along", "arc", "binding"}) {
        INFO(clip);
        const MotionClip c = checked(s, rig, doc, clip);
        CHECK(count_kind(c, MotionPiece::Kind::Frames) >= 1);
    }
    const MotionClip along = checked(s, rig, doc, "along");
    CHECK(track_of(along, "ball", MotionProp::Rot) != nullptr); // turned along the line
    // back, save and restore, rest: transitions to where the recording knew.
    for (const char* clip : {"back", "saved"}) {
        INFO(clip);
        const MotionClip c = checked(s, rig, doc, clip);
        CHECK(count_kind(c, MotionPiece::Kind::Frames) == 0);
        CHECK(count_kind(c, MotionPiece::Kind::Ease) >= 2);
    }
    // start_from: its first moment is where the other clip ends, the tracks of the parts it leaves moved there.
    const MotionClip start = checked(s, rig, doc, "start");
    const MotionTrack* skew = track_of(start, "arm", MotionProp::SkewX);
    REQUIRE(skew);
    CHECK(skew->pieces.front().kind == MotionPiece::Kind::Hold);
    CHECK(skew->pieces.front().value.v == doctest::Approx(15));
    REQUIRE(track_of(start, "mouth", MotionProp::Morph));
    CHECK(track_of(start, "mouth", MotionProp::Morph)->pieces.front().value.target == "mouth_w");
    // A block cut by the next: the first transition until the cut, the next from where it stood.
    const MotionClip cut = checked(s, rig, doc, "cut");
    const MotionTrack* cx = track_of(cut, "ball", MotionProp::X);
    REQUIRE(cx);
    REQUIRE(cx->pieces.size() >= 2);
    CHECK(cx->pieces[0].kind == MotionPiece::Kind::Ease);
    CHECK(cx->pieces[0].length == 1);
    CHECK(cx->pieces[1].at == doctest::Approx(0.3));
    CHECK(cx->pieces[1].from.v == doctest::Approx(3));
    // A clip of no length: one moment.
    const MotionClip zero = checked(s, rig, doc, "zero");
    CHECK(zero.length == 0);
    REQUIRE(track_of(zero, "ball", MotionProp::X));
    CHECK(track_of(zero, "ball", MotionProp::X)->pieces.size() == 1);
    CHECK(track_of(zero, "ball", MotionProp::X)->pieces[0].value.v == 5);
    // pose(t) on top: its property in frames, the recorded one under it a transition.
    const MotionClip posed = checked(s, rig, doc, "posed");
    CHECK(track_of(posed, "box", MotionProp::X)->pieces.front().kind == MotionPiece::Kind::Frames);
    CHECK(track_of(posed, "box", MotionProp::Y)->pieces.front().kind == MotionPiece::Kind::Ease);
    // pose(t) over a block of the same property: the frame is pose(t)'s, not the block's easing.
    const MotionClip over = checked(s, rig, doc, "over");
    REQUIRE(track_of(over, "box", MotionProp::Y));
    CHECK(track_of(over, "box", MotionProp::Y)->pieces.front().kind == MotionPiece::Kind::Frames);
    CHECK(count_kind(over, MotionPiece::Kind::Ease) == 0);
    // A block from rest to rest: no track.
    const MotionClip still = checked(s, rig, doc, "still");
    CHECK(still.tracks.empty());
}

TEST_CASE("Screen clips: a part's matrix is the rig's after the pose, in groups with transforms of their own") {
    Rig rig = loaded();
    PoseScript s = script_of(code());
    const PixelDoc doc = doc64();
    std::string why;
    for (const char* name : {"eased", "along", "arc", "start", "steep"}) { // steep: a skew past 89°, held there
        MotionFile file;
        file.clips.push_back(checked(s, rig, doc, name));
        const MotionClip& c = file.clips.back();
        for (const MotionTrack& t : c.tracks)
            if (vector::motion_prop_moves(t.prop) && !file.part(t.part)) {
                const i32 pi = rig.part(t.part);
                const Affine fr = rig.rest_frame(pi), own = rig.rest_local(pi);
                file.parts.push_back({t.part, rig.pivot(pi), {fr.a, fr.b, fr.c, fr.d, fr.e, fr.f}, {own.a, own.b, own.c, own.d, own.e, own.f}});
            }
        // On the 120ths, where what goes 120 times a second is the tab's exactly.
        for (const f64 t : {0.0, vector::motion_frame_moment(0, 8), vector::motion_frame_moment(0, 18), vector::motion_frame_moment(0, 37),
                            vector::motion_frame_moment(0, 60), vector::motion_frame_moment(0, 92), c.length}) {
            if (t > c.length) continue;
            Pose pose;
            REQUIRE(s.pose(rig, t, name, doc.params, doc.seed, pose));
            REQUIRE(rig.pose(pose, why));
            for (const vector::MotionPartPose& pp : vector::motion_pose(file, c, t)) {
                if (!pp.moves) continue;
                const Affine m = rig.matrix(rig.part(pp.id)); // ThorVG keeps it in floats
                const std::string what = std::string(name) + " " + pp.id + " at " + std::to_string(t);
                INFO(what);
                CHECK(pp.local.a == doctest::Approx(m.a).epsilon(1e-6));
                CHECK(pp.local.b == doctest::Approx(m.b).epsilon(1e-6));
                CHECK(pp.local.c == doctest::Approx(m.c).epsilon(1e-6));
                CHECK(pp.local.d == doctest::Approx(m.d).epsilon(1e-6));
                CHECK(pp.local.e == doctest::Approx(m.e).epsilon(1e-6));
                CHECK(pp.local.f == doctest::Approx(m.f).epsilon(1e-6));
            }
        }
    }
    // The rest pose: the file's own transforms.
    REQUIRE(rig.pose({}, why));
    for (const char* id : {"body", "arm", "hand"}) {
        const i32 pi = rig.part(id);
        CHECK(rig.matrix(pi) == rig.rest_local(pi));
    }
    const Affine arm_frame = rig.rest_frame(rig.part("arm"));
    CHECK(arm_frame.e == doctest::Approx(2));
    CHECK(arm_frame.f == doctest::Approx(3));
}

TEST_CASE("Screen clips: the screen's picture (lunasvg) is the tab's (ThorVG)") {
    Rig rig = loaded();
    PoseScript s = script_of(code());
    const PixelDoc doc = doc64();
    ScreenGift gift;
    std::string why;
    const bool built = build_screen(s, rig, doc, "Ёжик", "Ёжик.pixel.json", kPicture, nullptr, gift, why);
    INFO(why);
    REQUIRE(built);
    CHECK(gift.svg == kPicture);
    for (const char* name : {"eased", "along", "start", "spring"}) {
        const MotionClip* c = gift.motion.clip(name);
        REQUIRE(c);
        same_as_tab(gift, s, rig, doc, name, {0.0, 0.1, 0.2, 0.35, 0.6, 0.9, 1.3, c->length});
    }
}

TEST_CASE("Screen clips: the screen's picture of a pose, part by part in the file's order, as the file written so by hand") {
    // A group's fill over the parts in it and one of them its own after it (the pose lists it first); a shape without a
    // fill keeps none; a colour the file writes in `style` goes there; an opacity of its own times the clip's.
    const char* kHand = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <g id="team" fill="#2050a0">
    <rect id="lead" x="4" y="4" width="16" height="16" fill="#a02020"/>
    <rect id="rest" x="24" y="4" width="16" height="16"/>
    <rect id="hole" x="44" y="4" width="16" height="16" fill="none" stroke="#202020" stroke-width="3"/>
  </g>
  <rect id="styled" x="4" y="30" width="16" height="16" style="fill:#20a020"/>
  <rect id="ghost" x="30" y="30" width="16" height="16" fill="#000000" opacity="0.5"/>
</svg>)svg";
    const char* kWant = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <g id="team" fill="#ffffff">
    <rect id="lead" x="4" y="4" width="16" height="16" fill="#000000"/>
    <rect id="rest" x="24" y="4" width="16" height="16"/>
    <rect id="hole" x="44" y="4" width="16" height="16" fill="none" stroke="#202020" stroke-width="3"/>
  </g>
  <rect id="styled" x="4" y="30" width="16" height="16" style="fill:#ff00ff"/>
  <rect id="ghost" x="30" y="30" width="16" height="16" fill="#000000" opacity="0.25"/>
</svg>)svg";
    auto part = [](const char* id, MotionProp p, MotionValue v) {
        vector::MotionPartPose pp;
        pp.id = id;
        for (usize i = 0; i < vector::kMotionPropCount; ++i) pp.values[i] = vector::motion_rest(static_cast<MotionProp>(i));
        pp.has[static_cast<usize>(p)] = true;
        pp.values[static_cast<usize>(p)] = v;
        return pp;
    };
    const std::vector<vector::MotionPartPose> pose{part("lead", MotionProp::Fill, {1, {0, 0, 0, 255}, {}, 0}),
                                                   part("team", MotionProp::Fill, {1, {255, 255, 255, 255}, {}, 0}),
                                                   part("styled", MotionProp::Fill, {1, {255, 0, 255, 255}, {}, 0}),
                                                   part("ghost", MotionProp::Opacity, {0.5, {}, {}, 0})};
    std::vector<u8> got, want, rest;
    std::string why;
    REQUIRE(ui::render_posed_svg(kHand, {"hand.svg", pose}, 64, 64, got, why));
    REQUIRE(ui::render_posed_svg(kWant, {"want.svg", {}}, 64, 64, want, why));
    REQUIRE(ui::render_posed_svg(kHand, {"hand.svg", {}}, 64, 64, rest, why));
    usize off = 0, changed = 0;
    for (usize i = 0; i < got.size(); ++i) {
        off += got[i] != want[i];
        changed += got[i] != rest[i];
    }
    std::string where;
    for (usize i = 0; i < got.size() && where.size() < 400; i += 4)
        if (got[i] != want[i] || got[i + 1] != want[i + 1] || got[i + 2] != want[i + 2] || got[i + 3] != want[i + 3])
            where += " (" + std::to_string(i / 4 % 64) + "," + std::to_string(i / 4 / 64) + ": " + std::to_string(got[i]) + "," +
                     std::to_string(got[i + 1]) + "," + std::to_string(got[i + 2]) + "," + std::to_string(got[i + 3]) + " / " +
                     std::to_string(want[i]) + "," + std::to_string(want[i + 1]) + "," + std::to_string(want[i + 2]) + "," + std::to_string(want[i + 3]) + ")";
    INFO(off, " bytes off", where);
    CHECK(off == 0);
    CHECK(changed > 1000);
}

TEST_CASE("Screen clips: shapes without an id take their part's look, past the elements lunasvg leaves out") {
    // A group's fill and stroke over shapes of its own without ids, a <title>, an editor's element, a shape with a prefix
    // and a group inside it among them (lunasvg makes no element of the first three); a shape with an id another element
    // took first; a shape without a fill with an id first taken by one lunasvg does not make.
    const char* kHand = R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" width="64" height="64" viewBox="0 0 64 64">
  <rect id="twin" x="44" y="44" width="16" height="16" fill="#808080"/>
  <g id="team" fill="#2050a0">
    <title>команда</title>
    <rect x="4" y="4" width="16" height="16"/>
    <inkscape:path-effect effect="spiro"><rect id="dup" x="0" y="0" width="64" height="64"/></inkscape:path-effect>
    <svg:rect x="0" y="0" width="64" height="64"/>
    <g><rect x="24" y="4" width="16" height="16" fill="#a02020"/></g>
    <rect id="twin" x="4" y="30" width="16" height="16" stroke="#202020" stroke-width="2"/>
    <rect id="dup" x="24" y="30" width="16" height="16" fill="none" stroke="#202020" stroke-width="4"/>
  </g>
</svg>)svg";
    const char* kWant = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <rect x="44" y="44" width="16" height="16" fill="#808080"/>
  <g fill="#ffffff">
    <rect x="4" y="4" width="16" height="16" stroke="#00ff00" stroke-width="1"/>
    <g><rect x="24" y="4" width="16" height="16" fill="#ffffff" stroke="#00ff00" stroke-width="1"/></g>
    <rect x="4" y="30" width="16" height="16" stroke="#00ff00" stroke-width="2"/>
    <rect x="24" y="30" width="16" height="16" fill="none" stroke="#00ff00" stroke-width="4"/>
  </g>
</svg>)svg";
    vector::MotionPartPose pp;
    pp.id = "team";
    for (usize i = 0; i < vector::kMotionPropCount; ++i) pp.values[i] = vector::motion_rest(static_cast<MotionProp>(i));
    pp.has[static_cast<usize>(MotionProp::Fill)] = true;
    pp.values[static_cast<usize>(MotionProp::Fill)] = {1, {255, 255, 255, 255}, {}, 0};
    pp.has[static_cast<usize>(MotionProp::Stroke)] = true;
    pp.values[static_cast<usize>(MotionProp::Stroke)] = {1, {0, 255, 0, 255}, {}, 0};
    std::vector<u8> got, want, rest;
    std::string why;
    REQUIRE(ui::render_posed_svg(kHand, {"hand.svg", {pp}}, 64, 64, got, why));
    REQUIRE(ui::render_posed_svg(kWant, {"want.svg", {}}, 64, 64, want, why));
    REQUIRE(ui::render_posed_svg(kHand, {"hand.svg", {}}, 64, 64, rest, why));
    usize off = 0, changed = 0;
    for (usize i = 0; i < got.size(); ++i) {
        off += got[i] != want[i];
        changed += got[i] != rest[i];
    }
    INFO(off, " bytes off");
    CHECK(off == 0);
    CHECK(changed > 1000);
}

TEST_CASE("Screen clips: the marks a game passes between two of its frames") {
    MotionClip c;
    c.length = 1;
    c.loop = true;
    c.marks = {{true, "шаг", 0, 0}, {false, "удар", 0.5, 0.1}, {true, "шаг2", 1, 0}};
    using V = std::vector<usize>;
    CHECK(vector::motion_marks_passed(c, -1, 0, true) == V{0}); // the first frame counts the marks at 0
    CHECK(vector::motion_marks_passed(c, 0, 0.4, true) == V{});
    CHECK(vector::motion_marks_passed(c, 0.4, 0.6, true) == V{1});
    CHECK(vector::motion_marks_passed(c, 0.6, 1.2, true) == V{0, 2}); // round the loop: at the same moment, in the clip's order
    CHECK(vector::motion_marks_passed(c, 1.2, 1.5, true) == V{1});
    CHECK(vector::motion_marks_passed(c, 0.6, 3.6, true) == V{0, 2, 1}); // each once however many times round
    CHECK(vector::motion_clip_time(c, 2.25, true) == doctest::Approx(0.25));
    // The game's `loop`, not the clip's: the looping clip played once is held at its end, its mark at 0 not passed
    // again there.
    CHECK(vector::motion_marks_passed(c, 0.6, 1, false) == V{2});
    CHECK(vector::motion_marks_passed(c, 1, 1, false) == V{}); // held at its end: nothing more
    CHECK(vector::motion_clip_time(c, 3, false) == 1);
    // A clip without a loop played round and round.
    c.loop = false;
    CHECK(vector::motion_clip_time(c, 2.25, true) == doctest::Approx(0.25));
    CHECK(vector::motion_marks_passed(c, 0.6, 1.2, true) == V{0, 2});
    CHECK(vector::motion_marks_passed(c, 0.6, 1.2, false) == V{2});
}

TEST_CASE("Screen clips: what a clip does that the screen does not draw, each from its own clip") {
    // Since part 5г the blur and the shadow (and the swaps, below); the rest is drawn.
    const char* kWords = R"(return {
  clips = {
    dash = function(p) part("torso").dash(5, 0.2) end,
    draw = function(p) part("torso").draw_to(0.5, 0.2) end,
    tint = function(p) part("body").tint("#ff0000", 0.2) end,
    flash = function(p) part("body").flash(1, 0.2) end,
    blur = function(p) part("body").blur(2, 0.2) end,
    shadow = function(p) part("body").shadow("#000000", 0.2) end,
    mask = function(p) part("ball"):mask("box") wait(0.1) end,
    link = function(p) part("ball"):attach("hand") part("hand").x(3, 0.2) end,
    copies = function(p) copies("ball", 2, function(i, c) c.x(i * 4, 0.2) end) end,
    order = function(p) part("ball"):to_top() wait(0.1) end,
    plain = function(p) part("ball").x(3, 0.2) end,
    flash0 = function(p) part("body").flash(0, 0.2) end,
  },
})";
    const Rig rig = loaded();
    PoseScript s = script_of(kWords);
    const PixelDoc doc = doc64();
    for (const char* word : {"blur", "shadow"}) {
        INFO(word);
        MotionFile f;
        MotionClip c;
        std::string why;
        const bool ok = screen_clip(s, rig, doc, word, f, c, why);
        INFO(why);
        REQUIRE(ok);
        CHECK(c.not_drawn == std::vector<std::string>{word});
    }
    for (const char* word : vector::kMotionNotDrawn) CHECK(vector::motion_not_drawn_words(word) != nullptr);
    for (const char* clip : {"dash", "draw", "tint", "flash", "mask", "link", "copies", "order", "plain", "flash0"}) {
        INFO(clip);
        MotionFile f;
        MotionClip c;
        std::string why;
        REQUIRE(screen_clip(s, rig, doc, clip, f, c, why));
        CHECK(c.not_drawn.empty());
    }
    // The copies have tracks of their own and are the clip's.
    MotionFile f;
    MotionClip copies;
    std::string why;
    REQUIRE(screen_clip(s, rig, doc, "copies", f, copies, why));
    REQUIRE(copies.copies.size() == 2);
    CHECK(copies.copies[0].id == "ball[1]");
    CHECK(copies.copies[1].source == "ball");
    CHECK(copies.copies[1].number == 2);
    CHECK(track_of(copies, "ball[2]", MotionProp::X) != nullptr);

    // The line of the tab: each word with its clips.
    ScreenGift gift;
    REQUIRE(build_screen(s, rig, doc, "Ёжик", "Ёжик.pixel.json", kPicture, nullptr, gift, why));
    REQUIRE_FALSE(gift.notes.empty());
    CHECK(gift.notes[0] == "На экране пока не рисуются: размытие («blur»), тень («shadow»)");
}

TEST_CASE("Screen clips: a character the code draws, its swaps and morphs into another drawing") {
    const char* kDrawn = R"(return {
  clips = {
    open = function(p) part("mouth"):morph("mouth_o", 0.2) part("nose").x(2, 0.1) end,
    angry = function(p) swap("angry", 0.2) end,
    brow = function(p) part("mouth"):morph("brow", 0.2) end,
    brow_on = function(p) start_from("brow") part("nose").x(1, 0.1) end,
  },
  draw = function(p, rng)
    canvas(64, 64)
    group("face", { y = 2 }, function()
      circle("head", { x = 32, y = 28, r = 14, fill = "#c09060" })
      rect("mouth", { x = 26, y = 34, w = 12, h = 3, r = 1, fill = "#402010" })
      path("mouth_o", { d = "M 27 33 Q 32 40 37 33 Z", fill = "#802020", hidden = true })
    end)
    rect("nose", { x = 30, y = 26, w = 4, h = 3, fill = "#a06040" })
  end,
  drawings = {
    angry = function(p, rng)
      canvas(64, 64)
      circle("head", { x = 32, y = 28, r = 16, fill = "#e06040" })
      path("brow", { d = "M 22 18 L 30 22 L 30 24 L 22 20 Z", fill = "#401010" })
    end,
  },
}
)";
    PoseScript s = script_of(kDrawn);
    Drawing main;
    REQUIRE(s.draw({}, 1, main));
    std::vector<NamedDrawing> others;
    std::string why;
    u32 line = 0;
    const PixelDoc doc = doc64();
    REQUIRE(draw_drawings(s, doc, others, why, line));
    Rig rig;
    REQUIRE(rig.load(drawing_svg(main, 64, 64), why));
    REQUIRE(add_drawings(rig, others, why, line));
    apply_pivots(rig, doc, &main, &others);

    ScreenGift gift;
    REQUIRE(build_screen(s, rig, doc, "Персонаж", "персонажи/Персонаж.pixel.json", "", &main, gift, why));
    // The mouth morphs: a <path> of the same outline; the nose does not: a <rect> still.
    CHECK(gift.svg.find("<path id=\"mouth\"") != std::string::npos);
    CHECK(gift.svg.find("<rect id=\"mouth\"") == std::string::npos);
    CHECK(gift.svg.find("<rect id=\"nose\"") != std::string::npos);
    CHECK(gift.svg == drawing_svg(drawing_with_paths(main, {"mouth"}), 64, 64));
    // The same picture: drawn by ThorVG at rest, the same pixels as the rect.
    Rig as_path;
    REQUIRE(as_path.load(gift.svg, why));
    const assets::CookedTexture a = rig.draw(64, 64), b = as_path.draw(64, 64);
    usize differ = 0;
    for (usize i = 0; i < a.rgba8.size(); ++i) differ += std::abs(int(a.rgba8[i]) - int(b.rgba8[i])) > 2;
    CHECK(differ == 0);
    CHECK(gift.motion.picture == "Персонаж.svg");
    CHECK(gift.motion.from == "персонажи/Персонаж.pixel.json");
    REQUIRE(gift.motion.part("nose"));
    CHECK(gift.motion.clip("angry")->not_drawn == std::vector<std::string>{"drawing"});
    CHECK(gift.motion.clip("brow")->not_drawn == std::vector<std::string>{"morph_drawing"});
    // From the frames: start_from leaves the mouth morphed into the brow, by no track of the clip's own.
    CHECK(gift.motion.clip("brow_on")->not_drawn == std::vector<std::string>{"morph_drawing"});
    CHECK(gift.motion.clip("open")->not_drawn.empty());

    // A file's SVG: written as it is, a morph of a shape that is not a <path> drawn (its stage makes it one, part 5г).
    PoseScript rect_morph = script_of(R"(return { clips = { c = function(p) part("box"):morph("mouth_o", 0.2) end } })");
    const Rig file_rig = loaded();
    REQUIRE(build_screen(rect_morph, file_rig, doc, "Ёжик", "Ёжик.pixel.json", kPicture, nullptr, gift, why));
    CHECK(gift.svg == kPicture);
    CHECK(gift.notes.empty());

    // No clips, a clip that fails.
    PoseScript none = script_of(R"(return { pose = function(t) end })");
    CHECK_FALSE(build_screen(none, file_rig, doc, "Ёжик", "Ёжик.pixel.json", kPicture, nullptr, gift, why));
    CHECK(why == "В коде нет клипов: экрану нечего играть");
    PoseScript bad = script_of("return { clips = {\n  ok = function(p) part('ball').x(1, 0.1) end,\n  jump = function(p)\n    part('nope').x(1, 0.1)\n  end,\n} }");
    CHECK_FALSE(build_screen(bad, file_rig, doc, "Ёжик", "Ёжик.pixel.json", kPicture, nullptr, gift, why));
    CHECK(why.rfind("клип «jump», строка 4: ", 0) == 0);
}

// ---- part 5г: everything the tab draws but the blur, the shadow and the swaps ----

namespace {

// A band with a gradient, a body moved by its group with a dashed torso and an arm turned inside it holding a hand, a
// ball, a curve, a mouth with a hidden shape to morph into, a box with a hidden circle to mask it, a see-through spot,
// a pair to reorder and a hidden group.
const char* kStage = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#2040c0"/>
      <stop offset="1" stop-color="#c0e0ff" stop-opacity="0.8"/>
    </linearGradient>
  </defs>
  <rect id="band" x="4" y="2" width="30" height="8" fill="url(#sky)"/>
  <g id="body" transform="translate(2 3)">
    <rect id="torso" x="20" y="16" width="16" height="20" fill="#3060c0" stroke="#102040" stroke-width="2" stroke-dasharray="5 3"/>
    <g id="arm" transform="rotate(10 40 28)"><rect id="hand" x="38" y="25" width="6" height="7" fill="#c03030"/></g>
  </g>
  <circle id="ball" cx="10" cy="48" r="5" fill="#30a060" stroke="#104020" stroke-width="1.5"/>
  <path id="line" d="M 40 52 C 46 40 54 62 60 48" fill="none" stroke="#202020" stroke-width="2.5" stroke-dasharray="4 2"/>
  <path id="mouth" d="M 22 44 L 34 44 L 34 48 L 22 48 Z" fill="#402010"/>
  <path id="mouth_o" d="M 23 42 L 33 42 L 31 52 L 25 52 Z" fill="#802020" display="none"/>
  <rect id="box" x="44" y="6" width="14" height="12" fill="#e0e000"/>
  <circle id="lid" cx="51" cy="12" r="5" display="none"/>
  <circle id="spot" cx="28" cy="28" r="7" fill="#ffffff" fill-opacity="0.3"/>
  <g id="pair">
    <rect id="p1" x="4" y="56" width="10" height="6" fill="#a020a0"/>
    <rect id="p2" x="9" y="54" width="10" height="6" fill="#20a0a0"/>
  </g>
  <g id="ghost" display="none"><circle cx="56" cy="56" r="3" fill="#000000"/></g>
</svg>)svg";

const char* kStageCode = R"(return {
  clips = {
    dash = function(p) part("line").dash(8, 0.4, "linear") end,
    draw = function(p)
      all(part("line").draw_to(0.6, 0.4), part("torso").draw_from(0.3, 0.4), part("ball").draw_to(0.75, 0.4))
      part("line").draw_from(0.6, 0.2)
    end,
    draw_morph = function(p) all(part("mouth"):morph("mouth_o", 0.4), part("mouth").draw_to(0.5, 0.4)) end,
    tint = function(p) all(part("body").tint("#ff8000", 0.3), part("band").tint("#40ff40", 0.3), part("torso").tint("#8080ff", 0.3)) end,
    flash = function(p) all(part("ball").flash(0.8, 0.3), part("ball").flash_color("#ff0000", 0.3), part("band").flash(0.5, 0.3)) end,
    mask_in = function(p)
      part("box"):mask("lid")
      all(part("lid").x(4, 0.4), part("box").y(3, 0.4))
    end,
    mask_out = function(p)
      part("box"):mask("lid", { invert = true })
      part("lid").y(4, 0.4)
    end,
    mask_shown = function(p)
      part("torso"):mask("spot")
      part("spot").x(-6, 0.4)
    end,
    link = function(p)
      part("ball"):attach("hand")
      all(part("arm").rot(30, 0.3), part("body").x(5, 0.3))
      part("ball"):detach()
      part("body").x(0, 0.2)
    end,
    copies = function(p)
      all(copies("ball", 3, function(i, c) all(c.x(i * 7, 0.3), c.fill("#ff0000", 0.2)) end),
          copies("lid", 1, function(i, c) c.x(-30, 0.1) end),
          copies("arm", 1, function(i, c) c.y(8, 0.1) end))
      copies("box", 1, function(i, c) c.y(30, 0.2) end)
    end,
    order = function(p)
      part("box"):to_bottom()
      wait(0.1)
      part("p1"):above("p2")
      wait(0.1)
      part("ball"):to_top()
      copies("p2", 1, function(i, c) c.x(6, 0.1) c:to_bottom() end)
    end,
  },
})";

const std::vector<const char*> kStageClips = {"dash", "draw", "draw_morph", "tint", "flash", "mask_in", "mask_out", "mask_shown", "link", "copies", "order"};

} // namespace

TEST_CASE("Screen clips, part 5г: the game's values, copies and order are the tab's frame") {
    const Rig rig = loaded(kStage);
    PoseScript s = script_of(kStageCode);
    const PixelDoc doc = doc64();
    for (const char* name : kStageClips) {
        INFO(name);
        const MotionClip c = checked(s, rig, doc, name);
        CHECK(c.not_drawn.empty());
    }
    // What each clip writes beyond 5в.
    const MotionClip draw = checked(s, rig, doc, "draw");
    REQUIRE(track_of(draw, "line", MotionProp::DrawTo));
    CHECK(track_of(draw, "line", MotionProp::DrawTo)->pieces[0].kind == MotionPiece::Kind::Ease);
    CHECK(track_of(draw, "line", MotionProp::DrawFrom) != nullptr);
    CHECK(track_of(draw, "torso", MotionProp::DrawFrom) != nullptr);
    CHECK(track_of(checked(s, rig, doc, "dash"), "line", MotionProp::Dash) != nullptr);
    const MotionClip tint = checked(s, rig, doc, "tint");
    REQUIRE(track_of(tint, "band", MotionProp::Tint));
    CHECK(track_of(tint, "band", MotionProp::Tint)->pieces[0].kind == MotionPiece::Kind::Ease);
    const MotionClip flash = checked(s, rig, doc, "flash");
    CHECK(track_of(flash, "ball", MotionProp::Flash) != nullptr);
    CHECK(track_of(flash, "ball", MotionProp::FlashColor) != nullptr);
    // The mask and the link: holds, from the call on.
    const MotionClip mask = checked(s, rig, doc, "mask_out");
    const MotionTrack* m = track_of(mask, "box", MotionProp::Mask);
    REQUIRE(m);
    REQUIRE(m->pieces.size() == 1);
    CHECK(m->pieces[0].kind == MotionPiece::Kind::Hold);
    CHECK(m->pieces[0].value.v == 2);
    CHECK(m->pieces[0].value.target == "lid");
    const MotionClip link = checked(s, rig, doc, "link");
    const MotionTrack* l = track_of(link, "ball", MotionProp::Link);
    REQUIRE(l);
    REQUIRE(l->pieces.size() == 2);
    CHECK(l->pieces[0].value.target == "hand");
    CHECK(l->pieces[1].value.target.empty()); // let go where it was carried to
    CHECK(l->pieces[1].at == doctest::Approx(0.3));
    CHECK_FALSE(l->pieces[1].value.m == vector::MotionMatrix{});
    // Copies by birth, with their tracks; the order's steps by time, a copy's too.
    const MotionClip copies = checked(s, rig, doc, "copies");
    REQUIRE(copies.copies.size() == 6);
    CHECK(copies.copies[4].id == "arm[1]");
    CHECK(copies.copies[5].id == "box[1]");
    CHECK(copies.copies[5].born == doctest::Approx(0.3));
    CHECK(track_of(copies, "box[1]", MotionProp::Y) != nullptr);
    CHECK(track_of(copies, "ball[3]", MotionProp::Fill) != nullptr);
    const MotionClip order = checked(s, rig, doc, "order");
    REQUIRE(order.order.size() == 4);
    CHECK(order.order[0].kind == vector::MotionOrder::Bottom);
    CHECK(order.order[1].kind == vector::MotionOrder::Above);
    CHECK(order.order[1].other == "p2");
    CHECK(order.order[1].at == doctest::Approx(0.1));
    CHECK(order.order[3].part == "p2[1]");

    // start_from: the order the other clip left, at 0.
    PoseScript after = script_of(R"(return { clips = {
  order = function(p) part("box"):to_bottom() part("p1"):above("p2") end,
  next = function(p) start_from("order") part("ball"):to_top() end,
} })");
    const MotionClip next = checked(after, rig, doc, "next");
    REQUIRE(next.order.size() == 3);
    CHECK(next.order[0].at == 0);
    CHECK(next.order[1].at == 0);
    CHECK(next.order[1].part == "p1");
}

TEST_CASE("Screen clips, part 5г: the screen's picture (lunasvg) is the tab's (ThorVG)") {
    Rig rig = loaded(kStage);
    PoseScript s = script_of(kStageCode);
    const PixelDoc doc = doc64();
    ScreenGift gift;
    std::string why;
    const bool built = build_screen(s, rig, doc, "Ёжик", "Ёжик.pixel.json", kStage, nullptr, gift, why);
    INFO(why);
    REQUIRE(built);
    CHECK(gift.svg == kStage);
    CHECK(gift.notes.empty());
    CHECK(vector::motion_file_version(gift.motion) == 2);
    // The copies and the linked part have their places in the file.
    for (const char* id : {"ball", "ball[1]", "ball[3]", "box[1]", "lid[1]", "arm[1]", "p2[1]"}) {
        INFO(id);
        CHECK(gift.motion.part(id) != nullptr);
    }
    CHECK(gift.motion.part("ball[2]")->pivot == gift.motion.part("ball")->pivot);
    for (const char* name : kStageClips) {
        SUBCASE(name) { same_as_tab(gift, s, rig, doc, name, {0.0, 0.05, 0.1, 0.17, 0.25, 0.3, 0.4, 0.45, 0.6}); }
    }
}

TEST_CASE("Screen clips, part 5г: a shape's drawn share is ThorVG's trimpath") {
    // Lines, curves, a closed contour, an open one, a shape of two contours, a rect and a circle; shares at the ends, on
    // a joint, empty and whole: ThorVG drawing the share of the shape against ThorVG drawing the outline cut here.
    struct Shape {
        const char* svg;
        f64 from, to;
    };
    const std::vector<Shape> shapes = {
        {R"(<path id="s" d="M 4 4 L 60 4 L 60 60" fill="none" stroke="#000000" stroke-width="3"/>)", 0.2, 0.7},
        {R"(<path id="s" d="M 4 4 L 60 4 L 60 60" fill="none" stroke="#000000" stroke-width="3"/>)", 0, 0.5},
        {R"(<path id="s" d="M 4 4 L 60 4 L 60 60" fill="none" stroke="#000000" stroke-width="3"/>)", 0.5, 1},
        {R"(<path id="s" d="M 6 50 C 10 0 40 70 58 10" fill="none" stroke="#000000" stroke-width="3"/>)", 0.1, 0.9},
        {R"(<path id="s" d="M 6 50 C 10 0 40 70 58 10 Q 30 60 6 50 Z" fill="#c03030" stroke="#000000" stroke-width="2"/>)", 0.3, 0.95},
        {R"(<path id="s" d="M 4 4 L 30 4 L 30 30 Z M 34 34 L 60 34 L 60 60 L 34 60 Z" fill="#3060c0" stroke="#000000" stroke-width="2"/>)", 0.25, 0.8},
        {R"(<rect id="s" x="8" y="10" width="44" height="36" rx="6" fill="#30a060" stroke="#000000" stroke-width="2"/>)", 0.1, 0.6},
        {R"(<circle id="s" cx="32" cy="32" r="24" fill="none" stroke="#000000" stroke-width="4"/>)", 0.0, 0.4},
        {R"(<ellipse id="s" cx="32" cy="32" rx="26" ry="14" fill="#a020a0" stroke="#000000" stroke-width="2"/>)", 0.6, 1},
        {R"(<line id="s" x1="4" y1="60" x2="60" y2="8" stroke="#000000" stroke-width="3"/>)", 0.3, 0.6},
        {R"(<polygon id="s" points="32,4 60,60 4,60" fill="#e0e000" stroke="#000000" stroke-width="2"/>)", 0.4, 0.9},
    };
    auto picture = [](const std::string& shape) {
        return R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">)" + shape + "</svg>";
    };
    std::string why;
    for (const Shape& sh : shapes) {
        INFO(sh.svg, " from ", sh.from, " to ", sh.to);
        Rig rig = loaded(picture(sh.svg));
        Pose pose;
        pose.parts["s"].draw_from = sh.from;
        pose.parts["s"].draw_to = sh.to;
        REQUIRE(rig.pose(pose, why));
        const assets::CookedTexture want = rig.draw(128, 128);

        const vector::SvgOutline outline = vector::scan_svg(picture(sh.svg));
        vector::Path whole;
        REQUIRE(vector::shape_path(outline, 1, whole, why));
        const vector::Path part = vector::trim(whole, sh.from, sh.to, true);
        // The same paint on a path of the share: its fill, then its stroke on top, as one shape draws them. Two paths,
        // as ThorVG draws the fill of an untrimmed shape with an opaque stroke 2 pixels wide or more without
        // antialiasing (the stroke covers its edges), but that of a trimmed one with it.
        const vector::SvgElement& e = outline.elements[1];
        const std::string d = "d=\"" + (part.contours.empty() ? std::string("M0 0") : vector::to_d(part, true)) + "\"";
        const std::string fill = e.tag == "line" || e.property("fill").empty() ? "none" : e.property("fill");
        Rig drawn = loaded(picture("<path " + d + " fill=\"" + fill + "\"/><path id=\"s\" " + d + " fill=\"none\" stroke=\"" +
                                   e.property("stroke") + "\" stroke-width=\"" + e.property("stroke-width") + "\"/>"));
        REQUIRE(drawn.pose({}, why));
        const assets::CookedTexture got = drawn.draw(128, 128);
        REQUIRE(got.rgba8.size() == want.rgba8.size());
        usize off = 0, painted = 0;
        for (usize i = 0; i < got.rgba8.size(); ++i) {
            off += std::abs(int(got.rgba8[i]) - int(want.rgba8[i])) > 2;
            painted += want.rgba8[i] != 0;
        }
        INFO(off, " bytes off of ", painted);
        CHECK(off == 0);
        CHECK(painted > 100);
    }
    // Empty and outside: no path.
    vector::Path line;
    REQUIRE(vector::shape_path(vector::scan_svg(picture(R"(<path id="s" d="M 0 0 L 10 0"/>)")), 1, line, why));
    CHECK(vector::trim(line, 0.5, 0.5, true).contours.empty());
    CHECK(vector::trim(line, 0.7, 0.2, true).contours.empty());
    const vector::Path half = vector::trim(line, 0, 0.5, true);
    REQUIRE(half.contours.size() == 1);
    CHECK(half.contours[0].start.x == doctest::Approx(0));
    CHECK(vector::to_d(half).find("5") != std::string::npos);
}

TEST_CASE("Screen clips, part 5г: the stage a pose draws the picture from") {
    Rig rig = loaded(kStage);
    PoseScript s = script_of(kStageCode);
    const PixelDoc doc = doc64();
    ScreenGift gift;
    std::string why;
    REQUIRE(build_screen(s, rig, doc, "Ёжик", "Ёжик.pixel.json", kStage, nullptr, gift, why));
    auto stage = [&](const char* clip, f64 t) { return ui::posed_stage(kStage, screen_pose(gift.motion, *gift.motion.clip(clip), t)); };
    auto count = [](const std::string& text, const std::string& what) {
        usize n = 0;
        for (usize at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    // Nothing beyond 5в: the file itself.
    CHECK(stage("dash", 0.2).empty());
    // Copies: the source's text again, its ids kept, right after it and the copies with smaller numbers; one not born
    // yet is not there.
    const std::string copies_early = stage("copies", 0.1);
    CHECK(count(copies_early, "<circle id=\"ball\"") == 4);
    CHECK(count(copies_early, "<rect id=\"box\"") == 1);
    const std::string copies_late = stage("copies", 0.4);
    CHECK(count(copies_late, "<rect id=\"box\"") == 2);
    CHECK(count(copies_late, "<circle id=\"lid\"") == 2);
    CHECK(copies_late.find("<circle id=\"ball\"") < copies_late.find("<path id=\"line\""));
    // A shape drawn along its length or morphed: a <path> of the same outline in its place.
    const std::string draw = stage("draw", 0.2);
    CHECK(draw.find("<path id=\"torso\"") != std::string::npos);
    CHECK(draw.find("<path id=\"ball\"") != std::string::npos);
    CHECK(draw.find("<rect id=\"torso\"") == std::string::npos);
    CHECK(draw.find("stroke-dasharray=\"5 3\"") != std::string::npos); // its own look kept
    // A mask: a <mask> at the end, the part pointing to it.
    const ui::SvgPose masked = screen_pose(gift.motion, *gift.motion.clip("mask_out"), 0.2);
    const std::string mask = ui::posed_stage(kStage, masked);
    REQUIRE(mask.find("<mask id=\"forge-stage-") != std::string::npos);
    const std::string url = ui::posed_attribute(kStage, masked, "box", "mask");
    CHECK(url.rfind("url(#forge-stage-", 0) == 0);
    CHECK(mask.find("maskUnits=\"userSpaceOnUse\"") != std::string::npos);
    // A tinted gradient: a copy of its own, which the shape paints with.
    const ui::SvgPose tinted = screen_pose(gift.motion, *gift.motion.clip("tint"), 0.2);
    CHECK(ui::posed_stage(kStage, tinted).find("href=\"#sky\"") != std::string::npos);
    CHECK(ui::posed_attribute(kStage, tinted, "band", "fill").rfind("url(#forge-stage-", 0) == 0);
    // The order: the elements as the steps leave them, the copies with them.
    const std::string order = stage("order", 0.3);
    CHECK(order.find("id=\"box\"") < order.find("id=\"band\""));
    CHECK(order.find("id=\"p2\"") < order.find("id=\"p1\""));
    CHECK(order.find("id=\"ball\"") > order.find("id=\"ghost\""));
    CHECK(count(order, "<rect id=\"p2\"") == 2);
    CHECK(order.find("<rect id=\"p2\"") < order.find("<rect id=\"p1\"")); // the copy at the bottom of its group
}

TEST_CASE("Screen clips file, part 5г: the new tracks, copies and order there and back, version 2 only with them") {
    MotionFile f;
    f.character = "Ёжик";
    f.from = "Ёжик.pixel.json";
    f.picture = "Ёжик.svg";
    f.parts = {{"a", {1, 2}, {}, {}}, {"a[1]", {1, 2}, {}, {}}};
    MotionClip c;
    c.name = "всё";
    c.length = 1;
    auto hold = [](const char* part, MotionProp p, MotionValue v) {
        MotionTrack t;
        t.part = part;
        t.prop = p;
        MotionPiece pc;
        pc.kind = MotionPiece::Kind::Hold;
        pc.value = v;
        t.pieces.push_back(pc);
        return t;
    };
    c.tracks = {hold("a", MotionProp::Dash, {3.5, {}, {}, 0, {}}),
                hold("a", MotionProp::DrawFrom, {0.25, {}, {}, 0, {}}),
                hold("a", MotionProp::DrawTo, {0.75, {}, {}, 0, {}}),
                hold("a", MotionProp::Tint, {0.5, {255, 128, 0, 255}, {}, 0, {}}),
                hold("a", MotionProp::Flash, {0.8, {}, {}, 0, {}}),
                hold("a", MotionProp::FlashColor, {1, {255, 0, 0, 128}, {}, 0, {}}),
                hold("a", MotionProp::Mask, {2, {}, "lid", 0, {}}),
                hold("a", MotionProp::Link, {0, {}, "hand", 0, {1, 0, 0, 1, 3, -2}}),
                hold("a[1]", MotionProp::X, {4, {}, {}, 0, {}})};
    c.copies = {{"a[1]", "a", 1, 0.25}};
    c.order = {{0, "a", vector::MotionOrder::Top, ""}, {0.5, "a[1]", vector::MotionOrder::Below, "b"}};
    f.clips.push_back(c);
    const std::string text = vector::write_motion_file(f);
    INFO(text);
    CHECK(text.find("\"version\": 2") != std::string::npos);
    CHECK(text.find(R"("hold": [2, "lid"])") != std::string::npos);
    CHECK(text.find(R"("hold": ["hand", 1, 0, 0, 1, 3, -2])") != std::string::npos);
    CHECK(text.find(R"({"id": "a[1]", "source": "a", "number": 1, "born": 0.25})") != std::string::npos);
    CHECK(text.find(R"({"at": 0.5, "part": "a[1]", "kind": "below", "other": "b"})") != std::string::npos);
    MotionFile back;
    std::string why;
    REQUIRE(vector::read_motion_file(text, back, why));
    CHECK(back == f);
    CHECK(vector::write_motion_file(back) == text);
    // Before birth a copy is not drawn; its step of the order neither.
    CHECK_FALSE(vector::motion_copy_drawn(c.copies[0], 0.2));
    CHECK(vector::motion_order_at(c, 0.6).size() == 2);
    c.copies[0].born = 0.7;
    CHECK(vector::motion_order_at(c, 0.6).size() == 1);

    // Nothing new: version 1, as 5в wrote it.
    MotionFile old = f;
    old.clips[0].tracks.resize(0);
    old.clips[0].copies.clear();
    old.clips[0].order.clear();
    CHECK(vector::write_motion_file(old).find("\"version\": 1") != std::string::npos);
    CHECK(vector::write_motion_file(old).find("copies") == std::string::npos);

    // What it cannot read names the field.
    const std::string part_a1 = R"("parts": {"a": {"pivot": [0, 0], "frame": [1, 0, 0, 1, 0, 0], "own": [1, 0, 0, 1, 0, 0]}, "a[1]": {"pivot": [0, 0], "frame": [1, 0, 0, 1, 0, 0], "own": [1, 0, 0, 1, 0, 0]}},)";
    const std::string y = R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": 1}]})";
    auto with = [&](const std::string& rest) { return one_track(y, rest, part_a1); };
    REQUIRE(vector::read_motion_file(with(R"(, "copies": [{"id": "a[1]", "source": "a", "number": 1, "born": 0}])"), back, why));
    CHECK(read_error(with(R"(, "copies": [{"id": "a[1]", "number": 1, "born": 0}])")).find("clips.c.copies[0]: source") != std::string::npos);
    CHECK(read_error(with(R"(, "copies": [{"id": "a[2]", "source": "a", "number": 1, "born": 0}])")).find("нет в parts") != std::string::npos);
    CHECK(read_error(with(R"(, "copies": [{"id": "a[1]", "source": "a", "number": 2, "born": 0}])")).find("clips.c.copies[0]: number") != std::string::npos);
    CHECK(read_error(with(R"(, "copies": [{"id": "a[1]", "source": "a", "number": 1, "born": 3}])")).find("clips.c.copies[0]: born") != std::string::npos);
    CHECK(read_error(with(R"(, "order": [{"at": 0, "part": "a", "kind": "up"}])")).find("clips.c.order[0]: kind") != std::string::npos);
    CHECK(read_error(with(R"(, "order": [{"at": 0, "part": "a", "kind": "above"}])")).find("clips.c.order[0]: other") != std::string::npos);
    CHECK(read_error(with(R"(, "order": [{"at": 0, "part": "a", "kind": "top", "other": "b"}])")).find("clips.c.order[0]: other") != std::string::npos);
    CHECK(read_error(with(R"(, "order": [{"at": 0.5, "part": "a", "kind": "top"}, {"at": 0.2, "part": "a", "kind": "top"}])")).find("clips.c.order[1]") !=
          std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "mask", "pieces": [{"at": 0, "hold": [3, "lid"]}]})")).find("clips.c.tracks[0].pieces[0]") !=
          std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "mask", "pieces": [{"at": 0, "length": 0.5, "ease": "linear", "from": [0, ""], "to": [1, "lid"]}]})"))
              .find("только hold") != std::string::npos);
    CHECK(read_error(one_track(R"({"part": "a", "prop": "link", "pieces": [{"at": 0, "hold": ["b", 1, 0, 0, 1, 0, 0]}]})", "", R"("parts": {},)"))
              .find("нет в parts") != std::string::npos);
}

TEST_CASE("Screen clips, part 5г: the part a clip names that the picture has not") {
    const vector::SvgOutline svg = vector::scan_svg(kStage);
    MotionClip c;
    std::string part, role;
    CHECK_FALSE(vector::motion_clip_missing(c, svg, part, role));
    auto mask = [](const char* target) {
        MotionTrack t;
        t.part = "box";
        t.prop = MotionProp::Mask;
        MotionPiece pc;
        pc.value = {1, {}, target, 0, {}};
        t.pieces.push_back(pc);
        return t;
    };
    c.tracks = {mask("lid")};
    CHECK_FALSE(vector::motion_clip_missing(c, svg, part, role));
    c.tracks = {mask("веко")};
    REQUIRE(vector::motion_clip_missing(c, svg, part, role));
    CHECK(part == "веко");
    CHECK(role == "маска «box»");
    c.tracks.clear();
    c.copies = {{"искра[1]", "искра", 1, 0}};
    REQUIRE(vector::motion_clip_missing(c, svg, part, role));
    CHECK(part == "искра");
    CHECK(role == "копия «искра[1]»");
    c.copies.clear();
    c.order = {{0, "p1", vector::MotionOrder::Above, "p3"}};
    REQUIRE(vector::motion_clip_missing(c, svg, part, role));
    CHECK(part == "p3");
    c.order = {{0, "sky", vector::MotionOrder::Top, ""}}; // a gradient draws nothing where it is: no part
    REQUIRE(vector::motion_clip_missing(c, svg, part, role));
    CHECK(part == "sky");
}
