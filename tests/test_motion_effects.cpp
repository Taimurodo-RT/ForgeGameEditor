// Part 4б of docs/models/движение-модель.md («Эффекты и маски»): tint, flash, blur, shadow and masks go as the
// properties of 4а do (recording, frame, picture, timeline, code edits), and a part's colours take an alpha. A tint or
// a flash is the same picture, byte for byte, as the SVG with the colours it gives written in; a blur, a shadow and a
// mask as ThorVG's own, set directly on the same SVG with a group round the part, their numbers worked out here.

#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/pixel/timeline.h"
#include "forge/vector/trig.h"

#include <doctest/doctest.h>
#include <thorvg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::pixel;

namespace {

// A body of three shapes over one another (the head with a stroke), a band with a gradient, and shapes hidden to mask
// with: lid alone, lid1 and lid2 in the group lids.
struct El {
    const char* id;
    const char* shape; // the element up to its paint
    const char* paint; // its paint attributes
};
const El kBody[] = {
    {"torso", R"(<rect id="torso" x="20" y="20" width="16" height="20")", R"(fill="#3060c0")"},
    {"arm", R"(<rect id="arm" x="16" y="22" width="8" height="14")", R"(fill="#c03030")"},
    {"head", R"(<circle id="head" cx="24" cy="18" r="6")", R"(fill="#e0b080" stroke="#402000" stroke-width="1.5")"},
};
const El kBand = {"band", R"(<rect id="band" x="8" y="46" width="48" height="8")", R"x(fill="url(#sky)")x"};
const El kLid = {"lid", R"(<circle id="lid" cx="27" cy="18" r="4")", R"(display="none")"};
const El kLids[] = {
    {"lid1", R"(<circle id="lid1" cx="20" cy="17" r="2.5")", R"(display="none")"},
    {"lid2", R"(<circle id="lid2" cx="28" cy="19" r="2.5")", R"(display="none")"},
};
constexpr const char* kSky = R"(<linearGradient id="sky" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#2050a0"/>)"
                             R"(<stop offset="1" stop-color="#a0d0f0" stop-opacity="0.6"/></linearGradient>)";

// The picture with changes: per id its paint replaced and attributes added ("body" for the group's own), elements
// wrapped in a group "<id>_g" (the scene a reference sets effects and masks on), definitions and elements added.
struct Pic {
    std::map<std::string, std::string> paint, extra;
    std::vector<std::string> wrap;
    std::string defs, tail;

    bool wrapped(const std::string& id) const { return std::find(wrap.begin(), wrap.end(), id) != wrap.end(); }
    std::string attrs(const std::string& id) const {
        const auto it = extra.find(id);
        return it != extra.end() ? " " + it->second : std::string();
    }
    std::string el(const El& e) const {
        const auto p = paint.find(e.id);
        std::string s = std::string(e.shape) + " " + (p != paint.end() ? p->second : e.paint) + attrs(e.id) + "/>";
        if (wrapped(e.id)) s = "<g id=\"" + std::string(e.id) + "_g\">" + s + "</g>";
        return "    " + s + "\n";
    }
    std::string text() const {
        std::string s = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">)"
                        "\n  <defs>" + std::string(kSky) + defs + "</defs>\n";
        if (wrapped("body")) s += "  <g id=\"body_g\">\n";
        s += "  <g id=\"body\"" + attrs("body") + ">\n";
        for (const El& e : kBody) s += el(e);
        s += "  </g>\n";
        if (wrapped("body")) s += "  </g>\n";
        s += el(kBand) + el(kLid) + "  <g id=\"lids\">\n" + el(kLids[0]) + el(kLids[1]) + "  </g>\n" + tail;
        return s + "</svg>";
    }
};

std::string picture() { return Pic{}.text(); }

Rig loaded(std::string_view svg) {
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

// A script of one clip, "c", its body from line 2 on.
std::string clip_of(std::string_view body) { return "return { clips = { c = function(p, rng)\n" + std::string(body) + "\nend } }"; }

Recording recorded(PoseScript& s, const Rig* rig) {
    Recording rec;
    const bool ok = s.record(rig, "c", {}, 1, {}, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(PoseScript& s, const Rig* rig) {
    Recording rec;
    CHECK_FALSE(s.record(rig, "c", {}, 1, {}, rec));
    return s.error();
}

Pose frame(PoseScript& s, const Recording& rec, const Rig* rig, f64 t) {
    Pose pose;
    const bool ok = s.frame(rec, rig, t, {}, 1, pose);
    INFO(s.error());
    REQUIRE(ok);
    return pose;
}

const Block& block(const Recording& rec, std::string_view part, Prop p, usize nth = 0) {
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == part && b.prop == p && nth-- == 0) return b;
    FAIL("no block ", part, ".", prop_word(p));
    return rec.blocks.front();
}

usize bytes_off(const assets::CookedTexture& a, const assets::CookedTexture& b) {
    usize off = 0;
    for (usize i = 0; i < a.rgba8.size() && i < b.rgba8.size(); ++i) off += a.rgba8[i] != b.rgba8[i];
    return off;
}

// The posed rig and the reference drawn alike: smooth at 128 × 128 and baked at 64 × 64, byte for byte.
void same_picture(Rig& posed, const std::string& reference) {
    Rig ref = loaded(reference);
    const assets::CookedTexture a = posed.draw(128, 128), b = ref.draw(128, 128);
    CHECK(b.rgba8 != loaded(picture()).draw(128, 128).rgba8); // the reference shows it: not the picture at rest
    INFO("bytes off: ", bytes_off(a, b));
    CHECK(a.rgba8 == b.rgba8);
    CHECK(posed.bake(64, 64, {}).rgba8 == ref.bake(64, 64, {}).rgba8);
}

// %.9g of a float: it reads back as the same float.
std::string f32_text(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<f64>(static_cast<f32>(v)));
    return buf;
}

std::string matrix_text(const Affine& m) {
    return "transform=\"matrix(" + f32_text(m.a) + " " + f32_text(m.b) + " " + f32_text(m.c) + " " + f32_text(m.d) + " " +
           f32_text(m.e) + " " + f32_text(m.f) + ")\"";
}

std::string rgb(Rgba8 c) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "rgb(%d,%d,%d)", c.r, c.g, c.b);
    return buf;
}

// An opacity that ThorVG reads back as alpha a.
std::string opacity(u8 a) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", a / 255.0);
    return buf;
}

// The model's tint and flash on one colour, each step rounded, worked out apart from the rig: the tint multiplies each
// channel, its alpha too, by white + (tint − white) · k; the flash takes red, green and blue k of the way to f.
Rgba8 tinted(Rgba8 c, Rgba8 t, f64 k) {
    auto ch = [k](u8 v, u8 tv) { return static_cast<u8>(std::lround(v * (255 + (tv - 255.0) * k) / 255.0)); };
    return {ch(c.r, t.r), ch(c.g, t.g), ch(c.b, t.b), ch(c.a, t.a)};
}
Rgba8 flashed(Rgba8 c, Rgba8 f, f64 k) {
    auto ch = [k](u8 v, u8 fv) { return static_cast<u8>(std::lround(v + (fv - v) * k)); };
    return {ch(c.r, f.r), ch(c.g, f.g), ch(c.b, f.b), c.a};
}

// The SVG drawn by ThorVG as the rig draws it (the picture fitted into w × h), after `touch` set effects and masks on
// its paints directly.
using Touch = std::function<void(const std::vector<tvg::Paint*>&)>;

tvg::Paint* paint_of(const std::vector<tvg::Paint*>& all, const char* id) {
    for (tvg::Paint* p : all)
        if (p->id == tvg::Accessor::id(id)) return p;
    FAIL("no paint ", id);
    return nullptr;
}

tvg::Scene* scene_of(const std::vector<tvg::Paint*>& all, const char* id) {
    tvg::Paint* p = paint_of(all, id);
    REQUIRE(p->type() == tvg::Type::Scene);
    return static_cast<tvg::Scene*>(p);
}

assets::CookedTexture direct(const std::string& svg, u32 w, u32 h, const Touch& touch) {
    const Rig fit = loaded(svg);
    const Affine px = fit.to_pixels(w, h);
    tvg::Initializer::init(0);
    assets::CookedTexture out{w, h, {}};
    {
        tvg::Picture* pic = tvg::Picture::gen();
        pic->ref();
        REQUIRE(pic->load(svg.data(), static_cast<u32>(svg.size()), "svg", nullptr, true) == tvg::Result::Success);
        std::vector<tvg::Paint*> all;
        tvg::Accessor* acc = tvg::Accessor::gen();
        acc->set(pic, [](const tvg::Paint* p, void* data) {
            static_cast<std::vector<tvg::Paint*>*>(data)->push_back(const_cast<tvg::Paint*>(p));
            return true;
        }, &all);
        delete acc;
        REQUIRE(all.size() > 4);
        const tvg::Matrix d = all[3]->transform();
        const Affine doc{d.e11, d.e21, d.e12, d.e22, d.e13, d.e23};
        const Affine m = px * inverse(doc);
        const tvg::Matrix t{static_cast<f32>(m.a), static_cast<f32>(m.c), static_cast<f32>(m.e),
                            static_cast<f32>(m.b), static_cast<f32>(m.d), static_cast<f32>(m.f), 0, 0, 1};
        touch(all);
        std::vector<u32> buffer(static_cast<usize>(w) * h, 0);
        tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
        canvas->target(buffer.data(), w, w, h, tvg::ColorSpace::ABGR8888S);
        canvas->add(pic);
        pic->transform(t);
        canvas->update();
        canvas->draw(true);
        canvas->sync();
        const u8* bytes = reinterpret_cast<const u8*>(buffer.data());
        out.rgba8.assign(bytes, bytes + static_cast<usize>(w) * h * 4);
        canvas->remove(pic);
        delete canvas;
        pic->unref();
    }
    tvg::Initializer::term();
    return out;
}

// ThorVG's blur and drop shadow with the numbers of picture units on a scene drawn in them: the sigma as it is, the
// offset as ThorVG's angle (clockwise from up) and distance.
void blur_on(tvg::Scene* s, f64 sigma) { s->add(tvg::SceneEffect::GaussianBlur, sigma, 0, 0, 100); }
void shadow_on(tvg::Scene* s, Rgba8 c, f64 x, f64 y, f64 sigma) {
    f64 angle = std::atan2(x, -y) * 180 / vector::trig::kPi;
    if (angle < 0) angle += 360;
    s->add(tvg::SceneEffect::DropShadow, int(c.r), int(c.g), int(c.b), int(c.a), angle, std::hypot(x, y), sigma, 100);
}

// A paint drawn as a mask of another: a copy of it, the paint itself hidden.
void mask_with(const std::vector<tvg::Paint*>& all, const char* holder, const char* mask, bool invert) {
    tvg::Paint* m = paint_of(all, mask);
    tvg::Paint* copy = m->duplicate();
    m->visible(false);
    REQUIRE(paint_of(all, holder)->mask(copy, invert ? tvg::MaskMethod::InvAlpha : tvg::MaskMethod::Alpha) == tvg::Result::Success);
}

// A pixel's colour, "#rrggbb" or "#rrggbbaa" (doctest prints it).
std::string at_px(const assets::CookedTexture& t, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * t.width + x) * 4;
    return color_hex({t.rgba8[i], t.rgba8[i + 1], t.rgba8[i + 2], t.rgba8[i + 3]});
}
u8 alpha_at(const assets::CookedTexture& t, u32 x, u32 y) { return t.rgba8[(static_cast<usize>(y) * t.width + x) * 4 + 3]; }

constexpr Rgba8 kTorso{0x30, 0x60, 0xc0, 255};
constexpr Rgba8 kHead{0xe0, 0xb0, 0x80, 255};

} // namespace

TEST_CASE("Motion effects: the new properties are recorded and read as x and fill are") {
    const Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(all(
  part("body").tint("#ff8040", 1, "linear"),
  part("arm").flash(1, 1, "linear"),
  part("arm").flash_color("#00ff00", 1, "linear"),
  part("torso").blur(2, 1, "linear"),
  part("head"):to({ shadow = "#00000080", shadow_x = 2, shadow_y = -1, shadow_blur = 1 }, 1, "linear")
)
log(part("body").tint(), part("arm").flash(), part("arm").flash_color(), part("torso").blur(), part("head").shadow(),
  part("head").shadow_x(), part("head").shadow_y(), part("head").shadow_blur())
all(part("body").tint(nil, 1, "linear"), part("head").shadow(nil, 1, "linear"), part("arm").flash(0, 1, "linear")))"));
    const Recording rec = recorded(s, &rig);
    CHECK(rec.length == doctest::Approx(2));
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "#ff8040\t1\t#00ff00\t2\t#00000080\t2\t-1\t1");

    Pose p = frame(s, rec, &rig, 0.5);
    CHECK(p.parts["body"].has_tint);
    CHECK(p.parts["body"].tint == Rgba8{255, 128, 64, 255});
    CHECK(p.parts["body"].tint_k == doctest::Approx(0.5));
    CHECK(p.parts["arm"].flash == doctest::Approx(0.5));
    CHECK(p.parts["arm"].has_flash_color);
    CHECK(p.parts["arm"].flash_color == Rgba8{0, 255, 0, 255});
    CHECK(p.parts["arm"].flash_color_k == doctest::Approx(0.5));
    CHECK(p.parts["torso"].blur == doctest::Approx(1));
    CHECK(p.parts["head"].has_shadow);
    CHECK(p.parts["head"].shadow == Rgba8{0, 0, 0, 128});
    CHECK(p.parts["head"].shadow_k == doctest::Approx(0.5));
    CHECK(p.parts["head"].shadow_x == doctest::Approx(1));
    CHECK(p.parts["head"].shadow_y == doctest::Approx(-0.5));
    CHECK(p.parts["head"].shadow_blur == doctest::Approx(0.5));
    p = frame(s, rec, &rig, 1);
    CHECK(p.parts["body"].tint_k == 1);
    CHECK(p.parts["arm"].flash == 1);
    CHECK(p.parts["head"].shadow_k == 1);
    // Back to rest: nil goes from the colour given, as fill does.
    p = frame(s, rec, &rig, 1.5);
    CHECK(p.parts["body"].has_tint);
    CHECK(p.parts["body"].tint_k == doctest::Approx(0.5));
    CHECK(p.parts["head"].shadow_k == doctest::Approx(0.5));
    CHECK(p.parts["arm"].flash == doctest::Approx(0.5));
    p = frame(s, rec, &rig, 2);
    CHECK_FALSE(p.parts["body"].has_tint);
    CHECK_FALSE(p.parts["head"].has_shadow);
    CHECK(p.parts["arm"].flash == 0);
    CHECK(p.parts["head"].shadow_x == 2); // the offset stays: only the colour went
    // The rest of a property: what the frame has for a part without its track.
    CHECK(p.parts["torso"].blur == 2);
    CHECK_FALSE(p.parts["torso"].has_tint);
    CHECK(p.parts["torso"].flash == 0);
    CHECK(p.parts["torso"].mask.empty());

    // The block words and the value words of the timeline.
    CHECK(block_words(rec, block(rec, "body", Prop::Tint)) == "tint → #ff8040");
    CHECK(block_words(rec, block(rec, "arm", Prop::Flash)) == "flash → 1");
    CHECK(block_words(rec, block(rec, "arm", Prop::FlashColor)) == "flash_color → #00ff00");
    CHECK(block_words(rec, block(rec, "torso", Prop::Blur)) == "blur → 2");
    CHECK(block_words(rec, block(rec, "head", Prop::Shadow)) == "shadow → #00000080");
    CHECK(block_words(rec, block(rec, "head", Prop::ShadowY)) == "shadow_y → -1");
    CHECK(block_words(rec, block(rec, "body", Prop::Tint, 1)) == "tint → без тона");
    CHECK(block_words(rec, block(rec, "head", Prop::Shadow, 1)) == "shadow → без тени");
    CHECK(value_words(Prop::FlashColor, rest_value(Prop::FlashColor)) == "белый");
    CHECK(value_words(Prop::Fill, rest_value(Prop::Fill)) == "свой цвет");
    PropValue half;
    half.v = 0.5, half.color = {255, 0, 0, 128};
    CHECK(value_words(Prop::Tint, half) == "#ff000080 0.5");

    // The properties: their words, in the order of the tracks, which are numbers and which colours.
    const char* words[] = {"tint", "flash", "flash_color", "blur", "shadow", "shadow_x", "shadow_y", "shadow_blur"};
    for (usize i = 0; i < std::size(words); ++i) {
        Prop q;
        REQUIRE(prop_from_word(words[i], q));
        CHECK(static_cast<usize>(q) == static_cast<usize>(Prop::Tint) + i);
    }
    CHECK(static_cast<usize>(Prop::Visible) + 1 == static_cast<usize>(Prop::Tint));
    CHECK(static_cast<usize>(Prop::ShadowBlur) + 1 == static_cast<usize>(Prop::Morph));
    CHECK(std::string(prop_word(Prop::Mask)) == "маска");
    for (Prop q : {Prop::Flash, Prop::Blur, Prop::ShadowX, Prop::ShadowY, Prop::ShadowBlur}) CHECK(prop_is_number(q));
    for (Prop q : {Prop::Fill, Prop::Stroke, Prop::Tint, Prop::FlashColor, Prop::Shadow}) {
        CHECK(prop_is_color(q));
        CHECK_FALSE(prop_is_number(q));
    }
    for (Prop q : {Prop::Mask, Prop::Order}) {
        CHECK(prop_is_call(q));
        CHECK_FALSE(prop_is_number(q));
    }
    CHECK_FALSE(prop_is_call(Prop::Visible));
}

TEST_CASE("Motion effects: limits, overshoot, springs, save and restore, rest, bindings, tween") {
    Rig rig = loaded(picture());
    std::string why;
    // Back passes 1 on the way to a flash of 1 and 0 on the way to a blur from 0: the frame holds them.
    PoseScript s = script_of(clip_of(R"(all(part("arm").flash(1, 1, "out_back"), part("torso").blur(1, 1, "in_back"),
  part("head").shadow_blur(1, 1, "in_back")))"));
    const Recording rec = recorded(s, &rig);
    bool held = false;
    for (f64 t = 0.05; t < 1; t += 0.05) {
        const Pose p = frame(s, rec, &rig, t);
        CHECK(p.parts.at("arm").flash <= 1);
        CHECK(p.parts.at("torso").blur >= 0);
        CHECK(p.parts.at("head").shadow_blur >= 0);
        held |= p.parts.at("arm").flash == 1 && t < 0.9;
    }
    CHECK(held); // out_back reaches 1 early and stays there
    CHECK(frame(s, rec, &rig, 0.1).parts["torso"].blur == 0); // in_back first goes back

    // Past the limits in a pose given directly: drawn as at them.
    Pose over;
    over.parts["arm"].flash = 1.3;
    over.parts["torso"].blur = -2;
    over.parts["head"].has_shadow = true;
    over.parts["head"].shadow = {0, 0, 0, 255};
    over.parts["head"].shadow_x = 2.3;
    over.parts["head"].shadow_blur = -1;
    over.parts["torso"].has_tint = true; // shares past 1 of a tint and a shadow
    over.parts["torso"].tint = {255, 128, 64, 255};
    over.parts["torso"].tint_k = 1.5;
    over.parts["band"].has_shadow = true;
    over.parts["band"].shadow = {0, 0, 0, 0x80};
    over.parts["band"].shadow_x = 2.3;
    over.parts["band"].shadow_k = 2;
    REQUIRE(rig.pose(over, why));
    const assets::CookedTexture past = rig.draw(128, 128);
    Pose at = over;
    at.parts["arm"].flash = 1;
    at.parts["torso"].blur = 0;
    at.parts["head"].shadow_blur = 0;
    at.parts["torso"].tint_k = 1;
    at.parts["band"].shadow_k = 1;
    REQUIRE(rig.pose(at, why));
    CHECK(rig.draw(128, 128).rgba8 == past.rgba8);
    // A shadow halfway in: half its alpha.
    Pose half;
    half.parts["band"].has_shadow = true;
    half.parts["band"].shadow = {0, 0, 0, 255};
    half.parts["band"].shadow_x = 2.3;
    half.parts["band"].shadow_k = 0.5;
    REQUIRE(rig.pose(half, why));
    const assets::CookedTexture halfway = rig.draw(128, 128);
    half.parts["band"].shadow = {0, 0, 0, 128};
    half.parts["band"].shadow_k = 1;
    REQUIRE(rig.pose(half, why));
    CHECK(rig.draw(128, 128).rgba8 == halfway.rgba8);
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 != halfway.rgba8); // the shadow is there

    // Springs of the new numbers; none of a colour.
    PoseScript spring = script_of(clip_of(R"(part("torso").blur:spring(2, "plop")
part("arm").flash:spring(1, "plop"))"));
    const Recording sr = recorded(spring, &rig);
    CHECK(block(sr, "torso", Prop::Blur).spring);
    CHECK(block(sr, "arm", Prop::Flash).spring);
    for (f64 t = 0.02; t < 1.5; t += 0.02) CHECK(frame(spring, sr, &rig, t).parts["arm"].flash <= 1);
    PoseScript bad = script_of(clip_of(R"(part("arm").tint:spring("#ff0000"))"));
    CHECK(record_error(bad, &rig) ==
          "строка 2: пружина — только у чисел: x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, dash, draw_from, draw_to, flash, "
          "blur, shadow_x, shadow_y, shadow_blur");

    // save and restore, rest: the new properties go back too; the mask is no value to go back to.
    PoseScript back = script_of(clip_of(R"(part("head"):save()
part("head"):to({ tint = "#ff0000", flash = 0.5, blur = 2, shadow = "#000000", shadow_x = 3 }, 0.5)
part("head"):mask("lid")
part("head"):restore(0.5)
part("torso"):to({ blur = 1, flash_color = "#00ff00" }, 0.2)
part("torso"):rest(0.2))"));
    const Recording br = recorded(back, &rig);
    Pose q = frame(back, br, &rig, 0.5);
    CHECK(q.parts["head"].has_tint);
    CHECK(q.parts["head"].flash == doctest::Approx(0.5));
    CHECK(q.parts["head"].blur == 2);
    CHECK(q.parts["head"].has_shadow);
    CHECK(q.parts["head"].mask == "lid");
    q = frame(back, br, &rig, 1);
    CHECK_FALSE(q.parts["head"].has_tint);
    CHECK(q.parts["head"].flash == 0);
    CHECK(q.parts["head"].blur == 0);
    CHECK_FALSE(q.parts["head"].has_shadow);
    CHECK(q.parts["head"].shadow_x == 0);
    CHECK(q.parts["head"].mask == "lid"); // restore leaves the mask as it is
    q = frame(back, br, &rig, 1.4);
    CHECK(q.parts["torso"].blur == 0);
    CHECK_FALSE(q.parts["torso"].has_flash_color);
    CHECK(q.parts["head"].mask == "lid");
    for (const Block& b : br.blocks) CHECK_FALSE((b.to != Block::To::Value && b.prop == Prop::Mask));

    // Bindings and tween give them as values.
    PoseScript bind = script_of(clip_of(R"(part("torso").blur(function() return now() * 2 end)
part("body").tint(function() return now() < 0.5 and "#80808080" or nil end)
part("arm").flash(function() return now() * 3 end)
tween(1, function(k) part("head").shadow_x(k * 4) end))"));
    const Recording bdr = recorded(bind, &rig);
    q = frame(bind, bdr, &rig, 0.25);
    CHECK(q.parts["torso"].blur == doctest::Approx(0.5));
    CHECK(q.parts["body"].has_tint);
    CHECK(q.parts["body"].tint == Rgba8{128, 128, 128, 128});
    CHECK(q.parts["arm"].flash == doctest::Approx(0.75));
    CHECK(q.parts["head"].shadow_x == doctest::Approx(1).epsilon(0.1));
    q = frame(bind, bdr, &rig, 0.75);
    CHECK_FALSE(q.parts["body"].has_tint);
    CHECK(q.parts["arm"].flash == 1); // a binding past the limit is held as well
    PoseScript wrong = script_of(clip_of(R"(part("body").tint(function() return "teal-ish" end))"));
    const Recording wr = recorded(wrong, &rig);
    Pose none;
    CHECK_FALSE(wrong.frame(wr, &rig, 0.1, {}, 1, none));
    CHECK(wrong.error().find("связь tint вернула не цвет \"#rrggbb\" и не \"#rrggbbaa\"") != std::string::npos);

    // What is not a value of a property is named, at its line.
    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("arm").flash(1.5, 0.1))") == "строка 2: flash — доля от 0 до 1");
    CHECK(error(R"(part("arm").flash(-0.1, 0.1))") == "строка 2: flash — доля от 0 до 1");
    CHECK(error(R"(part("arm").blur(-1, 0.1))") == "строка 2: blur — число не ниже 0");
    CHECK(error(R"(part("arm"):to({ shadow_blur = -2 }, 0.1))") == "строка 2: shadow_blur — число не ниже 0");
    CHECK(error(R"(part("arm").flash("x", 0.1))") == "строка 2: flash: нужно число, а не string");
    CHECK(error(R"(part("arm").tint("red-ish", 0.1))") == "строка 2: tint: нужен цвет \"#rrggbb\" или \"#rrggbbaa\"");
    CHECK(error(R"(part("arm").shadow("#00000", 0.1))") == "строка 2: shadow: нужен цвет \"#rrggbb\" или \"#rrggbbaa\"");
    CHECK(error(R"(part("arm"):to({ mask = "lid" }, 0.1))") ==
          "строка 2: part:to: поля «mask» нет (есть x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, stroke_width, "
          "dash, draw_from, draw_to, visible, tint, flash, flash_color, blur, shadow, shadow_x, shadow_y, shadow_blur)");
}

TEST_CASE("Motion effects: a tint and a flash are the colours they give written in the SVG") {
    Rig rig = loaded(picture());
    std::string why;
    const assets::CookedTexture rest = rig.draw(128, 128);
    const assets::CookedTexture rest_baked = rig.bake(64, 64, {});
    const Rgba8 white{255, 255, 255, 255};
    const Rgba8 body_tint{0xff, 0x80, 0x40, 255};

    SUBCASE("each shape's fill, stroke and gradient stops; a part's own first, then its group's") {
        Pose pose;
        pose.parts["body"].has_tint = true;
        pose.parts["body"].tint = body_tint;
        pose.parts["body"].tint_k = 0.5;
        PartPose& arm = pose.parts["arm"];
        arm.has_fill = true;
        arm.fill = white;
        arm.flash = 0.5;
        arm.has_flash_color = true;
        arm.flash_color = {0, 255, 0, 255};
        pose.parts["head"].has_tint = true;
        pose.parts["head"].tint = {0x80, 0x80, 0x80, 0x80};
        PartPose& band = pose.parts["band"];
        band.has_tint = true;
        band.tint = {0x80, 0x80, 0x80, 255};
        band.tint_k = 0.25;
        band.flash = 0.25;
        REQUIRE(rig.pose(pose, why));

        auto body = [&](Rgba8 c) { return tinted(c, body_tint, 0.5); };
        const Rgba8 torso = body(kTorso);
        const Rgba8 arm_fill = body(flashed(white, {0, 255, 0, 255}, 0.5)); // its fill white first, then its flash
        const Rgba8 grey{0x80, 0x80, 0x80, 0x80};
        const Rgba8 head_fill = body(tinted(kHead, grey, 1)), head_stroke = body(tinted({0x40, 0x20, 0x00, 255}, grey, 1));
        CHECK(arm_fill == Rgba8{128, 192, 80, 255}); // the numbers by hand: the flash rounds 127.5 up, the tint's green is 191.5
        CHECK(head_fill.a == 128);
        auto stop = [&](Rgba8 c) { return flashed(tinted(c, {0x80, 0x80, 0x80, 255}, 0.25), white, 0.25); };
        const Rgba8 s0 = stop({0x20, 0x50, 0xa0, 255}), s1 = stop({0xa0, 0xd0, 0xf0, 153});
        Pic ref;
        ref.paint["torso"] = "fill=\"" + rgb(torso) + "\"";
        ref.paint["arm"] = "fill=\"" + rgb(arm_fill) + "\"";
        ref.paint["head"] = "fill=\"" + rgb(head_fill) + "\" fill-opacity=\"" + opacity(head_fill.a) + "\" stroke=\"" + rgb(head_stroke) +
                            "\" stroke-opacity=\"" + opacity(head_stroke.a) + "\" stroke-width=\"1.5\"";
        ref.paint["band"] = "fill=\"url(#sky2)\"";
        ref.defs = "<linearGradient id=\"sky2\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"><stop offset=\"0\" stop-color=\"" + rgb(s0) +
                   "\" stop-opacity=\"" + opacity(s0.a) + "\"/><stop offset=\"1\" stop-color=\"" + rgb(s1) + "\" stop-opacity=\"" +
                   opacity(s1.a) + "\"/></linearGradient>";
        same_picture(rig, ref.text());
    }
    SUBCASE("the flash's colour without its alpha, after the part's own fill; the group's tint over it") {
        Pose pose;
        pose.parts["torso"].flash = 0.75;
        pose.parts["torso"].has_flash_color = true;
        pose.parts["torso"].flash_color = {255, 0, 0, 0x80};
        pose.parts["body"].has_tint = true;
        pose.parts["body"].tint = body_tint;
        pose.parts["body"].tint_k = 0.5;
        // The arm's flash colour half way from white: white + (0 − 255) · ½ in red and blue.
        pose.parts["arm"].flash = 1;
        pose.parts["arm"].has_flash_color = true;
        pose.parts["arm"].flash_color = {0, 255, 0, 255};
        pose.parts["arm"].flash_color_k = 0.5;
        REQUIRE(rig.pose(pose, why));
        const Rgba8 torso = tinted(flashed(kTorso, {255, 0, 0, 255}, 0.75), body_tint, 0.5);
        CHECK(torso == Rgba8{203, 18, 30, 255});
        // 127.5 is no byte: the flash goes to it as it is, 0xc0 + (127.5 − 0xc0) · 1 rounds to 128.
        const Rgba8 arm = tinted(Rgba8{128, 255, 128, 255}, body_tint, 0.5);
        Pic ref;
        ref.paint["torso"] = "fill=\"" + rgb(torso) + "\"";
        ref.paint["arm"] = "fill=\"" + rgb(arm) + "\"";
        ref.paint["head"] = "fill=\"" + rgb(tinted(kHead, body_tint, 0.5)) + "\" stroke=\"" + rgb(tinted({0x40, 0x20, 0x00, 255}, body_tint, 0.5)) +
                            "\" stroke-width=\"1.5\"";
        same_picture(rig, ref.text());
    }
    SUBCASE("a shape without a fill gets none") {
        Rig line = loaded(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <path id="line" d="M4 16 L28 16" fill="none" stroke="#000000" stroke-width="2"/>
</svg>)");
        Pose pose;
        pose.parts["line"].flash = 1;
        REQUIRE(line.pose(pose, why));
        Rig ref = loaded(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <path id="line" d="M4 16 L28 16" fill="none" stroke="#ffffff" stroke-width="2"/>
</svg>)");
        CHECK(line.draw(64, 64).rgba8 == ref.draw(64, 64).rgba8);
    }
    // And back to rest: exactly the first picture, smooth and baked.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 == rest.rgba8);
    CHECK(rig.bake(64, 64, {}).rgba8 == rest_baked.rgba8);
}

TEST_CASE("Motion effects: blur and shadow are ThorVG's, in picture units however the part stands") {
    Rig rig = loaded(picture());
    std::string why;
    const assets::CookedTexture rest = rig.draw(128, 128);
    const assets::CookedTexture rest_baked = rig.bake(64, 64, {});
    const Rgba8 shade{0, 0, 0, 0x60};
    // The sigmas keep off the steps of ThorVG's box kernels at both sizes, the offsets off whole pixels (it drops
    // their fractions): a float off in the last place draws the same.

    SUBCASE("a shape's blur and shadow at rest") {
        Pose pose;
        pose.parts["torso"].blur = 1.2;
        PartPose& head = pose.parts["head"];
        head.has_shadow = true;
        head.shadow = shade;
        head.shadow_x = 2.3, head.shadow_y = 3.3, head.shadow_blur = 0.9;
        REQUIRE(rig.pose(pose, why));
        Pic ref;
        ref.wrap = {"torso", "head"};
        const Touch touch = [&](const std::vector<tvg::Paint*>& all) {
            blur_on(scene_of(all, "torso_g"), 1.2);
            shadow_on(scene_of(all, "head_g"), shade, 2.3, 3.3, 0.9);
        };
        const assets::CookedTexture drawn = rig.draw(128, 128);
        CHECK(drawn.rgba8 != rest.rgba8);
        INFO("bytes off: ", bytes_off(drawn, direct(ref.text(), 128, 128, touch)));
        CHECK(drawn.rgba8 == direct(ref.text(), 128, 128, touch).rgba8);
        CHECK(rig.draw(64, 64).rgba8 == direct(ref.text(), 64, 64, touch).rgba8);

        // Baked: each shape blurred alone and cut at half, in its own colour; the head's shadow a pass of its own
        // before the head, opaque, though its alpha is under half.
        const assets::CookedTexture baked = rig.bake(64, 64, {});
        CHECK(at_px(baked, 28, 30) == "#3060c0");
        CHECK(at_px(rest_baked, 20, 39) == "#3060c0");
        CHECK(alpha_at(baked, 20, 39) == 0); // the corner rounded by the blur
        CHECK(at_px(rest_baked, 29, 25) == "#3060c0");
        CHECK(at_px(baked, 29, 25) == "#000000"); // the head's shadow over the torso
        CHECK(at_px(baked, 25, 19) == "#e0b080"); // the head over its shadow
        for (usize i = 0; i < baked.rgba8.size(); i += 4) {
            const u8 a = baked.rgba8[i + 3];
            CHECK((a == 0 || a == 255));
        }
        // The effects are back after the bake.
        CHECK(rig.draw(128, 128).rgba8 == drawn.rgba8);
    }
    SUBCASE("a group's blur at rest: of its shapes together") {
        Pose pose;
        pose.parts["body"].blur = 0.7;
        REQUIRE(rig.pose(pose, why));
        Pic ref;
        ref.wrap = {"body"};
        const Touch touch = [&](const std::vector<tvg::Paint*>& all) { blur_on(scene_of(all, "body_g"), 0.7); };
        CHECK(rig.draw(128, 128).rgba8 == direct(ref.text(), 128, 128, touch).rgba8);
    }
    SUBCASE("a turned and scaled group: its blur and shadow as on a group round it that stands still") {
        Pose pose;
        PartPose& body = pose.parts["body"];
        body.x = 2, body.rot = 30, body.scale = 1.5;
        body.blur = 1.1;
        body.has_shadow = true;
        body.shadow = shade;
        body.shadow_x = 2.3, body.shadow_y = 3.3, body.shadow_blur = 0.9;
        REQUIRE(rig.pose(pose, why));
        Pic ref;
        ref.wrap = {"body"};
        ref.extra["body"] = matrix_text(rig.matrix(rig.part("body")));
        const Touch touch = [&](const std::vector<tvg::Paint*>& all) {
            tvg::Scene* g = scene_of(all, "body_g");
            blur_on(g, 1.1);
            shadow_on(g, shade, 2.3, 3.3, 0.9);
        };
        const assets::CookedTexture drawn = rig.draw(128, 128);
        INFO("bytes off: ", bytes_off(drawn, direct(ref.text(), 128, 128, touch)));
        CHECK(drawn.rgba8 == direct(ref.text(), 128, 128, touch).rgba8);
    }
    SUBCASE("a shape inside a turned group: its shadow falls down and right in the picture all the same") {
        Pose pose;
        pose.parts["body"].rot = 90;
        pose.parts["body"].scale = 1.5;
        PartPose& head = pose.parts["head"];
        head.blur = 1.1;
        head.has_shadow = true;
        head.shadow = shade;
        head.shadow_x = 2.3, head.shadow_y = 3.3, head.shadow_blur = 0.9;
        REQUIRE(rig.pose(pose, why));
        // In the group's own units: the offset turned back by 90° (x, y → y, −x) and both over its scale.
        Pic ref;
        ref.wrap = {"head"};
        ref.extra["body"] = matrix_text(rig.matrix(rig.part("body")));
        const Touch touch = [&](const std::vector<tvg::Paint*>& all) {
            tvg::Scene* g = scene_of(all, "head_g");
            blur_on(g, 1.1 / 1.5);
            shadow_on(g, shade, 3.3 / 1.5, -2.3 / 1.5, 0.9 / 1.5);
        };
        const assets::CookedTexture drawn = rig.draw(128, 128);
        INFO("bytes off: ", bytes_off(drawn, direct(ref.text(), 128, 128, touch)));
        CHECK(drawn.rgba8 == direct(ref.text(), 128, 128, touch).rgba8);
    }
    SUBCASE("baked: a group's shadow before its first shape, without the shadows of the parts in it, and theirs without it") {
        Pose pose;
        pose.parts["body"].has_shadow = true;
        pose.parts["body"].shadow = shade;
        pose.parts["body"].shadow_x = 2.3, pose.parts["body"].shadow_y = 3.3;
        pose.parts["head"].has_shadow = true;
        pose.parts["head"].shadow = {0, 0, 0x80, 0x60};
        pose.parts["head"].shadow_x = -3.3, pose.parts["head"].shadow_y = 2.3;
        REQUIRE(rig.pose(pose, why));
        const assets::CookedTexture baked = rig.bake(64, 64, {});
        CHECK(at_px(baked, 36, 30) == "#000000"); // the body's shadow right of the torso
        CHECK(at_px(baked, 35, 30) == "#3060c0"); // the torso over it
        CHECK(at_px(baked, 29, 25) == "#3060c0"); // the body's shadow of the head is under the torso
        CHECK(at_px(baked, 15, 19) == "#000080"); // the head's own shadow, left of it
        CHECK(at_px(baked, 25, 19) == "#e0b080");
    }
    SUBCASE("baked: shadows as opaque as can be are off in the passes of the shapes and of the group round them") {
        Pose pose;
        pose.parts["body"].has_shadow = true;
        pose.parts["body"].shadow = shade;
        pose.parts["body"].shadow_x = 2.3, pose.parts["body"].shadow_y = 3.3;
        pose.parts["head"].has_shadow = true;
        pose.parts["head"].shadow = {0, 0, 0x80, 255};
        pose.parts["head"].shadow_x = 0.3, pose.parts["head"].shadow_y = -10.3;
        REQUIRE(rig.pose(pose, why));
        const assets::CookedTexture baked = rig.bake(64, 64, {});
        CHECK(at_px(baked, 24, 4) == "#000080");  // the head's shadow above it
        CHECK(at_px(baked, 24, 13) == "#e0b080"); // the head over its shadow, not the shadow over the head
        CHECK(alpha_at(baked, 31, 12) == 0);      // no body's shadow of the head's shadow
        CHECK(at_px(baked, 36, 30) == "#000000"); // the body's own shadow
    }
    // And back to rest: exactly the first picture, smooth and baked.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 == rest.rgba8);
    CHECK(rig.bake(64, 64, {}).rgba8 == rest_baked.rgba8);
}

TEST_CASE("Motion effects: a mask is ThorVG's, the mask's shapes as they stand") {
    Rig rig = loaded(Pic{{}, {}, {}, "", R"(  <circle id="small" cx="25" cy="18" r="3" display="none"/>
  <g id="empty"/>
  <g id="gone" display="none"><circle id="g1" cx="8" cy="8" r="2"/></g>
)"}.text());
    std::string why;
    const assets::CookedTexture rest = rig.draw(128, 128);
    const assets::CookedTexture rest_baked = rig.bake(64, 64, {});
    // The reference: the masked part in a group, lid_m (lid drawn white, in the picture's units) its mask.
    const std::string lid_m = R"(  <circle id="lid_m" cx="27" cy="18" r="4" fill="#ffffff")";
    auto masked = [&](const Pic& ref, const char* holder, bool invert) {
        return direct(ref.text(), 128, 128, [&](const std::vector<tvg::Paint*>& all) { mask_with(all, holder, "lid_m", invert); });
    };
    auto check = [&](const assets::CookedTexture& want) {
        const assets::CookedTexture drawn = rig.draw(128, 128);
        CHECK(drawn.rgba8 != rest.rgba8);
        INFO("bytes off: ", bytes_off(drawn, want));
        CHECK(drawn.rgba8 == want.rgba8);
    };
    Pic ref;
    ref.wrap = {"head"};
    ref.tail = lid_m + "/>\n";

    SUBCASE("inside and outside") {
        Pose pose;
        pose.parts["head"].mask = "lid";
        REQUIRE(rig.pose(pose, why));
        check(masked(ref, "head_g", false));
        pose.parts["head"].mask_invert = true;
        REQUIRE(rig.pose(pose, why));
        check(masked(ref, "head_g", true));
    }
    SUBCASE("the mask's area whatever its look: hidden in the SVG, by the pose, see-through, or shown") {
        Pic shown;
        shown.paint["lid"] = "fill=\"#00a000\" stroke=\"#000000\" stroke-width=\"1\"";
        Rig lid = loaded(shown.text());
        Pose pose;
        pose.parts["head"].mask = "lid";
        pose.parts["lid"].visible = false;
        REQUIRE(lid.pose(pose, why));
        // Its stroke is its area too: the mask reaches half the stroke out.
        Pic stroked = ref;
        stroked.tail = lid_m + R"( stroke="#ffffff" stroke-width="1"/>)" "\n";
        stroked.paint["lid"] = shown.paint["lid"] + " display=\"none\"";
        const assets::CookedTexture want = direct(stroked.text(), 128, 128, [&](const std::vector<tvg::Paint*>& all) {
            mask_with(all, "head_g", "lid_m", false);
        });
        CHECK(lid.draw(128, 128).rgba8 == want.rgba8);
        pose.parts["lid"].visible = true;
        pose.parts["lid"].opacity = 0;
        REQUIRE(lid.pose(pose, why));
        CHECK(lid.draw(128, 128).rgba8 == want.rgba8);
        pose.parts.erase("lid");
        REQUIRE(lid.pose(pose, why));
        stroked.paint["lid"] = shown.paint["lid"];
        CHECK(lid.draw(128, 128).rgba8 == direct(stroked.text(), 128, 128, [&](const std::vector<tvg::Paint*>& all) {
                  mask_with(all, "head_g", "lid_m", false);
              }).rgba8);
    }
    SUBCASE("the masked part moves under its mask; the mask moves with its own part") {
        Pose pose;
        pose.parts["head"].mask = "lid";
        pose.parts["head"].x = 3;
        REQUIRE(rig.pose(pose, why));
        Pic moved = ref;
        moved.extra["head"] = "transform=\"translate(3 0)\"";
        check(masked(moved, "head_g", false));
        pose.parts.erase("head");
        pose.parts["head"].mask = "lid";
        pose.parts["lid"].x = 2, pose.parts["lid"].y = 1;
        REQUIRE(rig.pose(pose, why));
        Pic lid_moved = ref;
        lid_moved.tail = lid_m + R"x( transform="translate(2 1)"/>)x" "\n";
        check(masked(lid_moved, "head_g", false));
        // The group the masked part is in moves it, not the mask outside the group.
        pose.parts.erase("lid");
        pose.parts["body"].x = 3;
        REQUIRE(rig.pose(pose, why));
        Pic body_moved = ref;
        body_moved.extra["body"] = "transform=\"translate(3 0)\"";
        body_moved.tail = lid_m + R"x( transform="translate(-3 0)"/>)x" "\n";
        check(masked(body_moved, "head_g", false));
    }
    SUBCASE("a group masked, and a group as the mask") {
        Pose pose;
        pose.parts["body"].mask = "lid";
        pose.parts["body"].rot = 20;
        REQUIRE(rig.pose(pose, why));
        Pic group;
        group.wrap = {"body"};
        group.extra["body"] = matrix_text(rig.matrix(rig.part("body")));
        group.tail = lid_m + "/>\n";
        check(masked(group, "body_g", false));
        Pose lids;
        lids.parts["head"].mask = "lids";
        lids.parts["head"].mask_invert = true;
        REQUIRE(rig.pose(lids, why));
        Pic two = ref;
        two.tail = R"(  <g id="lids_m"><circle cx="20" cy="17" r="2.5" fill="#ffffff"/><circle cx="28" cy="19" r="2.5" fill="#ffffff"/></g>)" "\n";
        check(direct(two.text(), 128, 128, [&](const std::vector<tvg::Paint*>& all) { mask_with(all, "head_g", "lids_m", true); }));
    }
    SUBCASE("a mask cuts the part's blur and shadow too") {
        Pose pose;
        PartPose& head = pose.parts["head"];
        head.mask = "lid";
        head.mask_invert = true;
        head.blur = 1.1;
        head.has_shadow = true;
        head.shadow = {0, 0, 0, 0x60};
        head.shadow_x = 2.3, head.shadow_y = 3.3, head.shadow_blur = 0.9;
        REQUIRE(rig.pose(pose, why));
        check(direct(ref.text(), 128, 128, [&](const std::vector<tvg::Paint*>& all) {
            tvg::Scene* g = scene_of(all, "head_g");
            blur_on(g, 1.1);
            shadow_on(g, {0, 0, 0, 0x60}, 2.3, 3.3, 0.9);
            mask_with(all, "head_g", "lid_m", true);
        }));
    }
    SUBCASE("baked: each shape cut by the mask, at half cover") {
        // The small mask lies inside the head's fill: the head masked by it is that circle in the head's colour, its
        // stroke cut away whole.
        Pose pose;
        pose.parts["head"].mask = "small";
        REQUIRE(rig.pose(pose, why));
        Pic circle;
        circle.paint["head"] = "display=\"none\"";
        circle.tail = R"(  <circle cx="25" cy="18" r="3" fill="#e0b080"/>)" "\n";
        Rig want = loaded(circle.text());
        CHECK(rig.bake(64, 64, {}).rgba8 == want.bake(64, 64, {}).rgba8);
        CHECK(rig.draw(128, 128).rgba8 == want.draw(128, 128).rgba8);
        // Outside it: a ring of the head round the hole, the torso through the hole.
        pose.parts["head"].mask_invert = true;
        REQUIRE(rig.pose(pose, why));
        const assets::CookedTexture baked = rig.bake(64, 64, {});
        CHECK(at_px(baked, 25, 20) == "#3060c0");
        CHECK(alpha_at(baked, 24, 17) == 0);
        CHECK(at_px(baked, 19, 18) == "#e0b080");
    }
    SUBCASE("what cannot be a mask is named, the rest of the pose done") {
        Pose pose;
        pose.parts["head"].mask = "tail";
        pose.parts["arm"].x = 3;
        CHECK_FALSE(rig.pose(pose, why));
        CHECK(why == "маска части «head»: в рисунке нет части «tail»");
        Pic moved;
        moved.paint = {};
        moved.extra["arm"] = "transform=\"translate(3 0)\"";
        moved.tail = R"(  <circle id="small" cx="25" cy="18" r="3" display="none"/>
  <g id="empty"/>
  <g id="gone" display="none"><circle id="g1" cx="8" cy="8" r="2"/></g>
)";
        CHECK(rig.draw(128, 128).rgba8 == loaded(moved.text()).draw(128, 128).rgba8);
        pose.parts["head"].mask = "head";
        CHECK_FALSE(rig.pose(pose, why));
        CHECK(why == "часть «head» не маска сама себе");
        // A group hidden by display="none" is a part without shapes (ThorVG draws nothing in it); an empty group is
        // no part at all.
        CHECK(rig.part("empty") == -1);
        REQUIRE(rig.part("gone") >= 0);
        CHECK_FALSE(rig.has_shapes(rig.part("gone")));
        pose.parts["head"].mask = "gone";
        CHECK_FALSE(rig.pose(pose, why));
        CHECK(why == "у маски «gone» нет фигур");
        CHECK(rig.has_shapes(rig.part("lid")));
        CHECK(rig.has_shapes(rig.part("lids")));
    }
    // And back to rest: exactly the first picture, smooth and baked.
    REQUIRE(rig.pose({}, why));
    CHECK(rig.draw(128, 128).rgba8 == rest.rgba8);
    CHECK(rig.bake(64, 64, {}).rgba8 == rest_baked.rgba8);
}

TEST_CASE("Motion effects: the mask in the flow") {
    const Rig rig = loaded(Pic{{}, {}, {}, "", "  <g id=\"gone\" display=\"none\"><circle cx=\"8\" cy=\"8\" r=\"2\"/></g>\n"}.text());
    const std::string code = clip_of(R"(part("head"):mask("lid")
wait(0.5)
part("head"):mask("lid", { invert = true })
wait(0.5)
part("head"):mask(nil)
part("head").x(2, 0.5)
part("arm"):mask("lids", { invert = false }))");
    PoseScript s = script_of(code);
    const Recording rec = recorded(s, &rig);
    CHECK(rec.length == doctest::Approx(1.5));
    CHECK(block(rec, "head", Prop::Mask).length == 0);
    CHECK(block_words(rec, block(rec, "head", Prop::Mask)) == "маска lid");
    CHECK(block_words(rec, block(rec, "head", Prop::Mask, 1)) == "маска lid, наоборот");
    CHECK(block_words(rec, block(rec, "head", Prop::Mask, 2)) == "без маски");
    CHECK(block_words(rec, block(rec, "arm", Prop::Mask)) == "маска lids");
    Pose p = frame(s, rec, &rig, 0);
    CHECK(p.parts["head"].mask == "lid");
    CHECK_FALSE(p.parts["head"].mask_invert);
    p = frame(s, rec, &rig, 0.49);
    CHECK(p.parts["head"].mask == "lid");
    CHECK_FALSE(p.parts["head"].mask_invert);
    p = frame(s, rec, &rig, 0.5);
    CHECK(p.parts["head"].mask == "lid");
    CHECK(p.parts["head"].mask_invert);
    p = frame(s, rec, &rig, 1.2);
    CHECK(p.parts["head"].mask.empty());
    CHECK(p.parts["arm"].mask.empty());
    p = frame(s, rec, &rig, 1.5);
    CHECK(p.parts["arm"].mask == "lids");
    CHECK_FALSE(p.parts["arm"].mask_invert);
    CHECK(p.parts["head"].x == 2);

    // By time, then as recorded: all() records its first task whole before the second.
    PoseScript late = script_of(clip_of(R"(all(delay(0.5, part("head"):mask("lid")), delay(0.25, part("head"):mask("lid", { invert = true }))))"));
    const Recording later = recorded(late, &rig);
    CHECK(frame(late, later, &rig, 0.1).parts["head"].mask.empty());
    CHECK(frame(late, later, &rig, 0.3).parts["head"].mask_invert);
    p = frame(late, later, &rig, 0.7);
    CHECK(p.parts["head"].mask == "lid");
    CHECK_FALSE(p.parts["head"].mask_invert);

    // The mask is the code's: no field edits it.
    std::string why;
    for (BlockEdit e : {BlockEdit::Value, BlockEdit::Length, BlockEdit::Timing, BlockEdit::Start}) {
        CHECK_FALSE(can_edit(rec, block(rec, "head", Prop::Mask, 1), e, s.code(), why));
        CHECK(why == "маска правится в коде: mask(\"lid\"), mask(nil)");
    }
    std::string out;
    why.clear();
    CHECK_FALSE(edit_value(rec, block(rec, "head", Prop::Mask), Prop::Mask, rest_value(Prop::Mask), s.code(), code, out, why));
    CHECK(why == "маска правится в коде: mask(\"lid\"), mask(nil)");
    why.clear(); // nor through another block's call
    CHECK_FALSE(edit_value(rec, block(rec, "head", Prop::X), Prop::Mask, rest_value(Prop::Mask), s.code(), code, out, why));
    CHECK(why == "маска правится в коде: mask(\"lid\"), mask(nil)");
    CHECK(out.empty());
    // A mask comes in whole at its call: halfway is still the one before.
    PropValue lid;
    lid.v = 1, lid.target = "lid";
    CHECK(mix(Prop::Mask, rest_value(Prop::Mask), lid, 0.5) == rest_value(Prop::Mask));
    CHECK(mix(Prop::Mask, rest_value(Prop::Mask), lid, 1) == lid);

    auto error = [&](const char* body) {
        PoseScript e = script_of(clip_of(body));
        return record_error(e, &rig);
    };
    CHECK(error(R"(part("head"):mask("tail"))") == "строка 2: в рисунке нет части «tail»");
    CHECK(error(R"(part("head"):mask("head"))") == "строка 2: часть «head» не маска сама себе");
    CHECK(error(R"(part("head"):mask("gone"))") ==
          "строка 2: у маски «gone» нет фигур: спрячьте фигуры в ней по одной (hidden = true) или саму группу через gone.visible(false)");
    CHECK(error(R"(part("head"):mask(1))") == "строка 2: mask: нужно имя части-маски или nil (без маски)");
    CHECK(error(R"(part("head"):mask("lid", true))") == "строка 2: mask: второй аргумент — { invert = true } или { invert = false }");
    CHECK(error(R"(part("head"):mask("lid", { inverted = true }))") == "строка 2: mask: второй аргумент — { invert = true } или { invert = false }");
    CHECK(error(R"(part("head"):mask("lid", { invert = 1 }))") == "строка 2: mask: второй аргумент — { invert = true } или { invert = false }");
    CHECK(error(R"(part("head"):mask("lid", {}, 1))") == "строка 2: mask(\"lid\", { invert = true }): имя части-маски и, если нужно, таблица");
    CHECK(error(R"(part("head"):mask(nil, { invert = true }))") == "строка 2: mask(nil): без маски таблица не нужна");
    CHECK(error(R"(part("head"):mask("lid"):wait(1))") ==
          "строка 2: wait — не после mask: у маски нет длины, следующий переход — отдельной строкой");
    // Without the rig, the names wait for the frame.
    PoseScript free = script_of(clip_of(R"(part("head"):mask("tail"))"));
    const Recording fr = recorded(free, nullptr);
    CHECK(fr.blocks.size() == 1);
}

TEST_CASE("Motion effects: colours with their alpha, and values written into the code") {
    Rgba8 c;
    CHECK(parse_part_color("#ff000080", c));
    CHECK(c == Rgba8{255, 0, 0, 128});
    CHECK(parse_part_color("#FF8040", c));
    CHECK(c == Rgba8{255, 128, 64, 255});
    CHECK(parse_part_color("#f00", c));
    CHECK(c == Rgba8{255, 0, 0, 255});
    CHECK_FALSE(parse_part_color("#ff00008", c));
    CHECK_FALSE(parse_part_color("#ff0000zz", c));
    CHECK_FALSE(parse_part_color("#ff00008000", c));
    CHECK_FALSE(parse_color("#ff000080", c)); // the picture's own colours stay as SVG has them
    CHECK(color_hex({255, 0, 0, 255}) == "#ff0000");
    CHECK(color_hex({255, 0, 0, 128}) == "#ff000080");
    CHECK(color_hex({1, 2, 3, 0}) == "#01020300");

    // A fill with its alpha: times the shape's own, as fill-opacity.
    Rig rig = loaded(picture());
    PoseScript s = script_of(clip_of(R"(part("torso").fill("#ff000080", 0.5)
part("head").stroke("#0000ff40", 0.5)
log(part("torso").fill(), part("head").stroke()))"));
    const Recording rec = recorded(s, &rig);
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "#ff000080\t#0000ff40");
    CHECK(block_words(rec, block(rec, "torso", Prop::Fill)) == "fill → #ff000080");
    std::string why;
    REQUIRE(rig.pose(frame(s, rec, &rig, 1), why));
    Pic ref;
    ref.paint["torso"] = "fill=\"#ff0000\" fill-opacity=\"" + opacity(128) + "\"";
    ref.paint["head"] = "fill=\"#e0b080\" stroke=\"#0000ff\" stroke-opacity=\"" + opacity(64) + "\" stroke-width=\"1.5\"";
    same_picture(rig, ref.text());

    // Values written into the code: colours with their alpha, numbers, nil.
    const std::string code = clip_of(R"(part("body").tint("#ff0000", 0.5)
part("head"):to({ blur = 2, shadow = "#000000" }, 0.5)
part("arm").flash_color("#00ff00", 0.2))");
    PoseScript e = script_of(code);
    const Recording er = recorded(e, &rig);
    std::string out;
    PropValue grey;
    grey.v = 1, grey.color = {0x80, 0x80, 0x80, 0x80};
    REQUIRE(edit_value(er, block(er, "body", Prop::Tint), Prop::Tint, grey, e.code(), code, out, why));
    CHECK(out.find(R"(part("body").tint("#80808080", 0.5))") != std::string::npos);
    PropValue three;
    three.v = 3;
    REQUIRE(edit_value(er, block(er, "head", Prop::Blur), Prop::Blur, three, e.code(), code, out, why));
    CHECK(out.find("{ blur = 3, shadow = \"#000000\" }") != std::string::npos);
    REQUIRE(edit_value(er, block(er, "arm", Prop::FlashColor), Prop::FlashColor, rest_value(Prop::FlashColor), e.code(), code, out, why));
    CHECK(out.find(R"(part("arm").flash_color(nil, 0.2))") != std::string::npos);
    // The edited code records the value written.
    REQUIRE(edit_value(er, block(er, "body", Prop::Tint), Prop::Tint, grey, e.code(), code, out, why));
    PoseScript again = script_of(out);
    const Recording rec2 = recorded(again, &rig);
    CHECK(block(rec2, "body", Prop::Tint).value.color == Rgba8{0x80, 0x80, 0x80, 0x80});
    CHECK(block(rec2, "head", Prop::Blur).value.v == 2);
}

TEST_CASE("Motion effects: a pose of the new numbers that are no numbers is named") {
    Rig rig = loaded(picture());
    std::string why;
    for (int i = 0; i < 8; ++i) {
        Pose pose;
        PartPose& pp = pose.parts["head"];
        f64* field[] = {&pp.tint_k, &pp.flash, &pp.flash_color_k, &pp.blur, &pp.shadow_k, &pp.shadow_x, &pp.shadow_y, &pp.shadow_blur};
        *field[i] = std::nan("");
        CHECK_FALSE(rig.pose(pose, why));
        CHECK(why == "часть «head»: не число в позе");
    }
}
