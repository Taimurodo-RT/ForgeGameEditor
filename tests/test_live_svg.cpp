// Live SVG pictures (step 4 of docs/models/вектор-модель.md): a script changes the parts of the picture one element
// shows, by their id, and that element alone draws it again. RmlUi runs here with a render interface on the CPU that
// keeps the textures it is given, so what the SVG plugin draws for each element is compared with lunasvg drawing the
// file changed by hand: moved, turned, coloured, faded, morphed. Under transforms each kind of SVG picture is rasterized
// at their scale: as it is when it holds, by steps while it changes.

#include "forge/core/types.h"
#include "forge/ui/live_svg.h"
#include "forge/vector/morph.h"
#include "forge/vector/path.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/PictureScale.h>
#include <doctest/doctest.h>
#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace {

using forge::u8;
using forge::usize;

const char* const kFace = R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
  <circle id="head" cx="32" cy="32" r="28" fill="#f0c040"/>
  <g id="eyes" fill="#202020"><circle cx="22" cy="24" r="4"/><circle cx="42" cy="24" r="4" fill="#303030"/></g>
  <path id="mouth" d="M20 44 L44 44 L44 48 L20 48 Z" fill="#a02020"/>
  <path id="smile" d="M18 40 C24 54 40 54 46 40 Z" fill="#ff4040" display="none"/>
  <rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0"/>
</svg>
)";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const usize at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

// The plugin's pixels for an SVG text at a size: lunasvg's, red and blue swapped into RmlUi's order.
std::vector<u8> drawn(const std::string& text, int size = 64) {
    auto doc = lunasvg::Document::loadFromData(text);
    REQUIRE(doc);
    lunasvg::Bitmap bitmap = doc->renderToBitmap(size, size);
    REQUIRE(bitmap.valid());
    std::vector<u8> out(bitmap.data(), bitmap.data() + static_cast<usize>(size) * static_cast<usize>(size) * 4);
    for (usize i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
    return out;
}

// Pixels whose channels differ by more than `slack`.
int differ(const std::vector<u8>& a, const std::vector<u8>& b, int slack = 0) {
    REQUIRE(a.size() == b.size());
    int n = 0;
    for (usize i = 0; i < a.size(); i += 4)
        for (usize c = 0; c < 4; ++c)
            if (std::abs(int(a[i + c]) - int(b[i + c])) > slack) {
                ++n;
                break;
            }
    return n;
}

// Keeps the textures it is given and which one each draw used where.
struct CpuRender final : Rml::RenderInterface {
    std::map<Rml::TextureHandle, std::vector<u8>> textures;
    std::map<Rml::TextureHandle, Rml::Vector2i> sizes;
    Rml::TextureHandle next = 1;
    int generated = 0;
    std::map<int, Rml::TextureHandle> at_y; // a draw's texture by the y it was drawn at
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        if (texture) at_y[int(translation.y)] = texture;
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    // An <img> asks for its file's size this way first (Forge's renderer reads an SVG's own size).
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String&) override {
        size = {64, 64};
        textures[next] = {};
        return next++;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i size) override {
        ++generated;
        textures[next] = std::vector<u8>(source.begin(), source.end());
        sizes[next] = size;
        return next++;
    }
    void ReleaseTexture(Rml::TextureHandle texture) override { textures.erase(texture); }
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

// The file, from memory, whatever folder it is asked from.
struct MemoryFiles final : Rml::FileInterface {
    std::string svg = kFace;
    struct Stream {
        std::string data;
        usize at = 0;
    };
    std::vector<Stream*> open;
    Rml::FileHandle Open(const Rml::String& path) override {
        if (path.size() < 8 || path.compare(path.size() - 8, 8, "face.svg") != 0) return 0;
        Stream* f = new Stream{svg, 0};
        open.push_back(f);
        return reinterpret_cast<Rml::FileHandle>(f);
    }
    void Close(Rml::FileHandle file) override {
        Stream* f = reinterpret_cast<Stream*>(file);
        open.erase(std::find(open.begin(), open.end(), f));
        delete f;
    }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        Stream* f = reinterpret_cast<Stream*>(file);
        const usize n = std::min(size, f->data.size() - f->at);
        std::memcpy(buffer, f->data.data() + f->at, n);
        f->at += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        Stream* f = reinterpret_cast<Stream*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->at) : long(f->data.size());
        if (base + offset < 0 || base + offset > long(f->data.size())) return false;
        f->at = usize(base + offset);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<Stream*>(file)->at; }
};

} // namespace

TEST_CASE("Live SVG: a script's parts drawn on one element, the other keeps the file") {
    CpuRender render;
    MemoryFiles files;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&files);
    REQUIRE(Rml::Initialise());
    forge::ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("live", {200, 200});
    REQUIRE(context);
    // Two elements showing one file; made again as a game makes a page again.
    Rml::ElementDocument* doc = nullptr;
    Rml::Element *a = nullptr, *b = nullptr;
    auto load = [&] {
        doc = context->LoadDocumentFromMemory(R"(<rml><head><style>
        body { width: 200px; height: 200px; margin: 0; }
        img { display: block; width: 64px; height: 64px; }
        </style></head><body><img id="a" src="face.svg"/><img id="b" src="face.svg"/></body></rml>)",
                                              "mem/page.rml");
        REQUIRE(doc);
        doc->Show();
        a = doc->GetElementById("a");
        b = doc->GetElementById("b");
        REQUIRE(a);
        REQUIRE(b);
    };
    load();
    auto frame = [&] {
        render.at_y.clear();
        context->Update();
        context->Render();
    };
    auto made = [&] { // textures of pictures alive now (not an <img>'s stand-in for its size)
        int n = 0;
        for (const auto& [handle, pixels] : render.textures) n += pixels.empty() ? 0 : 1;
        return n;
    };
    auto pixels = [&](int y) -> const std::vector<u8>& {
        REQUIRE(render.at_y.count(y));
        REQUIRE(render.textures.count(render.at_y[y]));
        return render.textures[render.at_y[y]];
    };
    const std::vector<u8> file = drawn(kFace);

    frame();
    CHECK(render.at_y[0] == render.at_y[64]); // one texture for both: the same file at the same size
    CHECK(differ(pixels(0), file) == 0);

    SUBCASE("morph: k = 1 draws the other shape where the part was, k = 0 the part; the other element keeps the file") {
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 1));
        frame();
        CHECK(render.at_y[0] != render.at_y[64]);
        const std::string smile =
            replaced(kFace, R"(d="M20 44 L44 44 L44 48 L20 48 Z" fill="#a02020")", R"(d="M18 40 C24 54 40 54 46 40 Z" fill="#ff4040")");
        const int off = differ(pixels(0), drawn(smile), 8);
        INFO("pixels off by more than 8 at k = 1: " << off);
        CHECK(off <= 4);
        CHECK(differ(pixels(64), file) == 0);
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 0));
        const int generated = render.generated;
        frame();
        CHECK(render.generated == generated + 1); // one texture drawn again: a's
        CHECK(differ(pixels(0), file, 8) <= 4);
        CHECK(differ(pixels(64), file) == 0);
        // Halfway: neither, and not what lunasvg draws for either end.
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 0.5));
        frame();
        CHECK(differ(pixels(0), file, 8) > 20);
        CHECK(differ(pixels(0), drawn(smile), 8) > 20);
    }
    SUBCASE("move and turn: against the file, round the part's own centre") {
        CHECK(forge::ui::svg_move(a, "box", 10, 5));
        CHECK(forge::ui::svg_move(a, "box", 10, 5)); // twice: still 10, 5 from the file
        frame();
        CHECK(differ(pixels(0), drawn(replaced(kFace, R"(<rect id="box")", R"x(<rect transform="translate(10 5)" id="box")x"))) == 0);
        CHECK(forge::ui::svg_turn(a, "box", 90));
        frame();
        CHECK(differ(pixels(0), drawn(replaced(kFace, R"(<rect id="box")", R"x(<rect transform="translate(10 5) rotate(90 9 9)" id="box")x"))) == 0);
        CHECK(differ(pixels(64), file) == 0);
    }
    SUBCASE("colour: the part and the shapes in it with a fill of their own; fade") {
        CHECK(forge::ui::svg_color(a, "eyes", "#00ff00"));
        CHECK(forge::ui::svg_fade(a, "head", 0.5));
        frame();
        std::string want = replaced(kFace, R"(<g id="eyes" fill="#202020">)", R"(<g id="eyes" fill="#00ff00">)");
        want = replaced(want, R"(r="4" fill="#303030")", R"(r="4" fill="#00ff00")");
        want = replaced(want, R"(fill="#f0c040"/>)", R"(fill="#f0c040" opacity="0.5"/>)");
        CHECK(differ(pixels(0), drawn(want)) == 0);
        std::string why;
        CHECK_FALSE(forge::ui::svg_color(a, "eyes", "зелёный", &why));
        CHECK(why.find("не цвет") != std::string::npos);
    }
    SUBCASE("a clip's pose over what was said, then taken off: what was said draws again, the clip's goes") {
        using forge::vector::MotionProp;
        CHECK(forge::ui::svg_move(a, "box", 10, 5));
        forge::vector::MotionPartPose head;
        head.id = "head";
        for (usize p = 0; p < forge::vector::kMotionPropCount; ++p) head.values[p] = forge::vector::motion_rest(static_cast<MotionProp>(p));
        head.has[static_cast<usize>(MotionProp::Opacity)] = true;
        head.values[static_cast<usize>(MotionProp::Opacity)].v = 0.5;
        forge::ui::svg_pose(a, {"face.svg", {head}});
        frame();
        const std::string moved = replaced(kFace, R"(<rect id="box")", R"x(<rect transform="translate(10 5)" id="box")x");
        CHECK(differ(pixels(0), drawn(replaced(moved, R"(fill="#f0c040"/>)", R"(fill="#f0c040" opacity="0.5"/>)"))) == 0);
        REQUIRE(forge::ui::svg_posed(a));
        CHECK(forge::ui::svg_posed(a)->parts.size() == 1);
        CHECK_FALSE(forge::ui::svg_posed(b));
        forge::ui::svg_unpose(a);
        CHECK_FALSE(forge::ui::svg_posed(a));
        frame();
        CHECK(differ(pixels(0), drawn(moved)) == 0);
        CHECK(differ(pixels(64), file) == 0);
    }
    SUBCASE("a clip's frames do not build on each other: a morph and a colour back at 0, a part the next frame lacks, are the file's") {
        using forge::vector::MotionProp;
        using forge::vector::MotionValue;
        auto part = [](const char* id, MotionProp p, MotionValue v) {
            forge::vector::MotionPartPose pp;
            pp.id = id;
            for (usize i = 0; i < forge::vector::kMotionPropCount; ++i) pp.values[i] = forge::vector::motion_rest(static_cast<MotionProp>(i));
            pp.has[static_cast<usize>(p)] = true;
            pp.values[static_cast<usize>(p)] = v;
            return pp;
        };
        forge::ui::svg_pose(a, {"face.svg", {part("mouth", MotionProp::Morph, {1, {}, "smile", 0}), part("head", MotionProp::Fill, {1, {0, 0, 255, 255}, {}, 0}),
                                             part("box", MotionProp::Opacity, {0.3, {}, {}, 0})}});
        frame();
        CHECK(differ(pixels(0), file, 8) > 20);
        forge::ui::svg_pose(a, {"face.svg", {part("mouth", MotionProp::Morph, {0, {}, "", 0}), part("head", MotionProp::Fill, {0, {0, 0, 255, 255}, {}, 0})}});
        frame();
        CHECK(differ(pixels(0), file) == 0);
        CHECK(differ(pixels(64), file) == 0);
    }
    SUBCASE("a clip's copy (part 5г): drawn on a stage from its birth, not before it once more, nor once the clip is off or is another picture's") {
        using forge::vector::MotionProp;
        // The box half seen through, so a copy left on its source (as it is before its birth) shows.
        const std::string solid = R"(<rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0"/>)";
        const std::string box = R"(<rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0" fill-opacity="0.5"/>)";
        const std::string half = replaced(kFace, solid, box);
        files.svg = half;
        doc->Close();
        frame();
        load();
        frame();
        const std::vector<u8> plain = drawn(half);
        CHECK(differ(pixels(0), plain) == 0);
        // The box's copy 20 to the right; the box to the top all along, so a stage is there before the copy is born.
        forge::vector::MotionPartPose copy;
        copy.id = "box[1]";
        for (usize p = 0; p < forge::vector::kMotionPropCount; ++p) copy.values[p] = forge::vector::motion_rest(static_cast<MotionProp>(p));
        copy.has[static_cast<usize>(MotionProp::X)] = true;
        copy.values[static_cast<usize>(MotionProp::X)].v = 20;
        copy.moves = true;
        copy.local = {1, 0, 0, 1, 20, 0};
        const std::vector<forge::vector::MotionOrderStep> top{{0, "box", forge::vector::MotionOrder::Top, ""}};
        const forge::ui::SvgPose before{"face.svg", {}, {}, top}, after{"face.svg", {copy}, {{"box[1]", "box", 1, 0.5}}, top};
        const std::vector<u8> with_copy =
            drawn(replaced(half, box, box + R"x(<rect transform="translate(20 0)" id="box" x="4" y="4" width="10" height="10" fill="#2040a0" fill-opacity="0.5"/>)x"));
        forge::ui::svg_pose(a, before);
        frame();
        CHECK(differ(pixels(0), plain) == 0);
        forge::ui::svg_pose(a, after); // the stage made again: it had no copy
        frame();
        CHECK(differ(pixels(0), with_copy) == 0);
        forge::ui::svg_pose(a, before); // round again, before the birth
        frame();
        CHECK(differ(pixels(0), plain) == 0);
        forge::ui::svg_pose(a, after);
        frame();
        CHECK(differ(pixels(0), with_copy) == 0);
        forge::ui::svg_pose(a, {"other.svg", {}}); // a pose of another picture: this one is its file again
        frame();
        CHECK(differ(pixels(0), plain) == 0);
        forge::ui::svg_pose(a, after);
        frame();
        CHECK(differ(pixels(0), with_copy) == 0);
        // The element shows another file and this one again: the plugin reads this one anew (its document may well
        // stand where the stage's did), and the stage is made again on it.
        a->SetAttribute("src", "back_face.svg");
        frame();
        CHECK(differ(pixels(0), plain) == 0);
        a->SetAttribute("src", "face.svg");
        frame();
        CHECK(differ(pixels(0), with_copy) == 0);
        forge::ui::svg_unpose(a); // the file again, not the stage
        frame();
        CHECK(differ(pixels(0), plain) == 0);
        CHECK(differ(pixels(64), plain) == 0);
        CHECK(forge::ui::svg_problems().empty());
    }
    SUBCASE("another drawing (part 5д): shown in place of the file, a swap of two documents in one picture, the main one again") {
        // «angry»: the head red, a brow over the eyes, no box.
        const std::string angry = replaced(replaced(kFace, R"(fill="#f0c040")", R"(fill="#e04020")"),
                                           R"(<rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0"/>)",
                                           R"(<path id="brow" d="M14 14 L50 14 L50 18 L14 18 Z" fill="#401010"/>)");
        forge::ui::SvgPose shown{"face.svg", {}, {}, {}};
        shown.drawings.push_back({"angry", "face.angry.svg", std::make_shared<const std::string>(angry)});
        shown.drawing = "angry";
        forge::ui::SvgPose swap = shown, main = shown;
        swap.swap = true;
        swap.swap_from = "draw";
        swap.swap_k = 0.5;
        main.drawing = "draw";
        // A swap's picture: the old drawing with its pairs morphing, the new one over it (render_posed_svg), premultiplied
        // as the plugin keeps it.
        std::vector<u8> mid;
        std::string why;
        REQUIRE(forge::ui::render_posed_svg(kFace, swap, 64, 64, mid, why));
        for (usize i = 0; i < mid.size(); i += 4)
            for (usize c = 0; c < 3; ++c) mid[i + c] = u8((mid[i + c] * mid[i + 3] + 127) / 255);
        const std::vector<u8> to = drawn(angry);
        CHECK(differ(mid, file, 8) > 100);
        CHECK(differ(mid, to, 8) > 100);

        forge::ui::svg_pose(a, shown);
        frame();
        CHECK(differ(pixels(0), to) == 0);
        CHECK(differ(pixels(64), file) == 0);
        forge::ui::svg_pose(a, swap);
        frame();
        CHECK(differ(pixels(0), mid, 2) == 0);
        forge::ui::svg_pose(a, main);
        frame();
        CHECK(differ(pixels(0), file) == 0);
        forge::ui::svg_pose(a, swap);
        frame();
        CHECK(differ(pixels(0), mid, 2) == 0);
        forge::ui::svg_pose(a, shown); // the swap over: one document again
        frame();
        CHECK(differ(pixels(0), to) == 0);
        forge::ui::svg_pose(a, swap);
        frame();
        forge::ui::svg_pose(a, {"other.svg", {}}); // a pose of another picture: this one is its file again
        frame();
        CHECK(differ(pixels(0), file) == 0);
        forge::ui::svg_pose(a, swap);
        frame();
        CHECK(differ(pixels(0), mid, 2) == 0);
        // The element shows another file and this one again: the plugin reads it anew, nothing over it, and the swap is
        // made again on it.
        a->SetAttribute("src", "back_face.svg");
        frame();
        CHECK(differ(pixels(0), file) == 0);
        a->SetAttribute("src", "face.svg");
        frame();
        CHECK(differ(pixels(0), mid, 2) == 0);
        forge::ui::svg_unpose(a); // the file again, one document
        frame();
        CHECK(differ(pixels(0), file) == 0);
        CHECK(differ(pixels(64), file) == 0);
        CHECK(forge::ui::svg_problems().empty());
    }
    SUBCASE("another drawing (part 5д): what a script said acts on each drawing that has the part; a part none has is said") {
        // «angry»: the brow only it has; the box only the main drawing has; a tail neither has.
        const std::string angry = replaced(replaced(kFace, R"(fill="#f0c040")", R"(fill="#e04020")"),
                                           R"(<rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0"/>)",
                                           R"(<path id="brow" d="M14 14 L50 14 L50 18 L14 18 Z" fill="#401010"/>)");
        const std::string moved_brow = replaced(angry, R"(<path id="brow" d=)", R"x(<path id="brow" transform="translate(10 0)" d=)x");
        const std::string moved_box = replaced(kFace, R"(<rect id="box" x=)", R"x(<rect id="box" transform="translate(3 0)" x=)x");
        forge::ui::SvgPose shown{"face.svg", {}, {}, {}};
        shown.drawings.push_back({"angry", "face.angry.svg", std::make_shared<const std::string>(angry)});
        shown.drawing = "angry";
        forge::ui::svg_pose(a, shown);
        CHECK(forge::ui::svg_move(a, "box", 3, 0));
        CHECK(forge::ui::svg_move(a, "brow", 10, 0));
        CHECK(forge::ui::svg_move(a, "хвост", 1, 1));
        frame();
        CHECK(differ(pixels(0), drawn(moved_brow)) == 0);
        CHECK(forge::ui::svg_problems() == std::vector<std::string>{"«face.angry.svg»: нет части с id «хвост»"});
        // In a swap from the main drawing: the box moved in the old one, the brow in the new one over it.
        forge::ui::SvgPose swap = shown;
        swap.swap = true;
        swap.swap_from = "draw";
        swap.swap_k = 0.5;
        forge::ui::svg_pose(a, swap);
        frame();
        forge::ui::SvgPose want = swap;
        want.drawings[0].text = std::make_shared<const std::string>(moved_brow);
        std::vector<u8> mid;
        std::string why;
        REQUIRE(forge::ui::render_posed_svg(moved_box, want, 64, 64, mid, why));
        for (usize i = 0; i < mid.size(); i += 4)
            for (usize c = 0; c < 3; ++c) mid[i + c] = u8((mid[i + c] * mid[i + 3] + 127) / 255);
        CHECK(differ(pixels(0), mid, 2) == 0);
        CHECK(forge::ui::svg_problems() == std::vector<std::string>{"«face.svg»: нет части с id «хвост»"});
    }
    SUBCASE("another drawing (part 5д) twice the size, in the element's colour, cropped to its content: the new one over the old at the picture's scale") {
        // Both drawings in currentColor in part, on a ground the picture's size (cropped to its content, the picture keeps
        // its scale).
        const std::string framed = replaced(replaced(kFace, R"(<circle id="head")", "<rect id=\"ground\" x=\"0\" y=\"0\" width=\"64\" height=\"64\" fill=\"#e0e0e0\"/>\n  <circle id=\"head\""),
                                            R"(<g id="eyes" fill="#202020">)", R"(<g id="eyes" fill="currentColor">)");
        const std::string angry = replaced(replaced(framed, R"(fill="#f0c040")", R"(fill="#e04020")"),
                                           R"(<rect id="box" x="4" y="4" width="10" height="10" fill="#2040a0"/>)",
                                           R"(<path id="brow" d="M14 14 L50 14 L50 18 L14 18 Z" fill="currentColor"/>)");
        files.svg = framed;
        a->SetAttribute("src", "back_face.svg");
        a->SetProperty("width", "128px");
        a->SetProperty("height", "128px");
        a->SetProperty("color", "#10c040");
        forge::ui::SvgPose swap{"back_face.svg", {}, {}, {}};
        swap.drawings.push_back({"angry", "back_face.angry.svg", std::make_shared<const std::string>(angry)});
        swap.drawing = "angry";
        swap.swap = true;
        swap.swap_from = "draw";
        swap.swap_k = 0.5;
        forge::ui::svg_pose(a, swap);
        frame();
        // As lunasvg draws both at 128 with the element's colour on their roots (where the plugin gives it).
        auto coloured = [](const std::string& text) {
            return replaced(text, R"(<svg xmlns="http://www.w3.org/2000/svg")", R"(<svg xmlns="http://www.w3.org/2000/svg" color="#10c040")");
        };
        forge::ui::SvgPose want = swap;
        want.drawings[0].text = std::make_shared<const std::string>(coloured(angry));
        std::vector<u8> mid;
        std::string why;
        REQUIRE(forge::ui::render_posed_svg(coloured(framed), want, 128, 128, mid, why));
        for (usize i = 0; i < mid.size(); i += 4)
            for (usize c = 0; c < 3; ++c) mid[i + c] = u8((mid[i + c] * mid[i + 3] + 127) / 255);
        CHECK(differ(pixels(0), mid, 2) == 0);
        // An <svg> element of the file cropped to its content (an <img> has no cropping): the ground fills the picture, so
        // the same box; the plugin's other way of drawing, the new document over the old there too.
        b->SetProperty("display", "none");
        Rml::Element* cropped = a->GetParentNode()->AppendChild(doc->CreateElement("svg"));
        cropped->SetAttribute("src", "back_face.svg");
        cropped->SetAttribute("crop-to-content", "");
        cropped->SetProperty("display", "block");
        cropped->SetProperty("width", "128px");
        cropped->SetProperty("height", "128px");
        cropped->SetProperty("color", "#10c040");
        forge::ui::svg_pose(cropped, swap);
        frame();
        CHECK(differ(pixels(128), mid, 2) == 0);
        CHECK(forge::ui::svg_problems().empty());
    }
    SUBCASE("reset: the element takes the file again; what the file lacks is said once") {
        CHECK(forge::ui::svg_move(a, "nose", 3, 3));
        CHECK(forge::ui::svg_morph(a, "box", "smile", 0.5));
        frame();
        CHECK(forge::ui::svg_fade(a, "head", 0.2));
        frame();
        const std::vector<std::string> said = forge::ui::svg_problems();
        INFO(said.size());
        REQUIRE(said.size() == 2);
        CHECK(std::count(said.begin(), said.end(), "«face.svg»: нет части с id «nose»") == 1);
        CHECK(std::count_if(said.begin(), said.end(), [](const std::string& line) {
                  return line.rfind("«face.svg», морфинг «box» в «smile»: морфинг меняет контур d, а «box» — <rect>", 0) == 0;
              }) == 1);
        CHECK(forge::ui::svg_problems().empty());
        CHECK(forge::ui::svg_part(a, "head"));
        forge::ui::svg_reset(a);
        CHECK_FALSE(forge::ui::svg_part(a, "head"));
        frame();
        CHECK(render.at_y[0] == render.at_y[64]);
        CHECK(differ(pixels(0), file) == 0);
    }
    SUBCASE("the page closed: the live element's own pictures let go, and nothing said of it stays") {
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 0.5));
        frame();
        CHECK(made() == 2); // the file's and a's own
        doc->Close();
        frame();
        CHECK(made() == 0);
        CHECK(files.open.empty());
        // A page made again: its elements show the file, whatever address they have.
        load();
        frame();
        CHECK_FALSE(forge::ui::svg_part(a, "mouth"));
        CHECK(render.at_y[0] == render.at_y[64]);
        CHECK(differ(pixels(0), file) == 0);
        CHECK(made() == 1);
    }
    SUBCASE("the file changed (as by F5) and the page made again: what is said now is drawn on the new file, nothing of the old") {
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 1));
        frame();
        // The new file: another smile, another head.
        const std::string old_smile = R"(d="M18 40 C24 54 40 54 46 40 Z")", new_smile = R"(d="M12 34 C22 60 42 60 52 34 Z")";
        const std::string changed = replaced(replaced(kFace, old_smile, new_smile), R"(fill="#f0c040")", R"(fill="#40c0f0")");
        files.svg = changed;
        doc->Close();
        frame();
        load();
        frame();
        CHECK(render.at_y[0] == render.at_y[64]);
        CHECK(differ(pixels(0), drawn(changed)) == 0);
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 1));
        frame();
        const std::string mouth = R"(d="M20 44 L44 44 L44 48 L20 48 Z" fill="#a02020")";
        const std::string want = replaced(changed, mouth, new_smile + R"( fill="#ff4040")");
        const std::string stale = replaced(changed, mouth, old_smile + R"( fill="#ff4040")"); // the morph made of the old file
        const int off = differ(pixels(0), drawn(want), 8);
        INFO("pixels off by more than 8 from the new file's smile: " << off);
        CHECK(off <= 4);
        CHECK(differ(pixels(0), drawn(stale), 8) > 20);
        CHECK(differ(pixels(64), drawn(changed)) == 0);
    }
    SUBCASE("a game begins again: every live element of its page draws the file, another page's keeps what was said") {
        Rml::ElementDocument* other = context->LoadDocumentFromMemory(R"(<rml><head><style>
        body { width: 200px; height: 200px; margin: 0; }
        img { display: block; width: 64px; height: 64px; margin-top: 128px; }
        </style></head><body><img id="c" src="face.svg"/></body></rml>)",
                                                                      "mem/other.rml");
        REQUIRE(other);
        other->Show();
        Rml::Element* c = other->GetElementById("c");
        REQUIRE(c);
        CHECK(forge::ui::svg_morph(a, "mouth", "smile", 1));
        CHECK(forge::ui::svg_fade(b, "head", 0.5));
        CHECK(forge::ui::svg_fade(c, "head", 0.5));
        frame();
        CHECK(differ(pixels(0), file) > 20);
        CHECK(differ(pixels(64), file) > 20);
        forge::ui::svg_reset_all(doc);
        CHECK_FALSE(forge::ui::svg_part(a, "mouth"));
        CHECK_FALSE(forge::ui::svg_part(b, "head"));
        CHECK(forge::ui::svg_part(c, "head"));
        frame();
        CHECK(render.at_y[0] == render.at_y[64]);
        CHECK(differ(pixels(0), file) == 0);
        CHECK(differ(pixels(128), drawn(replaced(kFace, R"(fill="#f0c040"/>)", R"(fill="#f0c040" opacity="0.5"/>)"))) == 0);
        other->Close();
    }

    Rml::Shutdown();
    Rml::SetRenderInterface(nullptr);
    CHECK(files.open.empty());
}

TEST_CASE("SVG pictures under transforms: rasterized at their scale, as it is when it holds, by steps while it changes") {
    using Rml::PictureScale;
    CHECK(PictureScale::Step(1) == 1);
    CHECK(PictureScale::Step(2) == 2);
    CHECK(PictureScale::Step(1.05f) == doctest::Approx(1.189207f));
    CHECK(PictureScale::Step(0.8f) == doctest::Approx(0.840896f));
    CHECK(PictureScale::Step(13.05f) == doctest::Approx(13.454343f)); // a scene of 160 × 120 on 1920 × 1080, zoomed
    CHECK(PictureScale::Step(100) == 64);
    CHECK(PictureScale::Step(1e5f) == 64);
    CHECK(PictureScale::Step(1e-5f) == doctest::Approx(1.0f / 64));

    CpuRender render;
    MemoryFiles files;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&files);
    REQUIRE(Rml::Initialise());
    forge::ui::install_live_svg();
    Rml::Context* context = Rml::CreateContext("scaled", {300, 600});
    REQUIRE(context);
    // Each kind of picture, 64 × 64 a hundred pixels below the last: an <img>, a background, an <svg>, the svg decorator
    // and a frame of nine parts (its corners as wide as their slice, so the frame is the file at 64 × 64 too).
    Rml::ElementDocument* doc = context->LoadDocumentFromMemory(R"(<rml><head><style>
        body { width: 300px; height: 600px; margin: 0; }
        #w { position: absolute; left: 0; top: 0; width: 64px; height: 600px; transform: scale(1.5); transform-origin: 0 0; }
        #w > * { display: block; position: absolute; left: 0; width: 64px; height: 64px; }
        #bg { decorator: image(face.svg); }
        #dec { decorator: svg(face.svg); }
        #frame { border-image-source: url(face.svg); border-image-slice: 16 fill; border-image-width: 16px; }
        </style></head><body><div id="w"><img id="img" src="face.svg" style="top: 0"/><div id="bg" style="top: 100px"/>
        <svg id="el" src="face.svg" width="64" height="64" style="top: 200px"/><div id="dec" style="top: 300px"/>
        <div id="frame" style="top: 400px"/></div></body></rml>)",
                                                                "mem/scaled.rml");
    REQUIRE(doc);
    doc->Show();
    Rml::Element* w = doc->GetElementById("w");
    Rml::Element* img = doc->GetElementById("img");
    REQUIRE(w);
    REQUIRE(img);
    auto frame = [&] {
        render.at_y.clear();
        context->Update();
        context->Render();
    };
    // The texture each picture was drawn with this frame, by where it is on the page.
    auto used = [&](int y) -> Rml::TextureHandle {
        REQUIRE(render.at_y.count(y));
        return render.at_y[y];
    };
    auto made = [&] { // textures of pictures alive now (not the <img>'s stand-in for its size)
        int n = 0;
        for (const auto& [handle, pixels] : render.textures) n += pixels.empty() ? 0 : 1;
        return n;
    };

    // At 1.5 from the first frame: one raster of 96 × 96 for all five, lunasvg's at that size.
    frame();
    const Rml::TextureHandle at15 = used(0);
    for (int y : {100, 200, 300}) CHECK(used(y) == at15);
    CHECK(render.at_y.count(400)); // the frame: its corners on top of the others, drawn from the same raster
    CHECK(render.sizes[at15] == Rml::Vector2i(96, 96));
    CHECK(differ(render.textures[at15], drawn(kFace, 96)) == 0);
    CHECK(render.generated == 1);
    CHECK(made() == 1);

    SUBCASE("a turn alone keeps the raster") {
        w->SetProperty("transform", "scale(1.5) rotate(30deg)");
        for (int i = 0; i < 3; ++i) frame();
        CHECK(used(0) == at15);
        CHECK(render.generated == 1);
    }
    SUBCASE("growing from 1.5 to 3: a raster at each step passed, then at 3 once it holds") {
        std::vector<int> sides;
        for (int i = 1; i <= 30; ++i) {
            const float s = 1.5f + 0.05f * static_cast<float>(i);
            w->SetProperty("transform", "scale(" + std::to_string(s) + ")");
            frame();
            const Rml::Vector2i size = render.sizes[used(0)];
            const int want = static_cast<int>(std::lround(64 * PictureScale::Step(s)));
            INFO("scale " << s << ": raster " << size.x << " × " << size.y << ", the step's " << want);
            CHECK(size == Rml::Vector2i(want, want));
            for (int y : {100, 200, 300}) CHECK(used(y) == used(0));
            if (sides.empty() || sides.back() != size.x) sides.push_back(size.x);
        }
        CHECK(sides == std::vector<int>{108, 128, 152, 181, 215}); // 64 × 2^(3/4) … 2^(7/4)
        CHECK(render.generated == 1 + 5);
        for (int i = 1; i < PictureScale::kStill; ++i) {
            frame();
            CHECK(render.sizes[used(0)] == Rml::Vector2i(215, 215)); // stretched from the last step a few frames more
        }
        frame();
        const Rml::TextureHandle at3 = used(0);
        CHECK(render.sizes[at3] == Rml::Vector2i(192, 192));
        CHECK(differ(render.textures[at3], drawn(kFace, 192)) == 0);
        for (int y : {100, 200, 300}) CHECK(used(y) == at3);
        CHECK(render.generated == 1 + 5 + 1);
        CHECK(made() == 1); // the steps' rasters let go
        // A live picture at that scale: drawn by its own document at 192 × 192.
        CHECK(forge::ui::svg_morph(img, "mouth", "smile", 1));
        frame();
        CHECK(used(0) != at3);
        CHECK(render.sizes[used(0)] == Rml::Vector2i(192, 192));
        const std::string smile =
            replaced(kFace, R"(d="M20 44 L44 44 L44 48 L20 48 Z" fill="#a02020")", R"(d="M18 40 C24 54 40 54 46 40 Z" fill="#ff4040")");
        const int off = differ(render.textures[used(0)], drawn(smile, 192), 8);
        INFO("pixels off by more than 8 at k = 1, 192 × 192: " << off);
        CHECK(off <= 24);
        CHECK(used(100) == at3);
    }
    SUBCASE("back to no transform: the box's size again") {
        w->SetProperty("transform", "none");
        for (int i = 0; i <= PictureScale::kStill; ++i) frame();
        CHECK(render.sizes[used(0)] == Rml::Vector2i(64, 64));
        CHECK(differ(render.textures[used(0)], drawn(kFace)) == 0);
        CHECK(made() == 1);
    }

    Rml::Shutdown();
    Rml::SetRenderInterface(nullptr);
    CHECK(files.open.empty());
}
