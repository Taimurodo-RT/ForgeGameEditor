// Clips of «Векторная» written as code (step 1 of docs/models/движение-модель.md): Motion Canvas's timing functions,
// springs and arc in engine/vector, the place of every call of the code, the recording of a clip and a frame of it, a
// mouse edit written into the code. The expected curves come from Motion Canvas's own source
// (packages/core/src/tweening at commit 7b91435) run under node, so a curve that drifts from theirs shows here; the
// expected times of the flow functions are counted by hand by their rules.

#include "forge/pixel/code_sites.h"
#include "forge/pixel/motion.h"
#include "forge/pixel/motion_edit.h"
#include "forge/pixel/rig.h"
#include "forge/pixel/script.h"
#include "forge/vector/ease.h"
#include "forge/vector/timing.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <ostream> // doctest's INFO prints a string_view through it; MSVC needs the whole class
#include <string>
#include <vector>

using namespace forge;
using namespace forge::vector;
using namespace forge::pixel;

namespace {

struct TimingRef {
    const char* word;
    f64 at[7];
};
// Theirs at 0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9.
constexpr f64 kAt[7] = {0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9};
constexpr TimingRef kTimingRefs[] = {
    {"in_sine", {0.012311659404862230, 0.076120467488713262, 0.19098300562505255, 0.29289321881345243, 0.41221474770752686, 0.61731656763491016, 0.84356553495976905}},
    {"out_sine", {0.15643446504023087, 0.38268343236508978, 0.58778525229247314, 0.70710678118654746, 0.80901699437494745, 0.92387953251128674, 0.98768834059513777}},
    {"in_out_sine", {0.024471741852423234, 0.14644660940672621, 0.34549150281252627, 0.49999999999999994, 0.65450849718747373, 0.85355339059327373, 0.97552825814757682}},
    {"in_quad", {0.010000000000000002, 0.062500000000000000, 0.16000000000000003, 0.25000000000000000, 0.35999999999999999, 0.56250000000000000, 0.81000000000000005}},
    {"out_quad", {0.18999999999999995, 0.43750000000000000, 0.64000000000000001, 0.75000000000000000, 0.83999999999999997, 0.93750000000000000, 0.98999999999999999}},
    {"in_out_quad", {0.020000000000000004, 0.12500000000000000, 0.32000000000000006, 0.50000000000000000, 0.67999999999999994, 0.87500000000000000, 0.97999999999999998}},
    {"in_cubic", {0.0010000000000000002, 0.015625000000000000, 0.064000000000000015, 0.12500000000000000, 0.21600000000000000, 0.42187500000000000, 0.72900000000000009}},
    {"out_cubic", {0.27099999999999991, 0.57812500000000000, 0.78400000000000003, 0.87500000000000000, 0.93599999999999994, 0.98437500000000000, 0.99900000000000000}},
    {"in_out_cubic", {0.0040000000000000010, 0.062500000000000000, 0.25600000000000006, 0.50000000000000000, 0.74399999999999999, 0.93750000000000000, 0.99600000000000000}},
    {"in_quart", {0.00010000000000000003, 0.0039062500000000000, 0.025600000000000008, 0.062500000000000000, 0.12959999999999999, 0.31640625000000000, 0.65610000000000013}},
    {"out_quart", {0.34389999999999987, 0.68359375000000000, 0.87040000000000006, 0.93750000000000000, 0.97440000000000004, 0.99609375000000000, 0.99990000000000001}},
    {"in_out_quart", {0.00080000000000000026, 0.031250000000000000, 0.20480000000000007, 0.50000000000000000, 0.79519999999999991, 0.96875000000000000, 0.99919999999999998}},
    {"in_quint", {0.000010000000000000004, 0.00097656250000000000, 0.010240000000000004, 0.031250000000000000, 0.077759999999999996, 0.23730468750000000, 0.59049000000000018}},
    {"out_quint", {0.40950999999999993, 0.76269531250000000, 0.92224000000000006, 0.96875000000000000, 0.98975999999999997, 0.99902343750000000, 0.99999000000000005}},
    {"in_out_quint", {0.00016000000000000007, 0.015625000000000000, 0.16384000000000007, 0.50000000000000000, 0.83616000000000001, 0.98437500000000000, 0.99983999999999995}},
    {"in_expo", {0.0019531250000000000, 0.0055242717280199020, 0.015625000000000000, 0.031250000000000000, 0.062500000000000000, 0.17677669529663687, 0.50000000000000000}},
    {"out_expo", {0.50000000000000000, 0.82322330470336313, 0.93750000000000000, 0.96875000000000000, 0.98437500000000000, 0.99447572827198005, 0.99804687500000000}},
    {"in_out_expo", {0.0019531250000000000, 0.015625000000000000, 0.12500000000000000, 0.50000000000000000, 0.87500000000000000, 0.98437500000000000, 0.99804687500000000}},
    {"in_circ", {0.0050125628933800348, 0.031754163448145745, 0.083484861008832012, 0.13397459621556140, 0.19999999999999996, 0.33856217223385232, 0.56411010564593278}},
    {"out_circ", {0.43588989435406728, 0.66143782776614768, 0.80000000000000004, 0.86602540378443860, 0.91651513899116799, 0.96824583655185426, 0.99498743710661997}},
    {"in_out_circ", {0.010102051443364402, 0.066987298107780702, 0.20000000000000007, 0.50000000000000000, 0.79999999999999993, 0.93301270189221930, 0.98989794855663560}},
    {"in_back", {-0.014314220000000004, -0.064136562500000008, -0.099351680000000053, -0.087697500000000039, -0.029027519999999973, 0.18259031249999969, 0.59117202000000013}},
    {"out_back", {0.40882797999999987, 0.81740968750000020, 1.0290275200000001, 1.0876975000000000, 1.0993516800000001, 1.0641365625000001, 1.0143142199999999}},
    {"in_out_back", {-0.037518552000000004, -0.099681843749999999, 0.089925792000000102, 0.50000000000000000, 0.91007420799999994, 1.0996818437500000, 1.0375185519999999}},
    {"in_elastic", {0.0019531249999990266, -0.0055242717280179305, 0.015624999999996269, -0.015625015933805858, -0.031249973674573519, 0.088388296702312505, -0.24999992240927604}},
    {"out_elastic", {1.2499999889156115, 0.91161167978414515, 1.0312499819878669, 1.0156250117771615, 0.98437500000000222, 1.0055242717280186, 0.99804687500000067}},
    {"in_out_elastic", {0.00033909689372132632, 0.011969653676345967, -0.11746112313294280, 0.50000000000366107, 1.1174614503492937, 0.98803042319135670, 0.99966088838489053}},
    {"in_bounce", {0.011875000000000080, 0.027343750000000000, 0.22897500000000004, 0.23309999999999997, 0.085975000000000135, 0.52734375000000000, 0.92437500000000006}},
    {"out_bounce", {0.075625000000000012, 0.47265625000000000, 0.91402499999999987, 0.76690000000000003, 0.77102499999999996, 0.97265625000000000, 0.98812499999999992}},
    {"in_out_bounce", {0.030000000000000027, 0.11654999999999999, 0.34875000000000006, 0.50000000000000000, 0.65124999999999988, 0.88345000000000007, 0.96999999999999997}},
};

struct SpringRef {
    const char* word;
    usize steps;
    f64 at30, at60;
};
// Their presets from 0 to 10 with the tolerance 0.001: the steps until it settles, the position after 30 and 60.
constexpr SpringRef kSpringRefs[] = {
    {"beat", 233, 6.7278275739030793, 10.169778250954819},
    {"plop", 645, 14.609825567223035, 9.4895925077686272},
    {"bounce", 3567, 13.156579833608458, 16.510825251444619},
    {"swing", 301, 7.7063297643106887, 11.518328177077043},
    {"jump", 138, 11.034583130912599, 9.8987014011288874},
    {"strike", 69, 9.9751123599929397, 10.001345883576684},
    {"smooth", 189, 9.4272807296951555, 10.551751905802895},
};

constexpr f64 kInf = std::numeric_limits<f64>::infinity();
struct ArcRef {
    f64 value, ratio;
    bool reverse;
    f64 x, y;
};
constexpr ArcRef kArcRefs[] = {
    {0.25, 2, false, 0.31711854822900659, 0.051614094174145198},
    {0.5, 0.5, false, 0.79335334029123517, 0.39123857099127934},
    {0.75, 3, true, 0.70509776930574097, 0.95552743253741812},
    {0.3, kInf, false, 0.29999999999999999, 0.046060798583054341},
    {0.6, 1, false, 0.80901699437494745, 0.41221474770752686},
};

} // namespace

TEST_CASE("Vector timing: Motion Canvas's curves, words, exact ends") {
    for (usize i = 0; i < kTimingCount; ++i) {
        const auto t = static_cast<Timing>(i);
        INFO(timing_word(t));
        CHECK(timing(t, 0) == 0);
        CHECK(timing(t, 1) == 1);
        CHECK(timing(t, -0.5) == 0);
        CHECK(timing(t, 1.5) == 1);
        CHECK(timing(t, std::numeric_limits<f64>::quiet_NaN()) == 0);
        Timing back = Timing::Linear;
        CHECK(timing_from_word(timing_word(t), back));
        CHECK(back == t);
    }
    CHECK(std::string(timing_word(Timing::InOutBounce)) == "in_out_bounce");
    Timing t = Timing::Back;
    CHECK_FALSE(timing_from_word("easeInSine", t));
    CHECK_FALSE(timing_from_word("", t));
    CHECK(t == Timing::Back);

    // The seven words of the interface are its own curves.
    for (Ease e : kEases) {
        Timing same = Timing::Linear;
        REQUIRE(timing_from_word(ease_word(e), same));
        for (int s = 0; s <= 20; ++s) CHECK(timing(same, s / 20.0) == ease(e, s / 20.0));
    }
    // And «smooth» is their easeInOutCubic, «back» their easeOutBack.
    for (int s = 0; s <= 20; ++s) {
        CHECK(timing(Timing::Smooth, s / 20.0) == doctest::Approx(timing(Timing::InOutCubic, s / 20.0)).epsilon(1e-12));
        CHECK(timing(Timing::Back, s / 20.0) == doctest::Approx(timing(Timing::OutBack, s / 20.0)).epsilon(1e-12));
    }

    for (const TimingRef& ref : kTimingRefs) {
        Timing curve = Timing::Linear;
        REQUIRE(timing_from_word(ref.word, curve));
        for (usize k = 0; k < 7; ++k) {
            INFO(ref.word, " at ", kAt[k]);
            CHECK(std::fabs(timing(curve, kAt[k]) - ref.at[k]) <= 1e-12);
        }
    }
}

TEST_CASE("Vector timing: Motion Canvas's springs") {
    for (const SpringRef& ref : kSpringRefs) {
        INFO(ref.word);
        Spring s;
        REQUIRE(spring_preset(ref.word, s));
        std::vector<f64> steps;
        const char* why = "";
        REQUIRE(spring_steps(s, 0, 10, 0.001, steps, why));
        CHECK(why == nullptr);
        CHECK(steps.size() == ref.steps);
        CHECK(std::fabs(steps[29] - ref.at30) <= 1e-9);
        CHECK(std::fabs(steps[59] - ref.at60) <= 1e-9);
        CHECK(std::fabs(steps.back() - 10) < 0.001);
    }
    Spring s;
    CHECK_FALSE(spring_preset("wobble", s));

    std::vector<f64> steps{1, 2};
    const char* why = nullptr;
    // No damping at all: it rings forever.
    CHECK_FALSE(spring_steps(Spring{1, 10, 0, 0}, 0, 10, 0.001, steps, why));
    CHECK(steps.empty());
    CHECK(std::string(why) == "пружина не успокаивается за минуту: больше затухания");
    CHECK_FALSE(spring_steps(Spring{0, 10, 1, 0}, 0, 10, 0.001, steps, why));
    CHECK(std::string(why) == "у пружины масса должна быть больше 0");
    CHECK_FALSE(spring_steps(Spring{1, -1, 1, 0}, 0, 10, 0.001, steps, why));
    CHECK(std::string(why) == "у пружины жёсткость не меньше 0");
    CHECK_FALSE(spring_steps(Spring{1, 1, -1, 0}, 0, 10, 0.001, steps, why));
    CHECK(std::string(why) == "у пружины затухание не меньше 0");
    CHECK_FALSE(spring_steps(Spring{}, 0, std::numeric_limits<f64>::quiet_NaN(), 0.001, steps, why));
    CHECK(std::string(why) == "у пружины нужны числа, а допуск больше 0");
    CHECK_FALSE(spring_steps(Spring{}, 0, 10, 0, steps, why));
    // Already there and still: one step and done.
    REQUIRE(spring_steps(Spring{}, 5, 5, 0.001, steps, why));
    CHECK(steps.size() == 1);
    CHECK(steps[0] == 5);
}

TEST_CASE("Vector timing: Motion Canvas's arc") {
    for (const ArcRef& ref : kArcRefs) {
        INFO(ref.value, " ratio ", ref.ratio, ref.reverse ? " reversed" : "");
        f64 x = -1, y = -1;
        arc_shares(ref.value, ref.reverse, ref.ratio, x, y);
        CHECK(std::fabs(x - ref.x) <= 1e-12);
        CHECK(std::fabs(y - ref.y) <= 1e-12);
    }
    // The ends are the ends, whichever way it bends.
    for (bool reverse : {false, true})
        for (f64 ratio : {0.25, 1.0, 4.0}) {
            f64 x = -1, y = -1;
            arc_shares(0, reverse, ratio, x, y);
            CHECK(std::fabs(x) <= 1e-12);
            CHECK(std::fabs(y) <= 1e-12);
            arc_shares(1, reverse, ratio, x, y);
            CHECK(std::fabs(x - 1) <= 1e-12);
            CHECK(std::fabs(y - 1) <= 1e-12);
        }
    // No move at all: straight.
    f64 x = -1, y = -1;
    arc_shares(0.4, false, std::numeric_limits<f64>::quiet_NaN(), x, y);
    CHECK(x == 0.4);
    CHECK(y == 0.4);
}

namespace {

CodeSites parsed(std::string_view text) {
    CodeSites code;
    std::string why;
    u32 line = 0;
    REQUIRE_MESSAGE(code.parse(text, why, line), why);
    return code;
}

const CallSite& site_named(const CodeSites& code, std::string_view name, usize nth = 0) {
    for (const CallSite& s : code.sites())
        if (s.name == name && nth-- == 0) return s;
    FAIL("no call ", name);
    return code.sites().front();
}

std::string_view text_of(std::string_view src, CodeSpan s) { return src.substr(s.begin, s.end - s.begin); }

constexpr std::string_view kJump = R"(local m = require("moves")
return {
  params = { height = 6 },
  clips = {
    jump = function(p)
      local body = part("body")
      body:to({ y = 2, sy = 0.9 }, 0.1)
      body.y(0, 0.25, "in_quad"):to(1, 0.05):back(0.1)
      body.y(-p.height, 0.2)
      emit("удар") body.x(3, 0.1)
    end,
    walk = function(p) wait(1) end,
    ["бег"] = function(p)
    end,
  },
}
)";

} // namespace

TEST_CASE("Motion code: every call, its line and its arguments") {
    const CodeSites code = parsed(kJump);
    std::vector<std::string> names;
    for (const CallSite& s : code.sites()) names.push_back(s.name);
    // Outer calls first: back(…) holds to(…), which holds y(…).
    CHECK(names == std::vector<std::string>{"require", "part", "to", "back", "to", "y", "y", "emit", "x", "wait"});

    const CallSite& to = site_named(code, "to");
    CHECK(to.method);
    CHECK(to.parens);
    CHECK(to.line == 7);
    REQUIRE(to.args.size() == 2);
    CHECK(to.args[0].kind == ArgKind::Table);
    REQUIRE(to.args[0].fields.size() == 2);
    CHECK(to.args[0].fields[0].name == "y");
    CHECK(to.args[0].fields[0].value.kind == ArgKind::Number);
    CHECK(to.args[0].fields[0].value.number == 2);
    CHECK(to.args[0].fields[1].name == "sy");
    CHECK(to.args[0].fields[1].value.number == 0.9);
    CHECK(to.args[1].number == 0.1);
    CHECK(text_of(kJump, to.span) == "body:to({ y = 2, sy = 0.9 }, 0.1)");
    CHECK(kJump[to.open] == '(');
    CHECK(kJump[to.close] == ')');

    // The chain on one line: three calls, each its own arguments.
    const CallSite& back = site_named(code, "back");
    CHECK(back.line == 8);
    CHECK(back.args.size() == 1);
    CHECK(text_of(kJump, back.span) == R"(body.y(0, 0.25, "in_quad"):to(1, 0.05):back(0.1))");
    const CallSite& to2 = site_named(code, "to", 1);
    CHECK(text_of(kJump, to2.span) == R"(body.y(0, 0.25, "in_quad"):to(1, 0.05))");
    CHECK(to2.args[1].number == 0.05);
    const CallSite& y = site_named(code, "y");
    CHECK_FALSE(y.method);
    CHECK(y.args.size() == 3);
    CHECK(y.args[2].kind == ArgKind::String);
    CHECK(y.args[2].text == "in_quad");

    const CallSite& y2 = site_named(code, "y", 1);
    CHECK(y2.line == 9);
    CHECK(y2.args[0].kind == ArgKind::Other);
    CHECK_FALSE(y2.args[0].tail);
    CHECK(text_of(kJump, y2.args[0].span) == "-p.height");

    // A Cyrillic string before a call on the same line: columns are bytes.
    const CallSite& emit = site_named(code, "emit");
    CHECK(emit.args[0].text == "удар");
    const CallSite& x = site_named(code, "x");
    CHECK(text_of(kJump, x.span) == "body.x(3, 0.1)");
    CHECK(x.line == 10);

    REQUIRE(code.clips().size() == 3);
    CHECK(code.clip("jump") != nullptr);
    CHECK(code.clip("бег") != nullptr);
    CHECK(code.clip("fly") == nullptr);

    CodeSites bad;
    std::string why;
    u32 line = 0;
    CHECK_FALSE(bad.parse("return {\n  clips = {\n", why, line));
    CHECK(line == 3);
    CHECK_FALSE(why.empty());
    CHECK(bad.sites().empty());
}

TEST_CASE("Motion code: instrumented, every token on its line") {
    auto inst = [](std::string_view text) { return parsed(text).instrumented(0); };
    CHECK(inst("f()") == "__fc(0, f)");
    CHECK(inst("f(1, 2)") == "__fc(0, f, 1, 2)");
    CHECK(inst("o:m()") == R"(__fm(0, o, "m"))");
    CHECK(inst("o:m(1)") == R"(__fm(0, o, "m", 1))");
    CHECK(inst(R"(f"x")") == R"(__fc(0, f, "x"))");
    CHECK(inst("f{1}") == "__fc(0, f, {1})");
    CHECK(inst(R"(o:m{y = 2})") == R"(__fm(0, o, "m", {y = 2}))");
    CHECK(inst(R"(a "x" "y")") == R"(__fc(0, __fc(1, a, "x"), "y"))");
    CHECK(inst("body.y(0, 0.25):to(1, 0.05)") == R"(__fm(0, __fc(1, body.y, 0, 0.25), "to", 1, 0.05))");
    CHECK(inst(R"(part("head").rot(5))") == R"(__fc(0, __fc(1, part, "head").rot, 5))");
    CHECK(inst("all(a.y(1, 2), b(3))") == "__fc(0, all, __fc(1, a.y, 1, 2), __fc(2, b, 3))");
    CHECK(inst("local v = f (x) -- f(y)") == "local v = __fc(0, f, x) -- f(y)");
    // A chain over lines: what follows each newline stays on its line.
    const std::string chain = inst("body.y(0, 0.25)\n  :to(1, 0.05)\n  :back(0.1)\nwait(1)");
    CHECK(chain == "__fm(0, __fm(1, __fc(2, body.y, 0, 0.25)\n, \"to\", 1, 0.05)\n, \"back\", 0.1)\n__fc(3, wait, 1)");
    const std::string jump = inst(kJump);
    CHECK(std::count(jump.begin(), jump.end(), '\n') == std::count(kJump.begin(), kJump.end(), '\n'));
    CHECK(jump.find(R"(__fc(10, emit, "удар") __fm(11, body, "x", 3, 0.1))") == std::string::npos);
    CHECK(jump.find(R"(__fc(7, emit, "удар") __fc(8, body.x, 3, 0.1))") != std::string::npos);
    CHECK(parsed(kJump).instrumented(100).find("__fc(107, emit") != std::string::npos);
}

TEST_CASE("Motion code: a number rewritten, the rest byte for byte") {
    const std::string src = "body.y(2, 0.1)\nbody.y(-2, 0.1) -- keep\nbody.y(-p.height, 0.2)\nf(a or b, 1)";
    const CodeSites code = parsed(src);
    std::string out, why;

    REQUIRE(rewrite_number(src, code.sites()[0].args[0], 2, 3.5, out, why));
    CHECK(out == "body.y(3.5, 0.1)\nbody.y(-2, 0.1) -- keep\nbody.y(-p.height, 0.2)\nf(a or b, 1)");
    REQUIRE(rewrite_number(src, code.sites()[0].args[1], 0.1, 0.123456, out, why));
    CHECK(out.substr(0, 17) == "body.y(2, 0.123)\n");
    REQUIRE(rewrite_number(src, code.sites()[1].args[0], -2, 1, out, why));
    CHECK(out == "body.y(2, 0.1)\nbody.y(1, 0.1) -- keep\nbody.y(-p.height, 0.2)\nf(a or b, 1)");

    // An expression keeps itself and gets a correction; the next edit changes that number.
    REQUIRE(rewrite_number(src, code.sites()[2].args[0], -6, -8, out, why));
    CHECK(out == "body.y(2, 0.1)\nbody.y(-2, 0.1) -- keep\nbody.y(-p.height - 2, 0.2)\nf(a or b, 1)");
    std::string again = out;
    CodeSites code2 = parsed(again);
    CHECK(code2.sites()[2].args[0].tail);
    CHECK(code2.sites()[2].args[0].tail_minus);
    REQUIRE(rewrite_number(again, code2.sites()[2].args[0], -8, -5, out, why));
    CHECK(out.find("body.y(-p.height + 1, 0.2)") != std::string::npos);
    again = out;
    code2 = parsed(again);
    REQUIRE(rewrite_number(again, code2.sites()[2].args[0], -5, -7.5, out, why));
    CHECK(out.find("body.y(-p.height - 1.5, 0.2)") != std::string::npos);
    // The same value: nothing changes.
    REQUIRE(rewrite_number(src, code.sites()[2].args[0], -6, -6.0001, out, why));
    CHECK(out == src);
    // Looser than +: in parentheses.
    REQUIRE(rewrite_number(src, code.sites()[3].args[0], 3, 4, out, why));
    CHECK(out.substr(out.size() - 18) == "f((a or b) + 1, 1)");

    const CodeSites strings = parsed(R"(f("x", {}, function() end))");
    for (const CodeArg& a : strings.sites()[0].args) {
        CHECK_FALSE(rewrite_number(R"(f("x", {}, function() end))", a, 0, 1, out, why));
        CHECK(why == "там не число: строка, таблица или функция");
        CHECK(out.empty());
    }

    CHECK(format_number(0.25) == "0.25");
    CHECK(format_number(2) == "2");
    CHECK(format_number(-3) == "-3");
    CHECK(format_number(12.5) == "12.5");
    CHECK(format_number(0.1 + 0.2) == "0.3");
    CHECK(format_number(-0.0004) == "0");
    CHECK(format_number(1.0005) == "1.001");
    CHECK(quote_string("a\"b\\c\nотрыв") == "\"a\\\"b\\\\c\\nотрыв\"");
}

TEST_CASE("Motion code: an argument or a field added") {
    std::string out, why;
    const std::string src = "body.y(2, 0.1)\nf()\nbody:to{y = 2}";
    const CodeSites code = parsed(src);
    REQUIRE(rewrite_arg(src, code.sites()[0], 2, R"("out_quad")", out, why));
    CHECK(out == "body.y(2, 0.1, \"out_quad\")\nf()\nbody:to{y = 2}");
    REQUIRE(rewrite_arg(src, code.sites()[0], 1, "0.5", out, why));
    CHECK(out == "body.y(2, 0.5)\nf()\nbody:to{y = 2}");
    CHECK_FALSE(rewrite_arg(src, code.sites()[0], 3, "1", out, why));
    CHECK(why == "у вызова меньше аргументов, чем нужно до этого");
    REQUIRE(rewrite_arg(src, code.sites()[1], 0, "1", out, why));
    CHECK(out == "body.y(2, 0.1)\nf(1)\nbody:to{y = 2}");
    REQUIRE(rewrite_arg(src, code.sites()[2], 1, "0.5", out, why));
    CHECK(out == "body.y(2, 0.1)\nf()\nbody:to({y = 2}, 0.5)");

    // Fields: on one line, added after the last.
    const std::string one = "body:to({ y = 2, sy = -p.k }, 0.1)";
    const CodeSites c1 = parsed(one);
    const CodeArg& t1 = c1.sites()[0].args[0];
    REQUIRE(rewrite_field_number(one, t1, "x", 0, 3, out, why));
    CHECK(out == "body:to({ y = 2, sy = -p.k, x = 3 }, 0.1)");
    REQUIRE(rewrite_field_number(one, t1, "y", 2, 5, out, why));
    CHECK(out == "body:to({ y = 5, sy = -p.k }, 0.1)");
    REQUIRE(rewrite_field_number(one, t1, "sy", -1, -0.5, out, why));
    CHECK(out == "body:to({ y = 2, sy = -p.k + 0.5 }, 0.1)");
    REQUIRE(rewrite_field(one, t1, "fill", R"("#ff0000")", out, why));
    CHECK(out == R"(body:to({ y = 2, sy = -p.k, fill = "#ff0000" }, 0.1))");

    const std::string empty = "body:to({}, 0.1)";
    REQUIRE(rewrite_field_number(empty, parsed(empty).sites()[0].args[0], "x", 0, 3, out, why));
    CHECK(out == "body:to({x = 3}, 0.1)");

    // One field a line: a new line like the others, comments where they were.
    const std::string trailing = "body:to({\n  y = 2, -- down\n  sy = 0.9,\n}, 0.1)";
    REQUIRE(rewrite_field_number(trailing, parsed(trailing).sites()[0].args[0], "x", 0, 3, out, why));
    CHECK(out == "body:to({\n  y = 2, -- down\n  sy = 0.9,\n  x = 3,\n}, 0.1)");
    const std::string bare = "body:to({\n    y = 2,\n    sy = 0.9 -- squash\n  }, 0.1)";
    REQUIRE(rewrite_field_number(bare, parsed(bare).sites()[0].args[0], "x", 0, 3, out, why));
    CHECK(out == "body:to({\n    y = 2,\n    sy = 0.9, -- squash\n    x = 3\n  }, 0.1)");
    CHECK(parsed(out).sites()[0].args[0].fields.size() == 3);

    CHECK_FALSE(rewrite_field(src, code.sites()[0].args[0], "x", "1", out, why));
    CHECK(why == "там не таблица");
}

TEST_CASE("Motion code: a new first line of a clip") {
    const CodeSites code = parsed(kJump);
    std::string out, why;
    const std::string line = R"(at(0.25, part("head").rot(5, 0.1)))";
    REQUIRE(insert_clip_line(code, kJump, "jump", line, out, why));
    const std::string want_jump = std::string(kJump).replace(
        std::string(kJump).find("      local body"), 0, "      " + line + "\n");
    CHECK(out == want_jump);
    REQUIRE(insert_clip_line(code, kJump, "walk", line, out, why));
    CHECK(out.find("walk = function(p) " + line + " wait(1) end,") != std::string::npos);
    REQUIRE(insert_clip_line(code, kJump, "бег", line, out, why));
    CHECK(out.find("[\"бег\"] = function(p) " + line + "\n    end,") != std::string::npos);
    // What it wrote parses and has the call.
    const CodeSites after = parsed(out);
    CHECK(after.sites().size() == code.sites().size() + 3);
    CHECK_FALSE(insert_clip_line(code, kJump, "fly", line, out, why));
    CHECK(why == "в коде нет функции клипа «fly» в таблице clips: строку не вставить");
    CHECK(out.empty());
}

namespace {

PoseScript script_of(std::string_view source) {
    PoseScript s;
    const bool ok = s.load(source);
    INFO(s.error());
    REQUIRE(ok);
    return s;
}

// A script of one clip, "c", its body from line 2 on.
std::string clip_of(std::string_view body) {
    return "return { clips = { c = function(p, rng)\n" + std::string(body) + "\nend } }";
}

Recording recorded(PoseScript& s, const MotionLengths& lengths = {}, u64 seed = 1, const Rig* rig = nullptr) {
    Recording rec;
    const bool ok = s.record(rig, "c", {}, seed, lengths, rec);
    INFO(s.error());
    REQUIRE(ok);
    return rec;
}

std::string record_error(PoseScript& s, const MotionLengths& lengths = {}, const Rig* rig = nullptr) {
    Recording rec;
    CHECK_FALSE(s.record(rig, "c", {}, 1, lengths, rec));
    CHECK(rec.blocks.empty());
    return s.error();
}

// The nth block of a part's property, in the order the code made them.
const Block& block(const Recording& rec, std::string_view part, Prop p, usize nth = 0) {
    for (const Block& b : rec.blocks)
        if (rec.parts[b.part] == part && b.prop == p && nth-- == 0) return b;
    FAIL("no block ", part, ".", prop_word(p));
    return rec.blocks.front();
}

usize blocks_of(const Recording& rec, std::string_view part, Prop p) {
    return static_cast<usize>(std::count_if(rec.blocks.begin(), rec.blocks.end(), [&](const Block& b) {
        return rec.parts[b.part] == part && b.prop == p;
    }));
}

void check_block(const Block& b, f64 start, f64 length, f64 value) {
    CHECK(b.start == start);
    CHECK(b.length == length);
    CHECK(b.value.v == value);
}

// A part's pose at t, the recording read and pose(t) on top.
PartPose pose_at(PoseScript& s, const Recording& rec, f64 t, const std::string& part) {
    Pose pose;
    const bool ok = s.frame(rec, nullptr, t, {}, 1, pose);
    INFO(s.error());
    REQUIRE(ok);
    return pose.parts[part];
}

// Every block, save, event, mark, pause and log line: two recordings of the same code are the same text.
std::string dump(const Recording& rec) {
    std::string out = rec.clip + " " + format_number(rec.length) + "\n";
    char buf[256];
    for (const Block& b : rec.blocks) {
        std::snprintf(buf, sizeof buf, "%s.%s %.17g %.17g %u to%d %.17g %.17g→%.17g site %u:%d\n", rec.parts[b.part].c_str(),
                      prop_word(b.prop), b.start, b.length, b.order, static_cast<int>(b.to), b.value.v, b.from.v, b.end.v,
                      b.site.file, b.site.site);
        out += buf;
    }
    for (const TimeEvent& e : rec.events) out += e.name + " " + format_number(e.start) + " " + format_number(e.length) + "\n";
    for (const Mark& m : rec.marks) out += m.name + " " + format_number(m.time) + "\n";
    for (const Pause& p : rec.pauses) out += "wait " + format_number(p.start) + " " + format_number(p.length) + "\n";
    for (const LogLine& l : rec.log) out += "log " + format_number(l.time) + " " + l.text + "\n";
    return out;
}

} // namespace

TEST_CASE("Motion clip: the flow functions start and end as Motion Canvas's") {
    PoseScript s = script_of(clip_of(R"(local a, b, c = part("a"), part("b"), part("c")
a.x(1, 0.5)
all(a.y(1, 0.2), b.y(1, 0.3))
any(a.x(2, 0.2), b.x(2, 0.4))
chain(a.rot(10, 0.1), b.rot(10, 0.2))
sequence(0.2, c.x(1, 0.1), c.y(1, 0.1), c.rot(1, 0.05))
local s = spawn(a.sx(2, 1))
c.sx(2, 0.25)
join(s)
delay(0.5, b.sy(2, 0.1))
at(0.25, c.opacity(0, 0.1))
wait(0.2)
all(function() a.scale(2, 0.2) a.scale(1, 0.2) end, b.scale(3, 0.1)))"));
    const Recording rec = recorded(s);
    check_block(block(rec, "a", Prop::X), 0, 0.5, 1);
    // all: both from 0.5, on when the longer ends; any: when the shorter ends, the other runs on.
    check_block(block(rec, "a", Prop::Y), 0.5, 0.2, 1);
    check_block(block(rec, "b", Prop::Y), 0.5, 0.3, 1);
    check_block(block(rec, "a", Prop::X, 1), 0.8, 0.2, 2);
    check_block(block(rec, "b", Prop::X), 0.8, 0.4, 2);
    check_block(block(rec, "a", Prop::Rot), 1.0, 0.1, 10);
    check_block(block(rec, "b", Prop::Rot), 1.1, 0.2, 10);
    // sequence: each 0.2 after the one before, and 0.2 after the last one too, as theirs: on at 1.3 + 3 × 0.2.
    check_block(block(rec, "c", Prop::X), 1.3, 0.1, 1);
    check_block(block(rec, "c", Prop::Y), 1.5, 0.1, 1);
    check_block(block(rec, "c", Prop::Rot), 1.7, 0.05, 1);
    // spawn does not wait; join waits for what it spawned; delay starts 0.5 after.
    check_block(block(rec, "a", Prop::Sx), 1.9, 1, 2);
    check_block(block(rec, "c", Prop::Sx), 1.9, 0.25, 2);
    const Block& delayed = block(rec, "b", Prop::Sy);
    check_block(delayed, 3.4, 0.1, 2);
    // at: from the clip's start, whenever its line runs; the line after it does not wait.
    const Block& at = block(rec, "c", Prop::Opacity);
    check_block(at, 0.25, 0.1, 0);
    REQUIRE(rec.pauses.size() == 1);
    CHECK(rec.pauses[0].start == 3.5);
    CHECK(rec.pauses[0].length == 0.2);
    // A function given to all runs as its own line of work.
    check_block(block(rec, "a", Prop::Scale), 3.7, 0.2, 2);
    check_block(block(rec, "a", Prop::Scale, 1), 3.9, 0.2, 1);
    check_block(block(rec, "b", Prop::Scale), 3.7, 0.1, 3);
    CHECK(rec.length == 4.1);
    CHECK(rec.blocks.size() == 17);

    // Each block knows its call: its line, which argument is the value, the length, the timing.
    const CodeSites& code = s.code();
    auto line_of = [&](const Block& b) { return code.sites()[static_cast<usize>(b.site.site)].line; };
    CHECK(line_of(block(rec, "a", Prop::X)) == 3);
    CHECK(line_of(block(rec, "c", Prop::Rot)) == 7);
    CHECK(line_of(block(rec, "a", Prop::Scale, 1)) == 14);
    CHECK(block(rec, "a", Prop::X).value_arg == 0);
    CHECK(block(rec, "a", Prop::X).length_arg == 1);
    CHECK(block(rec, "a", Prop::X).timing_arg == 2);
    CHECK(code.sites()[static_cast<usize>(at.start_site.site)].name == "at");
    CHECK(code.sites()[static_cast<usize>(delayed.start_site.site)].name == "delay");
    CHECK(block(rec, "a", Prop::Sx).start_site.site == -1);
}

TEST_CASE("Motion clip: time events, loops and marks") {
    PoseScript s = script_of(clip_of(R"(local a = part("a")
wait_until("отрыв", 0.15)
a.x(1, 0.1)
loop_until("бег", function(i) a.y(i, 0.3) end, 1)
local d = duration("оборот", 0.6)
a.rot(360, d)
loop(3, function(i) a.sx(i, 0.1) end)
loop_for(0.25, function() a.sy(2, 0.1) end)
emit("удар", 0.05) sound("шаг")
log("конец", now()))"));
    MotionLengths lengths;
    lengths.waits["отрыв"] = 0.18;
    lengths.durations["бег"] = 0.6;
    const Recording rec = recorded(s, lengths);
    REQUIRE(rec.events.size() == 3);
    CHECK(rec.events[0].name == "отрыв");
    CHECK(rec.events[0].wait);
    CHECK(rec.events[0].start == 0);
    CHECK(rec.events[0].length == 0.18);
    CHECK(rec.events[0].from_file);
    check_block(block(rec, "a", Prop::X), 0.18, 0.1, 1);
    // loop_until: from 0.28 for the file's 0.6, the index from 0 as theirs; the line after starts at its end.
    CHECK(rec.events[1].name == "бег");
    CHECK_FALSE(rec.events[1].wait);
    CHECK(rec.events[1].start == 0.28);
    CHECK(rec.events[1].length == 0.6);
    CHECK(blocks_of(rec, "a", Prop::Y) == 2);
    check_block(block(rec, "a", Prop::Y), 0.28, 0.3, 0);
    check_block(block(rec, "a", Prop::Y, 1), 0.58, 0.3, 1);
    // duration: the code's 0.6, not in the file; it takes no time itself.
    CHECK(rec.events[2].name == "оборот");
    CHECK_FALSE(rec.events[2].from_file);
    CHECK(rec.events[2].start == 0.88);
    check_block(block(rec, "a", Prop::Rot), 0.88, 0.6, 360);
    REQUIRE(blocks_of(rec, "a", Prop::Sx) == 3);
    for (usize i = 0; i < 3; ++i) check_block(block(rec, "a", Prop::Sx, i), snap_time(1.48 + 0.1 * static_cast<f64>(i)), 0.1, static_cast<f64>(i));
    // loop_for: an iteration starts while time is left; the line after starts at its end.
    CHECK(blocks_of(rec, "a", Prop::Sy) == 3);
    CHECK(block(rec, "a", Prop::Sy, 2).start == 1.98);
    REQUIRE(rec.marks.size() == 2);
    CHECK(rec.marks[0].kind == Mark::Kind::Emit);
    CHECK(rec.marks[0].name == "удар");
    CHECK(rec.marks[0].time == 2.03);
    CHECK(rec.marks[0].length == 0.05);
    CHECK(rec.marks[1].kind == Mark::Kind::Sound);
    CHECK(rec.marks[1].time == 2.03);
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "конец\t2.03");
    CHECK(rec.log[0].line == 11);
    CHECK(rec.length == 2.03);

    // Without the file the code's lengths: loop_until runs 1 s, four times.
    const Recording plain = recorded(s);
    CHECK(plain.events[0].length == 0.15);
    CHECK_FALSE(plain.events[0].from_file);
    CHECK(blocks_of(plain, "a", Prop::Y) == 4);
    CHECK(plain.length == 2.4); // 0.15 + 0.1 + 1 + 0.6 + 0.3 + 0.25

    // loop(f) with no count runs to the clip's end, in the background only.
    PoseScript bg = script_of(clip_of(R"(local a = part("a")
spawn(loop(function(i) a.y(i, 0.25) end))
wait(1))"));
    const Recording forever = recorded(bg);
    CHECK(forever.length == 1);
    REQUIRE(blocks_of(forever, "a", Prop::Y) == 4);
    check_block(block(forever, "a", Prop::Y, 3), 0.75, 0.25, 3);
    PoseScript bad = script_of(clip_of(R"(local a = part("a")
a.x(1, 0.1)
loop(function(i) a.y(i, 0.25) end))"));
    CHECK(record_error(bad) == "строка 4: loop(f) без числа повторов идёт до конца клипа: только прямо в spawn(…) или "
                               "at(…); иначе loop(n, f) или loop_for(с, f)");
    CHECK(bad.error_line() == 4);
    PoseScript idle = script_of(clip_of("loop_for(1, function() end)"));
    CHECK(record_error(idle) == "строка 2: повтор не занял времени и шёл бы вечно: в нём нужен переход или wait(с)");
}

TEST_CASE("Motion clip: a frame read from the tracks, exact at the ends") {
    PoseScript s = script_of(clip_of(R"(local body = part("body")
body:to({ y = 2, sy = 0.9 }, 0.1)
body.y(-6, 0.25, "out_quad"):to(1, 0.05):back(0.1)
body.x(4, 1, "linear"))"));
    const Recording rec = recorded(s);
    REQUIRE(blocks_of(rec, "body", Prop::Y) == 4);
    // back returns to where the chain's first transition of y started.
    CHECK(block(rec, "body", Prop::Y, 3).to == Block::To::Back);
    CHECK(block(rec, "body", Prop::Y, 3).end.v == 2);
    auto y = [&](f64 t) { return pose_at(s, rec, t, "body").y; };
    CHECK(y(0) == 0);
    CHECK(y(0.1) == 2);
    CHECK(y(0.225) == doctest::Approx(2 - 8 * 0.75)); // out_quad at ½ is ¾
    CHECK(y(0.35) == -6);
    CHECK(y(0.4) == 1);
    CHECK(y(0.5) == 2);
    CHECK(y(0.9) == 2);
    CHECK(pose_at(s, rec, 0.05, "body").sy == doctest::Approx(0.95));
    CHECK(pose_at(s, rec, 0.1, "body").sy == 0.9);
    CHECK(pose_at(s, rec, 0.5, "body").x == 0);
    CHECK(pose_at(s, rec, 1.0, "body").x == 2);
    CHECK(pose_at(s, rec, 1.5, "body").x == 4);
    // A later block of a track cuts one still running: x from 0, cut at 0.3 by a jump to 9.
    PoseScript cut = script_of(clip_of(R"(local a = part("a")
spawn(a.x(10, 1, "linear"))
wait(0.3)
a.x(9, 0.2, "linear"))"));
    const Recording c = recorded(cut);
    CHECK(block(c, "a", Prop::X, 1).from.v == doctest::Approx(3));
    CHECK(pose_at(cut, c, 0.4, "a").x == doctest::Approx(6));
    CHECK(pose_at(cut, c, 0.5, "a").x == 9);
    CHECK(pose_at(cut, c, 0.9, "a").x == 9);
}

TEST_CASE("Motion clip: the same recording each time") {
    const std::string src = clip_of(R"(local a, b = part("a"), part("b")
loop(4, function(i) all(a.x(rng(1, 10), 0.1), b.rot(rng() * 90, 0.2)) end)
a:spring({ x = 0, y = 3 })
tween(0.5, function(k) b.y(10 * k) end)
log(a.x(), b.y()))");
    PoseScript s = script_of(src);
    const std::string first = dump(recorded(s, {}, 7));
    CHECK(dump(recorded(s, {}, 7)) == first);
    PoseScript again = script_of(src);
    CHECK(dump(recorded(again, {}, 7)) == first);
    CHECK(dump(recorded(s, {}, 8)) != first);
    CHECK(first.find("log ") != std::string::npos);
}

TEST_CASE("Motion clip: bindings, getters and pose on top") {
    PoseScript s = script_of(R"(return {
  clips = { c = function(p)
    local a, b = part("a"), part("b")
    b.x(function() return a.x() * 2 end)
    a.x(4, 1, "linear")
    log(a.x(), b.x(), now())
    a.y(2, 1, "linear")
  end },
  pose = function(t, p, rng, clip)
    part("a"):set{ x = 1, sx = 2 }
  end,
})");
    const Recording rec = recorded(s);
    // What the code reads is what is recorded by then.
    REQUIRE(rec.log.size() == 1);
    CHECK(rec.log[0].text == "4\t8\t1");
    CHECK(block(rec, "b", Prop::X).to == Block::To::Binding);
    CHECK(block(rec, "b", Prop::X).start == 0);
    const PartPose a = pose_at(s, rec, 0.5, "a");
    CHECK(a.x == doctest::Approx(3)); // 2 recorded, 1 from pose
    CHECK(a.sx == 2);
    CHECK(pose_at(s, rec, 0.5, "b").x == doctest::Approx(4)); // twice the recorded 2
    CHECK(pose_at(s, rec, 1.5, "a").y == doctest::Approx(1));
    // A binding that reads itself stops at the depth limit, at the line that reads.
    PoseScript loop = script_of(clip_of(R"(local b = part("b")
b.x(function() return b.x() + 1 end))"));
    const Recording self = recorded(loop);
    Pose pose;
    CHECK_FALSE(loop.frame(self, nullptr, 0, {}, 1, pose));
    CHECK(loop.error() == "строка 3: связи ссылаются друг на друга по кругу");
    CHECK(pose.parts.empty());
}

TEST_CASE("Motion clip: a binding is a function of time, whatever frames were read before") {
    const std::string src = clip_of(R"(local a, b, c = part("a"), part("b"), part("c")
a.x(function() return rng() end)
b.x(function() return a.x() - a.x() end)
c.x(function() return a.x() * 2 + rng(1, 6) end)
log(a.x(), rng())
wait(1))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    auto frame_at = [&](PoseScript& script, const Recording& r, f64 t) {
        Pose pose;
        const bool ok = script.frame(r, nullptr, t, {}, 1, pose);
        INFO(script.error());
        REQUIRE(ok);
        return std::vector<f64>{pose.parts["a"].x, pose.parts["b"].x, pose.parts["c"].x};
    };
    const std::vector<f64> half = frame_at(s, rec, 0.5);
    CHECK(half[0] >= 0);
    CHECK(half[0] < 1);
    CHECK(half[1] == 0); // a.x read twice in one frame: the same value
    const f64 die = half[2] - 2 * half[0];
    CHECK(die == std::floor(die));
    CHECK(die >= 1);
    CHECK(die <= 6);
    // The same t again; back to 0.1 and to 0.5 again; the frames in another order.
    CHECK(frame_at(s, rec, 0.5) == half);
    const std::vector<f64> early = frame_at(s, rec, 0.1);
    CHECK(early[0] != half[0]);
    CHECK(frame_at(s, rec, 0.5) == half);
    CHECK(frame_at(s, rec, 0.1) == early);
    // The recording read it too, and it is the same there: a.x at 0 and the clip's own chance after it untouched.
    REQUIRE(rec.log.size() == 1);
    PoseScript plain = script_of(clip_of("log(0, rng())\nwait(1)"));
    const Recording plain_rec = recorded(plain);
    REQUIRE(plain_rec.log.size() == 1);
    CHECK(rec.log[0].text.substr(rec.log[0].text.find('\t')) == plain_rec.log[0].text.substr(plain_rec.log[0].text.find('\t')));
    // A new script of the same code, recorded again, its first frames the other way round.
    PoseScript again = script_of(src);
    const Recording rec2 = recorded(again);
    CHECK(frame_at(again, rec2, 0.1) == early);
    CHECK(frame_at(again, rec2, 0.5) == half);
    // Another seed is another chance.
    Pose other;
    REQUIRE(s.frame(rec, nullptr, 0.5, {}, 2, other));
    CHECK(other.parts["a"].x != half[0]);
}

TEST_CASE("Motion clip: a binding that keeps something between frames is refused at its line") {
    auto refused = [](std::string_view body, f64 t) {
        PoseScript s = script_of(clip_of(body));
        const Recording rec = recorded(s);
        Pose pose;
        CHECK_FALSE(s.frame(rec, nullptr, t, {}, 1, pose));
        CHECK(pose.parts.empty());
        // And again: the same answer, not a count.
        CHECK_FALSE(s.frame(rec, nullptr, t, {}, 1, pose));
        return s.error();
    };
    const std::string kept = "у связи нет памяти между кадрами, она считает кадр только из времени и значений частей";
    // A local of the clip counted up, as the review's probe did: 1, 2, 3 at 0.5, 0.1, 0.5 before.
    CHECK(refused("local a = part('a')\nlocal count = 0\na.x(function() count += 1 return count end)\nwait(1)", 0.5) ==
          "строка 4: связь меняет переменную или буфер вне себя: " + kept);
    // Through a function it calls.
    CHECK(refused("local a = part('a')\nlocal count = 0\nlocal function bump() count += 1 return count end\n"
                  "a.x(function() return bump() end)\nwait(1)",
                  0.5) == "строка 5: связь меняет переменную или буфер вне себя: " + kept);
    // A table of the clip, and a global: at the line that writes.
    CHECK(refused("local a = part('a')\nlocal seen = {}\na.x(function()\n  seen[#seen + 1] = now()\n  return #seen\nend)\nwait(1)",
                  0.5) == "строка 5: связь меняет таблицу или глобальную переменную вне себя: " + kept);
    CHECK(refused("local a = part('a')\na.x(function()\n  n = (n or 0) + 1\n  return n\nend)\nwait(1)", 0.5) ==
          "строка 4: связь меняет таблицу или глобальную переменную вне себя: " + kept);
    // A buffer's bytes; a coroutine, whose steps cannot be put back.
    CHECK(refused("local a = part('a')\nlocal b = buffer.create(1)\n"
                  "a.x(function() buffer.writeu8(b, 0, buffer.readu8(b, 0) + 1) return buffer.readu8(b, 0) end)\nwait(1)",
                  0.5) == "строка 4: связь меняет переменную или буфер вне себя: " + kept);
    CHECK(refused("local a = part('a')\nlocal gen = coroutine.wrap(function() local i = 0 while true do i += 1 coroutine.yield(i) end end)\n"
                  "a.x(function() return gen() end)\nwait(1)",
                  0.5) == "строка 4: связь держит сопрограмму (coroutine), а её ход между кадрами не повторить: " + kept);
    // Its own locals, tables and calls are its own.
    PoseScript own = script_of(clip_of(R"(local a, b = part("a"), part("b")
local function twice(v) local t = { v } t[2] = v return t[1] + t[2] end
b.x(1)
a.x(function() local t = {} t.k = now() return twice(t.k) + b.x() end)
wait(1))"));
    const Recording own_rec = recorded(own);
    CHECK(pose_at(own, own_rec, 0.5, "a").x == 2);
    CHECK(pose_at(own, own_rec, 0.5, "a").x == 2);
    // Read while the clip records, then the clip goes on writing what the binding reads.
    PoseScript read = script_of(clip_of(R"(local a = part("a")
local st = { v = 1 }
a.x(function() return st.v end)
log(a.x())
st.v = 3
g = 4
wait(1))"));
    const Recording read_rec = recorded(read);
    REQUIRE(read_rec.log.size() == 1);
    CHECK(read_rec.log[0].text == "1");
    CHECK(pose_at(read, read_rec, 0.5, "a").x == 3);

    // Refused where it would change something, and the frames elsewhere as if it never ran.
    PoseScript s = script_of(clip_of(R"(local a, b = part("a"), part("b")
local level, seen = 1, { 1 }
a.x(function() if now() > 0.5 then level = 5 end return level end)
b.x(function() if now() > 0.5 then seen[1] = 7 end return seen[1] end)
wait(1))"));
    const Recording rec = recorded(s);
    CHECK(pose_at(s, rec, 0.25, "a").x == 1);
    Pose pose;
    CHECK_FALSE(s.frame(rec, nullptr, 0.75, {}, 1, pose));
    CHECK(s.error() == "строка 4: связь меняет переменную или буфер вне себя: " + kept);
    CHECK(pose_at(s, rec, 0.25, "a").x == 1);
    CHECK(pose_at(s, rec, 0.25, "b").x == 1);

    // What the clip changes itself after making the binding, the binding reads: the value the recording ended with.
    PoseScript late = script_of(clip_of("local a = part('a')\nlocal h = 2\na.x(function() return h end)\nh = 5\nwait(1)"));
    const Recording late_rec = recorded(late);
    CHECK(pose_at(late, late_rec, 0.5, "a").x == 5);
    CHECK(pose_at(late, late_rec, 0.5, "a").x == 5);
}

TEST_CASE("Motion clip: a binding keeps nothing through require, its environment or a library") {
    const std::string kept = "у связи нет памяти между кадрами, она считает кадр только из времени и значений частей";
    // The review's module counting in a local, one counting in its own table, one with nothing to keep.
    auto with_modules = [](std::string_view body) {
        PoseScript s;
        s.set_module_reader([](std::string_view name, std::string& text, std::string& why) {
            if (name == "counter") text = "local count = 0\nreturn { bump = function() count += 1 return count end }";
            else if (name == "tally") text = "local M = { n = 0 }\nfunction M.bump()\n  M.n += 1\n  return M.n\nend\nreturn M";
            else if (name == "calc") text = "return { double = function(v) return v * 2 end }";
            else return why = "нет", false;
            return true;
        });
        const bool loaded = s.load(clip_of(body));
        INFO(s.error());
        REQUIRE(loaded);
        return s;
    };
    // At 0.5, 0.1 and 0.5 again, as the review read it (1, 2, 3 before): refused each time, with the same words.
    auto refused = [](PoseScript& s, const Recording& rec) {
        std::string first;
        for (f64 t : {0.5, 0.1, 0.5}) {
            Pose pose;
            CHECK_FALSE(s.frame(rec, nullptr, t, {}, 1, pose));
            CHECK(pose.parts.empty());
            if (first.empty()) first = s.error();
            CHECK(s.error() == first);
        }
        return first;
    };
    const std::string from_require = "внутри связи: модуль живёт дольше кадра, возьмите его в начале кода или клипа "
                                     "(local m = require(\"counter\")) и зовите из связи";
    // require in the binding: the module loaded there the first time, or loaded by the clip before.
    PoseScript first_load = with_modules("local a = part('a')\na.x(function() return require(\"counter\").bump() end)\nwait(1)");
    const Recording first_rec = recorded(first_load);
    CHECK(refused(first_load, first_rec) == "строка 3: require(\"counter\") " + from_require);
    PoseScript cached = with_modules("local a = part('a')\nrequire(\"counter\")\na.x(function() return require(\"counter\").bump() end)\nwait(1)");
    const Recording cached_rec = recorded(cached);
    CHECK(refused(cached, cached_rec) == "строка 4: require(\"counter\") " + from_require);
    // Read while the clip records: the recording is refused there.
    PoseScript while_recording =
        with_modules("local a = part('a')\na.x(function() return require(\"counter\").bump() end)\nlog(a.x())\nwait(1)");
    CHECK(record_error(while_recording) == "строка 3: require(\"counter\") " + from_require);
    // A module taken before: what it keeps in a local or in its own table is refused as the clip's own would be.
    PoseScript held = with_modules("local a = part('a')\nlocal counter = require(\"counter\")\na.x(function() return counter.bump() end)\nwait(1)");
    const Recording held_rec = recorded(held);
    CHECK(refused(held, held_rec) == "строка 4: связь меняет переменную или буфер вне себя: " + kept);
    PoseScript table = with_modules("local a = part('a')\nlocal tally = require(\"tally\")\na.x(function() return tally.bump() end)\nwait(1)");
    const Recording table_rec = recorded(table);
    CHECK(refused(table, table_rec) == "tally.luau, строка 3: связь меняет таблицу или глобальную переменную вне себя: " + kept);
    // One that keeps nothing serves the binding at any t, in any order.
    PoseScript pure = with_modules("local a = part('a')\nlocal calc = require(\"calc\")\na.x(function() return calc.double(now()) end)\nwait(1)");
    const Recording pure_rec = recorded(pure);
    CHECK(pose_at(pure, pure_rec, 0.5, "a").x == doctest::Approx(1));
    CHECK(pose_at(pure, pure_rec, 0.1, "a").x == doctest::Approx(0.2));
    CHECK(pose_at(pure, pure_rec, 0.5, "a").x == doctest::Approx(1));

    // An iterator of the string library keeps its place in itself: refused as a local is.
    PoseScript digits = script_of(clip_of("local a = part('a')\nlocal next_digit = string.gmatch('1 2 3 4 5', '%d')\n"
                                          "a.x(function() return tonumber(next_digit()) end)\nwait(1)"));
    const Recording digits_rec = recorded(digits);
    CHECK(refused(digits, digits_rec) == "строка 4: связь меняет переменную или буфер вне себя: " + kept);
    // A userdata's metatable is a table it reaches.
    PoseScript proxy = script_of(clip_of("local a = part('a')\nlocal u = newproxy(true)\na.x(function()\n"
                                         "  local m = getmetatable(u)\n  m.n = (m.n or 0) + 1\n  return m.n\nend)\nwait(1)"));
    const Recording proxy_rec = recorded(proxy);
    CHECK(refused(proxy, proxy_rec) == "строка 6: связь меняет таблицу или глобальную переменную вне себя: " + kept);
    // Its own environment, set from inside it, and the thread's: the code has no getfenv and setfenv, nor gcinfo, the
    // heap's size.
    PoseScript env = script_of(clip_of("local a = part('a')\na.x(function()\n  local e = getfenv(1)\n"
                                       "  if rawget(e, 'seen') then return 2 end\n"
                                       "  setfenv(1, setmetatable({ seen = true }, { __index = e }))\n  return 1\nend)\nwait(1)"));
    const Recording env_rec = recorded(env);
    CHECK(refused(env, env_rec) == "строка 4: getfenv — не функция (nil)");
    PoseScript none = script_of(clip_of("local a = part('a')\na.x(function() return (getfenv or setfenv or gcinfo) and 1 or 0 end)\nwait(1)"));
    const Recording none_rec = recorded(none);
    CHECK(pose_at(none, none_rec, 0.5, "a").x == 0);
}

TEST_CASE("Motion clip: no address of a table or a function reaches a binding's number or the log") {
    // The review's formula: a number read from tostring({}). It is the same refusal at every t, in any order, in a new
    // script too, at the binding's line.
    const std::string address = "local a = part('a')\na.x(function() return tonumber(string.match(tostring({}), \"0x(%x+)\"), 16) end)\nwait(1)";
    PoseScript s = script_of(clip_of(address));
    const Recording rec = recorded(s);
    std::vector<std::string> said;
    for (f64 t : {0.5, 0.1, 0.5}) {
        Pose pose;
        CHECK_FALSE(s.frame(rec, nullptr, t, {}, 1, pose));
        CHECK(pose.parts.empty());
        said.push_back(s.error());
    }
    PoseScript again = script_of(clip_of(address));
    const Recording again_rec = recorded(again);
    Pose pose;
    CHECK_FALSE(again.frame(again_rec, nullptr, 0.5, {}, 1, pose));
    said.push_back(again.error());
    for (const std::string& e : said) CHECK(e == "строка 3: invalid argument #1 to 'tonumber' (string expected, got nil)");
    // Without tonumber: no hex digits to read, the binding gives no number, refused at its line.
    PoseScript digits = script_of(clip_of("local a = part('a')\na.x(function() return string.match(tostring({}), '0x(%x+)') end)\nwait(1)"));
    const Recording digits_rec = recorded(digits);
    for (f64 t : {0.5, 0.1, 0.5}) {
        CHECK_FALSE(digits.frame(digits_rec, nullptr, t, {}, 1, pose));
        CHECK(digits.error() == "строка 3: связь x вернула не число");
    }

    // Tables, functions, coroutines and buffers are told by their kind, the same in every frame and every script;
    // numbers, strings, booleans, nil and a __tostring of the author's are as they were.
    const std::string kinds = R"(local a, b = part("a"), part("b")
local named = setmetatable({}, { __tostring = function() return "мой" end })
local function text()
  return tostring({}) .. "|" .. tostring(print) .. "|" .. string.format("%*|%*|%*", function() end, coroutine.create(print), buffer.create(1))
    .. "|" .. tostring(named) .. "|" .. tostring(1.5) .. "|" .. tostring(nil) .. "|" .. string.format("%d %s %*|%5.2f|%d%%%*", 3, "x", true, 2, 4, print)
end
local want = "table|function|function|thread|buffer|мой|1.5|nil|3 x true| 2.00|4%function"
a.x(function() return text() == want and 1 or 0 end)
b.x(function() return #text() end)
log({}, text(), 2)
wait(1))";
    PoseScript k = script_of(clip_of(kinds));
    const Recording k_rec = recorded(k);
    REQUIRE(k_rec.log.size() == 1);
    CHECK(k_rec.log[0].text == "table\ttable|function|function|thread|buffer|мой|1.5|nil|3 x true| 2.00|4%function\t2");
    PoseScript k2 = script_of(clip_of(kinds));
    const Recording k2_rec = recorded(k2);
    REQUIRE(k2_rec.log.size() == 1);
    CHECK(k2_rec.log[0].text == k_rec.log[0].text);
    for (f64 t : {0.5, 0.1, 0.5}) {
        CHECK(pose_at(k, k_rec, t, "a").x == 1);
        CHECK(pose_at(k2, k2_rec, t, "a").x == 1);
        CHECK(pose_at(k, k_rec, t, "b").x == pose_at(k2, k2_rec, t, "b").x);
    }
    // A format still refuses what it refused: a table where a string or a number goes.
    PoseScript bad = script_of(clip_of("local a = part('a')\na.x(function() return #string.format('%s', {}) end)\nwait(1)"));
    const Recording bad_rec = recorded(bad);
    CHECK_FALSE(bad.frame(bad_rec, nullptr, 0.5, {}, 1, pose));
    CHECK(bad.error().find("строка 3: ") == 0);
    CHECK(bad.error().find("string expected, got table") != std::string::npos);
}

TEST_CASE("Motion clip: tween, save and restore, rest") {
    PoseScript s = script_of(clip_of(R"(local a = part("a")
tween(0.5, function(k) a.x(10 * k) a.y(k * k) end)
a:to({ x = 5, rot = 30 }, 0.2)
a:save()
a:to({ x = -5, y = 3 }, 0.2)
a:restore(0.1)
a.y(7, 0.1):back(0.1)
a.x:rest(0.1))"));
    const Recording rec = recorded(s);
    const Block& tx = block(rec, "a", Prop::X);
    CHECK(tx.frames.size() == 31);
    CHECK(pose_at(s, rec, 0.25, "a").x == doctest::Approx(5));
    CHECK(pose_at(s, rec, 0.25, "a").y == doctest::Approx(0.25));
    CHECK(pose_at(s, rec, 0.5, "a").x == 10);
    CHECK(pose_at(s, rec, 0.7, "a").x == 5);
    CHECK(pose_at(s, rec, 0.7, "a").rot == 30);
    CHECK(pose_at(s, rec, 0.9, "a").x == -5);
    CHECK(pose_at(s, rec, 0.9, "a").y == 3);
    // restore: every property back to what save kept at 0.7.
    const PartPose restored = pose_at(s, rec, 1.0, "a");
    CHECK(restored.x == 5);
    CHECK(restored.y == 1);
    CHECK(restored.rot == 30);
    CHECK(restored.sx == 1);
    CHECK(pose_at(s, rec, 1.1, "a").y == 7);
    CHECK(pose_at(s, rec, 1.2, "a").y == 1);
    CHECK(pose_at(s, rec, 1.3, "a").x == 0);
    CHECK(pose_at(s, rec, 1.3, "a").rot == 30);
    CHECK(rec.length == 1.3);
}

TEST_CASE("Motion clip: springs and the arc") {
    PoseScript s = script_of(clip_of(R"(local a = part("a")
a.x:spring(10)
a:to({ x = 0, y = 10 }, 1, "linear", "arc")
spring(a.y, 0, "jump"))"));
    const Recording rec = recorded(s);
    Spring plop, jump;
    REQUIRE(spring_preset("plop", plop));
    REQUIRE(spring_preset("jump", jump));
    std::vector<f64> steps;
    const char* why = nullptr;
    REQUIRE(spring_steps(plop, 0, 10, 0.001, steps, why));
    const Block& sx = block(rec, "a", Prop::X);
    CHECK(sx.spring);
    CHECK(sx.length == snap_time(static_cast<f64>(steps.size()) / kSpringRate));
    CHECK(pose_at(s, rec, 1.0 / kSpringRate, "a").x == doctest::Approx(steps[0]));
    CHECK(pose_at(s, rec, sx.length, "a").x == 10);
    // The arc: x and y on a quarter of an ellipse, as theirs; straight it would be 5 and 5.
    const f64 t0 = sx.length;
    f64 kx = 0, ky = 0;
    arc_shares(0.5, false, (10.0 - 0.0) / (0.0 - 10.0), kx, ky);
    const PartPose mid = pose_at(s, rec, t0 + 0.5, "a");
    CHECK(mid.x == doctest::Approx(10 - 10 * kx));
    CHECK(mid.y == doctest::Approx(10 * ky));
    CHECK(std::fabs(mid.x - 5) > 1);
    CHECK(pose_at(s, rec, t0 + 1, "a").x == 0);
    CHECK(pose_at(s, rec, t0 + 1, "a").y == 10);
    // spring(prop, v, kind): Motion Canvas's form; its value is the second argument.
    const Block& sy = block(rec, "a", Prop::Y, 1);
    CHECK(sy.spring);
    CHECK(sy.value_arg == 1);
    CHECK(sy.spring_def.stiffness == jump.stiffness);
    CHECK(sy.start == snap_time(t0 + 1));
    CHECK(pose_at(s, rec, sy.start + sy.length, "a").y == 0);
}

TEST_CASE("Motion clip: errors name the line of their call") {
    auto error_of = [](std::string_view body) {
        PoseScript s = script_of(clip_of(body));
        return record_error(s);
    };
    CHECK(error_of("local a = part('a')\na.x('left', 1)") == "строка 3: x: нужно число, а не string");
    CHECK(error_of("part('a').x(1, 0.1, 'fast')") ==
          "строка 2: нет сглаживания «fast» (smooth, linear, in, out, back, bounce, elastic, in_sine, out_sine, "
          "in_out_sine … in_out_bounce)");
    CHECK(error_of("part('a'):restore(0.1)") == "строка 2: restore(…) без save(): сначала part(\"a\"):save()");
    CHECK(error_of("part('a'):jump()") ==
          "строка 2: у части нет «jump» (свойства: x, y, rot, scale, sx, sy, skew_x, skew_y, opacity, fill, stroke, "
          "stroke_width, dash, draw_from, draw_to, visible, tint, flash, flash_color, blur, shadow, shadow_x, shadow_y, "
          "shadow_blur; методы: to, rest, save, restore, morph, spring, set, to_top, to_bottom, above, below, mask)");
    CHECK(error_of("wait(-1)") == "строка 2: wait(с): секунды — число не меньше 0");
    CHECK(error_of("\n\nlocal t = part('a').x(1, 0.1)\nall(t)\nall(t)") == "строка 6: all: этот переход уже отдан другой функции");
    PoseScript s = script_of(clip_of("wait(1)"));
    Recording rec;
    CHECK_FALSE(s.record(nullptr, "jump", {}, 1, {}, rec));
    CHECK(s.error() == "в коде нет клипа «jump»: clips = { jump = function(p) … end }");
    // The clip's functions in pose(t) say where they belong.
    PoseScript pose = script_of("return { pose = function(t)\n part('a').x(1, 0.1)\nend }");
    Pose p;
    CHECK_FALSE(pose.frame(Recording{}, nullptr, 0, {}, 1, p));
    CHECK(pose.error() == "строка 2: x — внутри функции клипа: clips = { walk = function(p) … end }");
}

TEST_CASE("Motion clip: a module's calls and the rig's parts") {
    PoseScript s;
    s.set_module_reader([](std::string_view name, std::string& text, std::string& why) {
        if (name != "moves") return why = "нет", false;
        text = "return {\n  shake = function(h, s) h.rot(5, s):to(-5, s):to(0, s) end,\n  bad = function(h) h.rot('x') end,\n}";
        return true;
    });
    REQUIRE(s.load(R"(local m = require("moves")
return { clips = {
  c = function(p) m.shake(part("head"), 0.1) part("head").x(1, 0.1) end,
  bad = function(p) m.bad(part("head")) end,
} })"));
    const Recording rec = recorded(s);
    REQUIRE(rec.files.size() == 2);
    CHECK(rec.files[1] == "moves");
    const Block& r = block(rec, "head", Prop::Rot, 1);
    CHECK(r.site.file == 1);
    const CodeSites* moves = s.module_code("moves");
    REQUIRE(moves);
    CHECK(moves->sites()[static_cast<usize>(r.site.site)].name == "to");
    CHECK(block(rec, "head", Prop::X).site.file == 0);
    CHECK(block(rec, "head", Prop::X).start == doctest::Approx(0.3));
    std::string out, why;
    CHECK_FALSE(edit_length(rec, r, 0.2, s.code(), "", out, why));
    CHECK(why == "блок из общего модуля «moves.luau»: правка изменит всех персонажей, правьте его в коде");
    Recording failed;
    CHECK_FALSE(s.record(nullptr, "bad", {}, 1, {}, failed));
    CHECK(s.error() == "moves.luau, строка 3: rot: нужно число, а не string");
    CHECK(s.error_line() == 0);

    // With the rig: its parts, its shapes for morph, fill as a colour.
    Rig rig;
    REQUIRE(rig.load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
  <g id="body"><rect id="torso" x="8" y="8" width="16" height="16" fill="#3060c0"/></g>
  <path id="mouth" d="M12 20 L20 20 L20 22 L12 22 Z" fill="#000000"/>
  <path id="mouth_open" d="M12 18 L20 18 L20 24 L12 24 Z" fill="#a00000" style="display:none"/>
</svg>)svg", why));
    PoseScript face = script_of(clip_of(R"(part("mouth"):morph("mouth_open", 0.1)
part("torso").fill("#ff0000", 0.2)
for _, h in parts("mouth*") do h.y(1, 0.1) end)"));
    const Recording f = recorded(face, {}, 1, &rig);
    CHECK(blocks_of(f, "mouth", Prop::Y) == 1);
    CHECK(blocks_of(f, "mouth_open", Prop::Y) == 1);
    Pose p;
    REQUIRE(face.pose(rig, 0.3, "c", {}, 1, p));
    CHECK(p.morphs["mouth"].target == "mouth_open");
    CHECK(p.morphs["mouth"].k == 1);
    CHECK(p.parts["torso"].has_fill);
    CHECK(p.parts["torso"].fill == Rgba8{255, 0, 0, 255});
    CHECK(p.parts["torso"].fill_k == 1);
    REQUIRE(face.pose(rig, 0.05, "c", {}, 1, p));
    CHECK(p.morphs["mouth"].k == doctest::Approx(0.5));
    PoseScript group = script_of(clip_of("part('body'):morph('mouth_open', 0.1)"));
    CHECK(record_error(group, {}, &rig) == "строка 2: «body» — группа, морфинг бывает только у фигуры");
    PoseScript none = script_of(clip_of("part('tail').x(1, 0.1)"));
    CHECK(record_error(none, {}, &rig) == "строка 2: в рисунке нет части «tail»");

    // pose() records with the lengths set for the clip, and records again when they change.
    PoseScript w = script_of(clip_of(R"(wait_until("отрыв", 0.1)
part("mouth").x(5, 0.1))"));
    REQUIRE(w.pose(rig, 0.2, "c", {}, 1, p));
    CHECK(p.parts["mouth"].x == 5);
    MotionLengths l;
    l.waits["отрыв"] = 0.5;
    w.set_lengths("c", l);
    REQUIRE(w.pose(rig, 0.2, "c", {}, 1, p));
    CHECK(p.parts["mouth"].x == 0);
}

namespace {

constexpr std::string_view kEdit = R"(return { params = { height = 6 }, clips = { c = function(p)
  local body = part("body")
  body:to({ y = 2, sy = 0.9 }, 0.1)
  body.y(-p.height, 0.25, "out_quad"):to(1, 0.05):back(0.1)
  loop(2, function(i) body.rot(28 * (i % 2 == 0 and 1 or -1), 0.25) end)
  delay(0.2, body.x(3, 0.1))
  wait(0.3)
end } }
)";

// kEdit with `from` replaced by `to` once.
std::string edited(std::string_view from, std::string_view to) {
    std::string s(kEdit);
    const usize at = s.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(s.find(from, at + 1) == std::string::npos);
    return s.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("Motion edit: a block's number written into its call, then recorded again") {
    PoseScript s = script_of(kEdit);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    const Block& y0 = block(rec, "body", Prop::Y);
    const Block& y1 = block(rec, "body", Prop::Y, 1);
    const Block& y2 = block(rec, "body", Prop::Y, 2);
    const Block& y3 = block(rec, "body", Prop::Y, 3);
    const Block& x = block(rec, "body", Prop::X);
    check_block(y1, 0.1, 0.25, -6);
    check_block(x, 1.2, 0.1, 3);

    // A length in a chain on one line: only its characters; what comes after moves when recorded again.
    REQUIRE(edit_length(rec, y2, 0.2, code, kEdit, out, why));
    CHECK(out == edited(":to(1, 0.05)", ":to(1, 0.2)"));
    PoseScript longer = script_of(out);
    const Recording l = recorded(longer);
    CHECK(block(l, "body", Prop::Y, 3).start == 0.55);
    CHECK(block(l, "body", Prop::X).start == 1.35);
    CHECK(l.length == 1.75);
    REQUIRE(edit_length(rec, y3, 0.2, code, kEdit, out, why));
    CHECK(out == edited(":back(0.1)", ":back(0.2)"));

    // An expression keeps itself and gets a correction; a second edit changes the correction.
    REQUIRE(edit_value(rec, y1, Prop::Y, {-8}, code, kEdit, out, why));
    CHECK(out == edited("body.y(-p.height, ", "body.y(-p.height - 2, "));
    PoseScript lower = script_of(out);
    const Recording lw = recorded(lower);
    CHECK(block(lw, "body", Prop::Y, 1).value.v == -8);
    std::string twice;
    REQUIRE(edit_value(lw, block(lw, "body", Prop::Y, 1), Prop::Y, {-9}, lower.code(), out, twice, why));
    CHECK(twice == edited("body.y(-p.height, ", "body.y(-p.height - 3, "));
    CHECK(recorded(lower).blocks.size() == rec.blocks.size());

    // A table: its field, or a new one.
    REQUIRE(edit_value(rec, y0, Prop::Y, {3}, code, kEdit, out, why));
    CHECK(out == edited("{ y = 2, sy", "{ y = 3, sy"));
    REQUIRE(edit_value(rec, y0, Prop::X, {3}, code, kEdit, out, why));
    CHECK(out == edited("sy = 0.9 }", "sy = 0.9, x = 3 }"));
    PoseScript wider = script_of(out);
    check_block(block(recorded(wider), "body", Prop::X), 0, 0.1, 3);
    REQUIRE(edit_value(rec, block(rec, "body", Prop::Sy), Prop::Sy, {0.8}, code, kEdit, out, why));
    CHECK(out == edited("sy = 0.9", "sy = 0.8"));
    // Another property's field of the same call: its own block says what the expression was worth.
    const std::string_view both = "return { params = { height = 6 }, clips = { c = function(p)\n"
                                  "  part(\"a\"):to({ y = 2, x = p.height }, 0.1)\nend } }";
    PoseScript bs = script_of(both);
    const Recording br = recorded(bs);
    REQUIRE(edit_value(br, block(br, "a", Prop::Y), Prop::X, {8}, bs.code(), both, out, why));
    CHECK(out.find("{ y = 2, x = p.height + 2 }") != std::string::npos);
    // A call of one property takes no other: that is a new line at(…).
    CHECK_FALSE(edit_value(rec, y2, Prop::X, {3}, code, kEdit, out, why));
    CHECK(why == "в вызове одного свойства (y) другое не дописать: оно ляжет новой строкой at(…)");
    CHECK(out.empty());
    CHECK_FALSE(edit_value(rec, y3, Prop::Y, {3}, code, kEdit, out, why));
    CHECK(why == "блок возвращает туда, откуда начался переход: его значения в коде нет");

    // A call in a loop: one number in the code for all its blocks.
    const Block& r0 = block(rec, "body", Prop::Rot);
    const Block& r1 = block(rec, "body", Prop::Rot, 1);
    CHECK(r0.site == r1.site);
    CHECK(r1.value.v == -28);
    REQUIRE(edit_value(rec, r1, Prop::Rot, {-30}, code, kEdit, out, why));
    CHECK(out == edited("-1), 0.25)", "-1) - 2, 0.25)"));
    PoseScript turned = script_of(out);
    const Recording t = recorded(turned);
    CHECK(block(t, "body", Prop::Rot).value.v == 26);
    CHECK(block(t, "body", Prop::Rot, 1).value.v == -30);
}

TEST_CASE("Motion edit: timing, start, a pause, a new line") {
    PoseScript s = script_of(kEdit);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    const Block& y1 = block(rec, "body", Prop::Y, 1);
    const Block& x = block(rec, "body", Prop::X);

    REQUIRE(edit_timing(rec, y1, Timing::InCubic, code, kEdit, out, why));
    CHECK(out == edited("\"out_quad\"", "\"in_cubic\""));
    REQUIRE(edit_timing(rec, x, Timing::OutBack, code, kEdit, out, why));
    CHECK(out == edited("body.x(3, 0.1)", "body.x(3, 0.1, \"out_back\")"));
    REQUIRE(edit_timing(rec, block(rec, "body", Prop::Y, 3), Timing::Linear, code, kEdit, out, why));
    CHECK(out == edited(":back(0.1)", ":back(0.1, \"linear\")"));

    // The start of a block of delay(…) is delay's number; of a block that follows the one before, none.
    REQUIRE(edit_start(rec, x, 1.3, code, kEdit, out, why));
    CHECK(out == edited("delay(0.2,", "delay(0.3,"));
    PoseScript later = script_of(out);
    const Recording l = recorded(later);
    CHECK(block(l, "body", Prop::X).start == 1.3);
    CHECK(l.length == 1.7);
    CHECK_FALSE(edit_start(rec, x, 0.9, code, kEdit, out, why));
    CHECK(why == "delay(с, …): секунд не меньше 0");
    CHECK_FALSE(edit_start(rec, y1, 0.5, code, kEdit, out, why));
    CHECK(why == "блок идёт за предыдущим, своего начала в коде у него нет: оно есть у at(с, …) и delay(с, …)");

    REQUIRE(rec.pauses.size() == 1);
    REQUIRE(edit_pause(rec, rec.pauses[0], 0.5, code, kEdit, out, why));
    CHECK(out == edited("wait(0.3)", "wait(0.5)"));

    // A part moved where nothing of it runs: a new first line of the clip, recorded at its time.
    REQUIRE(add_at_line(rec, 0.25, "head", Prop::Rot, {5}, 0.1, code, kEdit, out, why));
    CHECK(out == edited("  local body", "  at(0.25, part(\"head\").rot(5, 0.1))\n  local body"));
    PoseScript more = script_of(out);
    const Recording m = recorded(more);
    const Block& head = block(m, "head", Prop::Rot);
    check_block(head, 0.25, 0.1, 5);
    CHECK(more.code().sites()[static_cast<usize>(head.start_site.site)].line == 2);
    CHECK(m.length == rec.length);
    PropValue open;
    open.v = 1;
    open.target = "mouth_open";
    REQUIRE(add_at_line(rec, 0, "mouth", Prop::Morph, open, 0.1, code, kEdit, out, why));
    CHECK(out.find("  at(0, part(\"mouth\"):morph(\"mouth_open\", 0.1))\n  local body") != std::string::npos);
    PropValue red;
    red.v = 1;
    red.color = {255, 0, 0, 255};
    REQUIRE(add_at_line(rec, 0.5, "body", Prop::Fill, red, 0.2, code, kEdit, out, why));
    CHECK(out.find("  at(0.5, part(\"body\").fill(\"#ff0000\", 0.2))\n") != std::string::npos);
}

TEST_CASE("Motion edit: the block that decides a frame, whether its transition goes, a part's latest") {
    PoseScript s = script_of(clip_of(R"(local body = part("body")
body.y(2, 0.2)
wait(0.1)
body:to({ x = 3 }, 0.2)
at(0.4, part("head").rot(5, 0))
part("a"):to({ y = 2, x = 1 }, 0.1))"));
    const Recording rec = recorded(s);
    const u32 body = static_cast<u32>(std::find(rec.parts.begin(), rec.parts.end(), "body") - rec.parts.begin());
    const u32 head = static_cast<u32>(std::find(rec.parts.begin(), rec.parts.end(), "head") - rec.parts.begin());
    const u32 a = static_cast<u32>(std::find(rec.parts.begin(), rec.parts.end(), "a") - rec.parts.begin());
    const u32 ty = static_cast<u32>(rec.track(body, Prop::Y)), tx = static_cast<u32>(rec.track(body, Prop::X));
    const u32 th = static_cast<u32>(rec.track(head, Prop::Rot));
    auto index = [&](const Block& b) { return static_cast<i32>(&b - rec.blocks.data()); };
    const Block& y = block(rec, "body", Prop::Y);
    const Block& x = block(rec, "body", Prop::X);
    const Block& h = block(rec, "head", Prop::Rot);
    // The block that started last at t or before decides, as a frame reads it.
    CHECK(deciding_block(rec, ty, 0) == index(y));
    CHECK(deciding_block(rec, ty, 0.25) == index(y));
    CHECK(deciding_block(rec, tx, 0.29) == -1);
    CHECK(deciding_block(rec, tx, 0.3) == index(x));
    CHECK(deciding_block(rec, 99, 0.3) == -1);
    // Its transition goes after its start up to its end; one of length 0 in the frame it starts in.
    CHECK_FALSE(block_going(y, 0, 0.1));
    CHECK(block_going(y, 0.1, 0.1));
    CHECK(block_going(y, 0.2, 0.1));
    CHECK_FALSE(block_going(y, 0.25, 0.1));
    CHECK_FALSE(block_going(x, 0.3, 0.1));
    CHECK(block_going(x, 0.5, 0.1));
    CHECK(deciding_block(rec, th, 0.4) == index(h));
    CHECK(block_going(h, 0.4, 0.1));
    CHECK(block_going(h, 0.45, 0.1));
    CHECK_FALSE(block_going(h, 0.5, 0.1));
    CHECK_FALSE(block_going(h, 0.39, 0.1));
    // Of a part's tracks, the one that started last; started together, the first property.
    CHECK(latest_block(rec, body, 0.1) == index(y));
    CHECK(latest_block(rec, body, 0.35) == index(x));
    CHECK(latest_block(rec, head, 0.3) == -1);
    CHECK(latest_block(rec, a, 0.6) == index(block(rec, "a", Prop::X)));
}

namespace {

constexpr std::string_view kMove = R"(return { params = { height = 6 }, clips = { c = function(p)
  local body = part("body")
  at(0, body.x(1, 0.2))
  body.y(-p.height, 0.2)
  part("torso"):to({ x = 2 }, 0.2)
  wait(0.2)
end } }
)";

std::string moved(std::string_view from, std::string_view to) {
    std::string s(kMove);
    const usize at = s.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(s.find(from, at + 1) == std::string::npos);
    return s.replace(at, from.size(), to);
}

f64 value_at(const Recording& rec, std::string_view part, Prop p, f64 t) {
    const u32 pi = static_cast<u32>(std::find(rec.parts.begin(), rec.parts.end(), part) - rec.parts.begin());
    const i32 tr = pi < rec.parts.size() ? rec.track(pi, p) : -1;
    PropValue v = rest_value(p);
    std::string why;
    if (tr >= 0) REQUIRE(sample(rec, static_cast<u32>(tr), t, nullptr, v, why));
    return v.v;
}

} // namespace

TEST_CASE("Motion edit: a part dragged in a frame, written into its code") {
    PoseScript s = script_of(kMove);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string out, why;
    CHECK(rec.length == 0.6);

    // Both transitions go: each end moves by as much, two calls of one text (an expression keeps itself).
    REQUIRE(edit_part_move(rec, "body", 0.2, 0.1, 0.5, -1, code, kMove, out, why));
    CHECK(out == moved("body.x(1, 0.2))\n  body.y(-p.height, 0.2)", "body.x(1.5, 0.2))\n  body.y(-p.height - 1, 0.2)"));
    PoseScript both = script_of(out);
    const Recording b = recorded(both);
    CHECK(value_at(b, "body", Prop::X, 0.2) == 1.5);
    CHECK(value_at(b, "body", Prop::Y, 0.2) == -7);
    CHECK(b.blocks.size() == rec.blocks.size());
    // Only the one that moved.
    REQUIRE(edit_part_move(rec, "body", 0.1, 0.1, 0, 2, code, kMove, out, why));
    CHECK(out == moved("body.y(-p.height, 0.2)", "body.y(-p.height + 2, 0.2)"));

    // No transition of it goes: a first line at the frame, from where the recording has it, length 0.
    REQUIRE(edit_part_move(rec, "body", 0.3, 0.1, 1, 0, code, kMove, out, why));
    CHECK(out == moved("  local body", "  at(0.3, part(\"body\"):to({ x = 2 }, 0))\n  local body"));
    PoseScript put = script_of(out);
    const Recording pr = recorded(put);
    CHECK(value_at(pr, "body", Prop::X, 0.3) == 2);
    CHECK(value_at(pr, "body", Prop::X, 0.25) == 1);
    CHECK(pr.length == rec.length);

    // x goes in a table without y: y is added to it, its value where the recording has it plus the move.
    REQUIRE(edit_part_move(rec, "torso", 0.4, 0.1, 0.5, -2, code, kMove, out, why));
    CHECK(out == moved("{ x = 2 }", "{ x = 2.5, y = -2 }"));
    PoseScript field = script_of(out);
    const Recording fr = recorded(field);
    check_block(block(fr, "torso", Prop::Y), 0.2, 0.2, -2);
    CHECK(value_at(fr, "torso", Prop::X, 0.4) == 2.5);

    // A part without a block: both in one line; dragged again in that frame, the line's own numbers.
    REQUIRE(edit_part_move(rec, "head", 0.5, 0.1, 1, 2, code, kMove, out, why));
    const std::string head = moved("  local body", "  at(0.5, part(\"head\"):to({ x = 1, y = 2 }, 0))\n  local body");
    CHECK(out == head);
    PoseScript placed = script_of(head);
    const Recording hr = recorded(placed);
    CHECK(value_at(hr, "head", Prop::Y, 0.5) == 2);
    REQUIRE(edit_part_move(hr, "head", 0.5, 0.1, 0.5, 0, placed.code(), head, out, why));
    CHECK(out == moved("  local body", "  at(0.5, part(\"head\"):to({ x = 1.5, y = 2 }, 0))\n  local body"));
    REQUIRE(edit_part_move(hr, "head", 0.55, 0.1, 0, -1, placed.code(), head, out, why));
    CHECK(out == moved("  local body", "  at(0.5, part(\"head\"):to({ x = 1, y = 1 }, 0))\n  local body"));

    // A frame off the thousandths (12 a second): the line starts before the frame, so the frame shows it.
    REQUIRE(edit_part_move(rec, "head", 5.0 / 12, 1.0 / 12, 1, 0, code, kMove, out, why));
    CHECK(out == moved("  local body", "  at(0.416, part(\"head\"):to({ x = 1 }, 0))\n  local body"));
    PoseScript twelfth = script_of(out);
    const Recording tr = recorded(twelfth);
    CHECK(value_at(tr, "head", Prop::X, 5.0 / 12) == 1);
    CHECK(value_at(tr, "head", Prop::X, 4.0 / 12) == 0);

    CHECK_FALSE(edit_part_move(rec, "head", 0.5, 0.1, 0, 0, code, kMove, out, why));
    CHECK(why == "часть не сдвинута");
    CHECK(out.empty());
}

TEST_CASE("Motion edit: a part dragged where its block takes no number, or its track started since") {
    // A binding and a back decide: their words; a track that started since the other's call: a line, not a field.
    const std::string src = clip_of(R"(local a = part("a")
part("t"):to({ x = 2 }, 0.4)
at(0.1, part("t").y(1, 0))
a.x(function(t) return t end)
a.y(2, 0.1):back(0.2))");
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    std::string out, why;
    CHECK_FALSE(edit_part_move(rec, "a", 0.6, 0.1, 1, 0, s.code(), src, out, why));
    CHECK(why == "значение блока считает функция связи: правьте её в коде");
    CHECK_FALSE(edit_part_move(rec, "a", 0.6, 0.1, 0, 1, s.code(), src, out, why));
    CHECK(why == "блок возвращает туда, откуда начался переход: его значения в коде нет");
    CHECK(out.empty());
    REQUIRE(edit_part_move(rec, "t", 0.3, 0.1, 0, 1, s.code(), src, out, why));
    CHECK(out == "return { clips = { c = function(p, rng)\nat(0.3, part(\"t\"):to({ y = 2 }, 0))\n" + src.substr(src.find("local a")));

    // A module's call is not edited.
    PoseScript m;
    m.set_module_reader([](std::string_view name, std::string& text, std::string& w) {
        if (name != "moves") return w = "нет", false;
        text = "return { lift = function(h) h:to({ y = -3 }, 0.4) end }";
        return true;
    });
    const std::string ms = "local mv = require(\"moves\")\n" + clip_of("mv.lift(part(\"a\"))");
    REQUIRE(m.load(ms));
    const Recording mr = recorded(m);
    CHECK_FALSE(edit_part_move(mr, "a", 0.2, 0.1, 0, 1, m.code(), ms, out, why));
    CHECK(why == "блок из общего модуля «moves.luau»: правка изменит всех персонажей, правьте его в коде");
    // x does not go and y's call is the module's: x has a line of its own.
    REQUIRE(edit_part_move(mr, "a", 0.2, 0.1, 1, 0, m.code(), ms, out, why));
    CHECK(out.find("at(0.2, part(\"a\"):to({ x = 1 }, 0))") != std::string::npos);
}

TEST_CASE("Motion edit: what of a block can be edited, and why not") {
    PoseScript s = script_of(kEdit);
    const Recording rec = recorded(s);
    const CodeSites& code = s.code();
    std::string why;
    const Block& y0 = block(rec, "body", Prop::Y);
    const Block& y3 = block(rec, "body", Prop::Y, 3);
    const Block& x = block(rec, "body", Prop::X);
    CHECK(can_edit(rec, y0, BlockEdit::Value, code, why));
    CHECK(why.empty());
    CHECK(can_edit(rec, y0, BlockEdit::Length, code, why));
    CHECK(can_edit(rec, y0, BlockEdit::Timing, code, why));
    CHECK_FALSE(can_edit(rec, y0, BlockEdit::Start, code, why));
    CHECK(why == "блок идёт за предыдущим, своего начала в коде у него нет: оно есть у at(с, …) и delay(с, …)");
    CHECK_FALSE(can_edit(rec, y3, BlockEdit::Value, code, why));
    CHECK(why == "блок возвращает туда, откуда начался переход: его значения в коде нет");
    CHECK(can_edit(rec, y3, BlockEdit::Length, code, why));
    CHECK(can_edit(rec, x, BlockEdit::Start, code, why));
    REQUIRE(rec.pauses.size() == 1);
    CHECK(can_edit_pause(rec, rec.pauses[0], code, why));

    const std::string src = clip_of(R"(local a = part("a")
a.x:spring(10)
a.y(function(t) return t end)
a.rot(4, 0.1))");
    PoseScript sp = script_of(src);
    const Recording sr = recorded(sp);
    const Block& spring = block(sr, "a", Prop::X);
    const Block& bound = block(sr, "a", Prop::Y);
    const Block& rot = block(sr, "a", Prop::Rot);
    CHECK(can_edit(sr, spring, BlockEdit::Value, sp.code(), why));
    CHECK_FALSE(can_edit(sr, spring, BlockEdit::Length, sp.code(), why));
    CHECK(why == "длину пружины задают её масса, жёсткость и затухание");
    CHECK_FALSE(can_edit(sr, spring, BlockEdit::Timing, sp.code(), why));
    CHECK(why == "у пружины нет сглаживания: её ход задают масса, жёсткость и затухание");
    CHECK_FALSE(can_edit(sr, bound, BlockEdit::Value, sp.code(), why));
    CHECK(why == "значение блока считает функция связи: правьте её в коде");
    CHECK_FALSE(can_edit(sr, bound, BlockEdit::Length, sp.code(), why));
    CHECK(why == "у связи нет длины: она держится до следующего блока");
    // A timing the call has not: added after its length.
    CHECK(can_edit(sr, rot, BlockEdit::Timing, sp.code(), why));
    std::string out;
    REQUIRE(edit_timing(sr, rot, Timing::OutQuad, sp.code(), src, out, why));
    CHECK(out.find("a.rot(4, 0.1, \"out_quad\")") != std::string::npos);
}

TEST_CASE("Motion edit: a line put into a file of CRLF lines ends as they do") {
    const std::string src = "return { clips = { c = function(p)\r\n  part(\"a\").x(1, 0.1)\r\nend } }\r\n";
    PoseScript s = script_of(src);
    const Recording rec = recorded(s);
    std::string out, why;
    REQUIRE(add_at_table(rec, 0.1, "b", {{Prop::X, 1}, {Prop::Y, -2}}, 0, s.code(), src, out, why));
    CHECK(out == "return { clips = { c = function(p)\r\n  at(0.1, part(\"b\"):to({ x = 1, y = -2 }, 0))\r\n  part(\"a\").x(1, 0.1)\r\nend } }\r\n");
    CHECK_FALSE(add_at_table(rec, 0.1, "b", {}, 0, s.code(), src, out, why));
    CHECK(why == "нечего ставить");
    CHECK_FALSE(add_at_table(rec, 0.1, "b", {{Prop::Fill, 1}}, 0, s.code(), src, out, why));
}
