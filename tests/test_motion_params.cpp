// Part 5ж of docs/models/движение-модель.md («Параметры из игры»): a game plays a character's clip at other values of
// its code's parameters than the tab gave it at. The file keeps the parameters and how each number of each piece changes
// per unit of each; a clip that a parameter does not change straight keeps it fixed, with why. The file reads back as
// written and refuses what it cannot be with the field named.

#include "forge/vector/motion_file.h"

#include <doctest/doctest.h>

#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using vector::MotionBy;
using vector::MotionClip;
using vector::MotionDelta;
using vector::MotionFile;
using vector::MotionPiece;
using vector::MotionProp;
using vector::MotionTrack;
using vector::MotionValue;

namespace {

std::string read_error(std::string_view json) {
    MotionFile f;
    std::string why;
    CHECK_FALSE(vector::read_motion_file(json, f, why));
    return why;
}

MotionDelta dv(f64 v) {
    MotionDelta d;
    d.v = v;
    return d;
}

} // namespace

TEST_CASE("Screen clips file, part 5ж: the parameters, the pieces' changes and the fixed ones there and back, version 4 only with them") {
    const std::string kPartA = R"({"a": {"pivot": [0, 0], "frame": [1, 0, 0, 1, 0, 0], "own": [1, 0, 0, 1, 0, 0]}, "b": {"pivot": [1, 2], "frame": [1, 0, 0, 1, 0, 0], "own": [1, 0, 0, 1, 0, 0]}})";
    auto file = [&](std::string_view params, std::string_view tracks, std::string_view clip_rest = "") {
        return R"({"forge": "motion", "version": 4, "character": "Ёжик", "from": "Ёжик.pixel.json", "picture": "Ёжик.svg", )" + std::string(params) +
               R"( "parts": )" + kPartA + R"(, "curves": [], "clips": {"c": {"length": 1, "loop": false, "tracks": [)" + std::string(tracks) +
               R"(], "marks": [], "not_drawn": [])" + std::string(clip_rest) + "}}}";
    };
    const std::string params = R"("params": [{"name": "height", "value": 6}, {"name": "count", "value": 3}],)";
    std::string frames = R"({"part": "a", "prop": "x", "pieces": [{"at": 0, "end": 0.025, "frames": [1, 2, 3, 4], "by": {"count": [0, 0, 1, 0], "height": [0.5, 0.25, 0, -1]}}, {"at": 0.025, "hold": 4}]})";
    REQUIRE(vector::motion_frame_count(0, 0.025) == 4);
    const std::string tracks =
        R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6, "by": {"height": -1}}, {"at": 0.5, "length": 0.25, "from": -6, "to": 0, "ease": "linear", "by_from": {"height": -1}, "by_to": {"count": 2}}]}, )"
        R"({"part": "b", "prop": "stroke_width", "pieces": [{"at": 0, "hold": [1, 4], "by": {"height": [0, 0.5]}}]}, )"
        R"({"part": "b", "prop": "fill", "pieces": [{"at": 0, "hold": [0.5, "#ff0000ff"], "by": {"count": 0.125}}]}, )"
        R"({"part": "b", "prop": "link", "pieces": [{"at": 0, "hold": ["a", 1, 0, 0, 1, 3, 4], "by": {"height": [0, 0, 0, 0, 1, 0.5]}}]}, )"
        R"({"part": "b", "prop": "visible", "pieces": [{"at": 0, "hold": true}]}, )" + frames;
    const std::string fixed = R"(, "fixed": {"count": "число переходов"})";
    MotionFile f;
    std::string why;
    const bool read = vector::read_motion_file(file(params, tracks), f, why);
    INFO(why);
    REQUIRE(read);
    REQUIRE(f.params.size() == 2);
    CHECK(f.params[0] == vector::MotionParam{"height", 6});
    CHECK(f.params[1] == vector::MotionParam{"count", 3});
    CHECK(f.param("count") == &f.params[1]);
    CHECK(f.param("heigth") == nullptr);
    const MotionClip& c = f.clips[0];
    // A hold's change, a transition's from and to merged a parameter an entry, in the order of params whatever the file's.
    const MotionPiece& hold = c.tracks[0].pieces[0];
    REQUIRE(hold.by.size() == 1);
    CHECK(hold.by[0].param == "height");
    CHECK(hold.by[0].value == dv(-1));
    const MotionPiece& ease = c.tracks[0].pieces[1];
    REQUIRE(ease.by.size() == 2);
    CHECK(ease.by[0].param == "height");
    CHECK(ease.by[0].from == dv(-1));
    CHECK(ease.by[0].to.zero());
    CHECK(ease.by[1].param == "count");
    CHECK(ease.by[1].from.zero());
    CHECK(ease.by[1].to == dv(2));
    MotionDelta width;
    width.n = 0.5;
    CHECK(c.tracks[1].pieces[0].by[0].value == width);
    CHECK(c.tracks[2].pieces[0].by[0].value == dv(0.125));
    MotionDelta link;
    link.m[4] = 1;
    link.m[5] = 0.5;
    CHECK(c.tracks[3].pieces[0].by[0].value == link);
    CHECK(c.tracks[4].pieces[0].by.empty());
    const MotionPiece& fr = c.tracks[5].pieces[0];
    REQUIRE(fr.by.size() == 2);
    CHECK(fr.by[0].param == "height");
    CHECK(fr.by[0].frames == std::vector<MotionDelta>{dv(0.5), dv(0.25), dv(0), dv(-1)});
    CHECK(fr.by[1].param == "count");
    CHECK(fr.by[1].frames == std::vector<MotionDelta>{dv(0), dv(0), dv(1), dv(0)});
    CHECK(c.fixed.empty());

    // There and back, version 4; the fixed ones too.
    const std::string text = vector::write_motion_file(f);
    CHECK(text.find("\"version\": 4") != std::string::npos);
    CHECK(text.find(R"("params": [{"name": "height", "value": 6}, {"name": "count", "value": 3}])") != std::string::npos);
    CHECK(text.find(R"("by_from": {"height": -1}, "by_to": {"count": 2})") != std::string::npos);
    CHECK(text.find(R"("by": {"height": [0, 0.5]})") != std::string::npos);
    CHECK(text.find(R"("by": {"height": [0, 0, 0, 0, 1, 0.5]})") != std::string::npos);
    CHECK(text.find(R"("by": {"height": [0.5, 0.25, 0, -1], "count": [0, 0, 1, 0]})") != std::string::npos);
    MotionFile back;
    REQUIRE(vector::read_motion_file(text, back, why));
    CHECK(back == f);
    CHECK(vector::write_motion_file(back) == text);
    MotionFile g;
    REQUIRE(vector::read_motion_file(file(params, R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6, "by": {"height": -1}}]})", fixed), g, why));
    CHECK(g.clips[0].fixed == std::map<std::string, std::string>{{"count", "число переходов"}});
    const std::string gtext = vector::write_motion_file(g);
    CHECK(gtext.find(R"("fixed": {"count": "число переходов"})") != std::string::npos);
    MotionFile gback;
    REQUIRE(vector::read_motion_file(gtext, gback, why));
    CHECK(gback == g);
    // A parameter that changes nothing is as none: the writer leaves it out, and the reader reads it so.
    MotionFile zero;
    REQUIRE(vector::read_motion_file(file(params, R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6, "by": {"height": 0, "count": -0.0}}]})"), zero, why));
    CHECK(zero.clips[0].tracks[0].pieces[0].by.empty());
    MotionFile zero_written = f;
    zero_written.clips[0].tracks[0].pieces[0].by[0].value = {};
    CHECK(vector::write_motion_file(zero_written).find(R"("hold": -6})") != std::string::npos);
    // Without parameters as before: version 3 at most, no "params".
    MotionFile plain = f;
    plain.params.clear();
    for (MotionTrack& t : plain.clips[0].tracks)
        for (MotionPiece& p : t.pieces) p.by.clear();
    CHECK(vector::motion_file_version(plain) == 2);
    CHECK(vector::write_motion_file(plain).find("params") == std::string::npos);
    MotionFile only_params = plain;
    only_params.params = f.params;
    CHECK(vector::motion_file_version(only_params) == 4);

    // What it cannot read.
    const std::string y = R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6}]})";
    CHECK(read_error(file(R"("params": {},)", y)) == "params: нужен список параметров [{\"name\": …, \"value\": …}]");
    CHECK(read_error(file(R"("params": [6],)", y)) == "params[0]: нужен объект {\"name\": …, \"value\": …}");
    CHECK(read_error(file(R"("params": [{"name": "", "value": 6}],)", y)) == "params[0]: name: нужно имя параметра строкой");
    CHECK(read_error(file(R"("params": [{"name": "h", "value": "6"}],)", y)) == "params[0]: value: нужно число — с ним вкладка отдала клипы");
    CHECK(read_error(file(R"("params": [{"name": "h", "value": 6}, {"name": "h", "value": 1}],)", y)) == "params[1]: параметр «h» дважды");
    auto with = [&](std::string_view piece_rest, std::string_view prop = "y", std::string_view value = "-6") {
        return file(params, R"({"part": "a", "prop": ")" + std::string(prop) + R"(", "pieces": [{"at": 0, "hold": )" + std::string(value) +
                                std::string(piece_rest) + "}]}");
    };
    CHECK(read_error(with(R"(, "by": {"heigth": 1})")) == "clips.c.tracks[0].pieces[0].by.heigth: нет параметра «heigth»: есть «height», «count»");
    CHECK(read_error(file("", R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6, "by": {"height": 1}}]})")) ==
          "clips.c.tracks[0].pieces[0].by.height: нет параметра «height»: в params ни одного");
    CHECK(read_error(with(R"(, "by": {"height": "1"})")) == "clips.c.tracks[0].pieces[0].by.height: нужно число");
    CHECK(read_error(with(R"(, "by": {"height": 1, "height": 2})")) == "clips.c.tracks[0].pieces[0].by.height: параметр дважды");
    CHECK(read_error(with(R"(, "by": [1])")) == "clips.c.tracks[0].pieces[0].by: нужен объект {\"параметр\": изменение на единицу}");
    CHECK(read_error(with(R"(, "by_from": {"height": 1})")) == "clips.c.tracks[0].pieces[0].by_from: by_from и by_to только у перехода, у этого куска by");
    CHECK(read_error(with(R"(, "by": {"height": 1})", "visible", "true")) == "clips.c.tracks[0].pieces[0].by: у visible нет чисел, которые менял бы параметр");
    CHECK(read_error(with(R"(, "by": {"height": 1})", "mask", R"([1, "b"])")) == "clips.c.tracks[0].pieces[0].by: у mask нет чисел, которые менял бы параметр");
    CHECK(read_error(with(R"(, "by": {"height": 1})", "stroke_width", "[1, 4]")) == "clips.c.tracks[0].pieces[0].by.height: нужно [доля, толщина]");
    CHECK(read_error(with(R"(, "by": {"height": [1, 2]})", "link", R"(["a", 1, 0, 0, 1, 3, 4])")) ==
          "clips.c.tracks[0].pieces[0].by.height: нужно шесть чисел a, b, c, d, e, f");
    CHECK(read_error(file(params, R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "length": 0.25, "from": -6, "to": 0, "ease": "linear", "by": {"height": 1}}]})")) ==
          "clips.c.tracks[0].pieces[0].by: у перехода by_from и by_to");
    CHECK(read_error(file(params, R"({"part": "a", "prop": "x", "pieces": [{"at": 0, "end": 0.025, "frames": [1, 2, 3, 4], "by": {"height": [1, 2, 3]}}, {"at": 0.025, "hold": 4}]})")) ==
          "clips.c.tracks[0].pieces[0].by.height: нужен список изменений, по одному на кадр (их 4), каждое число");
    CHECK(read_error(file(params, R"({"part": "a", "prop": "x", "pieces": [{"at": 0, "end": 0.025, "frames": [1, 2, 3, 4], "by": {"height": [1, 2, "3", 4]}}, {"at": 0.025, "hold": 4}]})")) ==
          "clips.c.tracks[0].pieces[0].by.height[2]: нужно число");
    CHECK(read_error(file(params, R"({"part": "a", "prop": "y", "pieces": [{"at": 0, "hold": -6, "by": {"count": 1}}]})", fixed)) ==
          "clips.c.tracks[0].pieces[0].by.count: параметр в fixed клипа: клип от него не меняется");
    CHECK(read_error(file(params, y, R"(, "fixed": {"cnt": "число переходов"})")) == "clips.c.fixed.cnt: нет параметра «cnt»: есть «height», «count»");
    CHECK(read_error(file(params, y, R"(, "fixed": {"count": ""})")) == "clips.c.fixed.count: нужно, почему, строкой");
    CHECK(read_error(file(params, y, R"(, "fixed": {"count": "цвет", "count": "метки"})")) == "clips.c.fixed.count: параметр дважды");
    CHECK(read_error(file(params, y, R"(, "fixed": ["count"])")) == "clips.c: fixed: нужен объект {\"параметр\": почему}");
}

TEST_CASE("Screen clips, part 5ж: a clip at other values of the parameters, each number by its change, the fixed ones and unknown ones left") {
    MotionFile f;
    f.params = {{"height", 6}, {"count", 3}};
    MotionClip c;
    c.name = "c";
    c.length = 1;
    MotionTrack y;
    y.part = "a";
    y.prop = MotionProp::Y;
    MotionPiece hold;
    hold.value.v = -6;
    hold.by = {{"height", dv(-1), {}, {}, {}}, {"count", dv(0.5), {}, {}, {}}};
    MotionPiece ease;
    ease.kind = MotionPiece::Kind::Ease;
    ease.at = 0.5;
    ease.length = 0.25;
    ease.from.v = -6;
    ease.to.v = 0;
    ease.by = {{"height", {}, dv(-1), dv(0.25), {}}};
    y.pieces = {hold, ease};
    MotionTrack w;
    w.part = "b";
    w.prop = MotionProp::StrokeWidth;
    MotionPiece wp;
    wp.value.v = 1;
    wp.value.n = 4;
    MotionDelta wd;
    wd.v = -0.125;
    wd.n = 0.5;
    wp.by = {{"height", wd, {}, {}, {}}};
    w.pieces = {wp};
    MotionTrack l;
    l.part = "b";
    l.prop = MotionProp::Link;
    MotionPiece lp;
    lp.value.target = "a";
    lp.value.m = {1, 0, 0, 1, 3, 4};
    MotionDelta ld;
    for (usize i = 0; i < 6; ++i) ld.m[i] = 0.1 * static_cast<f64>(i + 1);
    lp.by = {{"height", ld, {}, {}, {}}};
    l.pieces = {lp};
    MotionTrack x;
    x.part = "a";
    x.prop = MotionProp::X;
    MotionPiece fp;
    fp.kind = MotionPiece::Kind::Frames;
    fp.end = 0.025;
    for (f64 v : {1.0, 2.0, 3.0, 4.0}) {
        MotionValue mv;
        mv.v = v;
        fp.frames.push_back(mv);
    }
    fp.by = {{"height", {}, {}, {}, {dv(1), dv(0), dv(-1), dv(2)}}};
    x.pieces = {fp};
    c.tracks = {y, w, l, x};
    f.clips = {c};

    const MotionClip at8 = vector::motion_clip_with(f, c, {{"height", 8}});
    CHECK(at8.tracks[0].pieces[0].value.v == -8);
    CHECK(at8.tracks[0].pieces[1].from.v == -8);
    CHECK(at8.tracks[0].pieces[1].to.v == 0.5);
    CHECK(at8.tracks[1].pieces[0].value.v == 0.75);
    CHECK(at8.tracks[1].pieces[0].value.n == 5);
    const vector::MotionMatrix m = at8.tracks[2].pieces[0].value.m;
    CHECK(m.a == doctest::Approx(1.2));
    CHECK(m.b == doctest::Approx(0.4));
    CHECK(m.c == doctest::Approx(0.6));
    CHECK(m.d == doctest::Approx(1.8));
    CHECK(m.e == doctest::Approx(4));
    CHECK(m.f == doctest::Approx(5.2));
    CHECK(at8.tracks[2].pieces[0].value.target == "a");
    std::vector<f64> xs;
    for (const MotionValue& v : at8.tracks[3].pieces[0].frames) xs.push_back(v.v);
    CHECK(xs == std::vector<f64>{3, 2, 1, 8});
    // Both parameters, one below the file's; one the file has not is left; a fixed one is left.
    const MotionClip both = vector::motion_clip_with(f, c, {{"height", 3}, {"count", 5}, {"speed", 9}});
    CHECK(both.tracks[0].pieces[0].value.v == -6 + 3 + 1);
    CHECK(both.tracks[0].pieces[1].to.v == -0.75);
    MotionFile fx = f;
    fx.clips[0].fixed["count"] = "число переходов";
    CHECK(vector::motion_clip_with(fx, fx.clips[0], {{"height", 3}, {"count", 5}}).tracks[0].pieces[0].value.v == -3);
    // At the file's values, and with none given, the clip as it is.
    CHECK(vector::motion_clip_with(f, c, {{"height", 6}, {"count", 3}}) == c);
    CHECK(vector::motion_clip_with(f, c, {}) == c);
    // Visible, a mask, a drawing have no numbers; the rest have.
    CHECK_FALSE(vector::motion_prop_has_numbers(MotionProp::Visible));
    CHECK_FALSE(vector::motion_prop_has_numbers(MotionProp::Mask));
    CHECK_FALSE(vector::motion_prop_has_numbers(MotionProp::Drawing));
    for (MotionProp p : {MotionProp::X, MotionProp::Opacity, MotionProp::Fill, MotionProp::StrokeWidth, MotionProp::Morph, MotionProp::Link, MotionProp::FlashColor})
        CHECK(vector::motion_prop_has_numbers(p));
}
