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
    Rml::ElementDocument* doc = context->LoadDocumentFromMemory(R"(<rml><head><style>
        body { width: 200px; height: 200px; margin: 0; }
        img { display: block; width: 64px; height: 64px; }
        </style></head><body><img id="a" src="face.svg"/><img id="b" src="face.svg"/></body></rml>)",
                                                                "mem/page.rml");
    REQUIRE(doc);
    doc->Show();
    Rml::Element* a = doc->GetElementById("a");
    Rml::Element* b = doc->GetElementById("b");
    REQUIRE(a);
    REQUIRE(b);
    auto frame = [&] {
        render.at_y.clear();
        context->Update();
        context->Render();
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
    CHECK(PictureScale::Step(100) == 8);

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
