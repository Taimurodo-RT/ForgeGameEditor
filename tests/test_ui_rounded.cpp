// Web pages' backgrounds at rounded corners, and screens that come in with a movement: what a text game's interface
// built in «Интерфейс» showed. A gradient or a picture over a box with border-radius was drawn square (its tiles were not
// cut at the corners, though the box's colour was); here what the page gives the GPU is checked. A window shown always
// on looked half see-through on a snapshot; here it is checked to end whole, its veil too.

#include "forge/game/screens.h"
#include "forge/game/vars.h"
#include "forge/ui/html.h"

#include <RmlUi/Core.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace forge;

namespace {

// Files from disk as they are named; the user agent sheet of web pages ("/web/html.rcss") from the UI folder of the sources.
struct DiskFiles final : Rml::FileInterface {
    Rml::FileHandle Open(const Rml::String& path) override {
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) return reinterpret_cast<Rml::FileHandle>(f);
        const std::string web = path.rfind("/web/", 0) == 0 ? path.substr(1) : path;
        if (web.rfind("web/", 0) != 0) return 0;
        return reinterpret_cast<Rml::FileHandle>(std::fopen((std::string(FORGE_SOURCE_DIR "/ui/") + web).c_str(), "rb"));
    }
    void Close(Rml::FileHandle file) override { std::fclose(reinterpret_cast<std::FILE*>(file)); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override { return std::fread(buffer, 1, size, reinterpret_cast<std::FILE*>(file)); }
    bool Seek(Rml::FileHandle file, long offset, int origin) override { return std::fseek(reinterpret_cast<std::FILE*>(file), offset, origin) == 0; }
    size_t Tell(Rml::FileHandle file) override { return static_cast<size_t>(std::ftell(reinterpret_cast<std::FILE*>(file))); }
};

// Keeps every mesh the page makes (in the element's own coordinates); draws nothing. Pictures are 64 × 64.
struct Meshes final : Rml::RenderInterface {
    std::vector<Rml::Mesh> made;
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
        Rml::Mesh m;
        m.vertices.assign(vertices.begin(), vertices.end());
        m.indices.assign(indices.begin(), indices.end());
        made.push_back(std::move(m));
        return made.size();
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String&) override {
        size = {64, 64};
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    // Gradients are shaders: without one the page makes no mesh for them.
    Rml::CompiledShaderHandle CompileShader(const Rml::String&, const Rml::Dictionary&) override { return 1; }
    void RenderShader(Rml::CompiledShaderHandle, Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseShader(Rml::CompiledShaderHandle) override {}
};

struct Page {
    Meshes render;
    DiskFiles files;
    Rml::Context* context = nullptr;
    Page() {
        Rml::SetRenderInterface(&render);
        Rml::SetFileInterface(&files);
        REQUIRE(Rml::Initialise());
        context = Rml::CreateContext("rounded", {600, 400});
        REQUIRE(context);
    }
    ~Page() { Rml::Shutdown(); }
    // The meshes of a page with one box of this style (its colour none), drawn once.
    std::vector<Rml::Mesh> box(const std::string& style) {
        render.made.clear();
        const std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body style=\"margin: 0;\">"
                                 "<div style=\"position: absolute; left: 20px; top: 10px; " +
                                 style + "\"></div></body></html>";
        Rml::ElementDocument* doc = context->LoadDocumentFromMemory(ui::html_to_rml(html, "/web/html.rcss"), "rounded.html");
        REQUIRE(doc);
        REQUIRE(doc->GetTagName() == "html"); // a web page: backgrounds placed as in a browser
        doc->Show();
        context->Update();
        context->Render();
        std::vector<Rml::Mesh> out = render.made;
        doc->Close();
        context->Update();
        return out;
    }
};

// How far a point is outside a w × h box with corners rounded by r (0: on its edge or inside).
f32 outside(Rml::Vector2f p, f32 w, f32 h, f32 r) {
    const f32 cx = std::clamp(p.x, r, w - r), cy = std::clamp(p.y, r, h - r);
    const f32 d = std::hypot(p.x - cx, p.y - cy) - r;
    const f32 edge = std::max({-p.x, -p.y, p.x - w, p.y - h});
    return std::max({d, edge, 0.0f});
}

f32 area(const Rml::Mesh& m) {
    f64 a = 0;
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        const Rml::Vector2f p = m.vertices[m.indices[i]].position, q = m.vertices[m.indices[i + 1]].position,
                            s = m.vertices[m.indices[i + 2]].position;
        a += std::abs((q.x - p.x) * (s.y - p.y) - (s.x - p.x) * (q.y - p.y)) * 0.5;
    }
    return static_cast<f32>(a);
}

constexpr f32 kPi = 3.14159265f;

} // namespace

TEST_CASE("A web page's gradient or picture over a box with rounded corners is cut at them, as its colour is") {
    Page page;
    // Only an outline of the right size and place: a mesh of this page's box, its points all on it or inside it.
    auto check = [](const std::vector<Rml::Mesh>& meshes, f32 w, f32 h, f32 r, f32 expected_area) {
        REQUIRE(meshes.size() == 1);
        const Rml::Mesh& m = meshes[0];
        f32 worst = 0;
        for (const Rml::Vertex& v : m.vertices) worst = std::max(worst, outside(v.position, w, h, r));
        CHECK(worst < 0.01f);
        // All of it covered: the outline's area (its arcs are drawn by straight pieces, a little short of the curve).
        CHECK(area(m) == doctest::Approx(expected_area).epsilon(0.01));
        return m;
    };
    SUBCASE("a circle with a gradient not repeated, as the UI editor writes a fill") {
        const Rml::Mesh m = check(page.box("width: 100px; height: 100px; border-radius: 50%; background-image: linear-gradient(#ff0000, #0000ff);"
                                           " background-size: auto; background-repeat: no-repeat; background-position: 0% 0%;"),
                                  100, 100, 50, kPi * 50 * 50);
        // The gradient's coordinates: where in its picture (the whole box) each point is.
        for (const Rml::Vertex& v : m.vertices) {
            CHECK(v.tex_coord.x == doctest::Approx(v.position.x));
            CHECK(v.tex_coord.y == doctest::Approx(v.position.y));
        }
    }
    SUBCASE("a rounded box with a gradient in tiles: each tile cut, each point where it is in its own tile") {
        const Rml::Mesh m = check(page.box("width: 200px; height: 80px; border-radius: 20px; background-image: radial-gradient(#ff0000, #0000ff);"
                                           " background-size: 50px 40px;"),
                                  200, 80, 20, 200 * 80 - (4 - kPi) * 20 * 20);
        for (const Rml::Vertex& v : m.vertices) {
            CHECK(v.tex_coord.x >= -0.01f);
            CHECK(v.tex_coord.x <= 50.01f);
            CHECK(v.tex_coord.y >= -0.01f);
            CHECK(v.tex_coord.y <= 40.01f);
            const f32 tile_x = (v.position.x - v.tex_coord.x) / 50, tile_y = (v.position.y - v.tex_coord.y) / 40;
            CHECK(std::abs(tile_x - std::round(tile_x)) < 0.001f);
            CHECK(std::abs(tile_y - std::round(tile_y)) < 0.001f);
        }
    }
    SUBCASE("a circle with a picture stretched over it") {
        const Rml::Mesh m = check(page.box("width: 120px; height: 120px; border-radius: 50%; background-image: url('picture.png');"
                                           " background-size: 100% 100%; background-repeat: no-repeat;"),
                                  120, 120, 60, kPi * 60 * 60);
        // The picture's coordinates: the share of it each point shows.
        for (const Rml::Vertex& v : m.vertices) {
            CHECK(v.tex_coord.x == doctest::Approx(v.position.x / 120));
            CHECK(v.tex_coord.y == doctest::Approx(v.position.y / 120));
        }
    }
    SUBCASE("square corners: the tiles stay whole quads") {
        const std::vector<Rml::Mesh> meshes = page.box("width: 100px; height: 60px; background-image: linear-gradient(#ff0000, #0000ff);"
                                                       " background-size: 50px 30px;");
        REQUIRE(meshes.size() == 1);
        CHECK(meshes[0].vertices.size() == 4 * 4);
        CHECK(area(meshes[0]) == doctest::Approx(100 * 60));
    }
}

TEST_CASE("A screen always on that comes in with a movement ends whole, not see-through") {
    Meshes render;
    DiskFiles files;
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&files);
    REQUIRE(Rml::Initialise());
    Rml::Context* context = Rml::CreateContext("appear", {400, 300});
    REQUIRE(context);
    {
        game::GameScreens screens;
        u32 frames = 0; // the clock: on by a 60th of a second a frame
        screens.set_clock([&] { return 1'000'000'000ULL + static_cast<u64>(frames) * 1'000'000'000ULL / 60; });
        // A HUD, and a window with its veil that darkens the game, both set to «Всегда на экране».
        const std::string hud = R"(<!DOCTYPE html><html><head><meta charset="utf-8"></head><body>)"
                                R"(<div id="hud" forge-screen="playing" forge-size="400 300" forge-appear="rise" forge-appear-time="0.25")"
                                R"( style="position: absolute; left: 0; top: 0; width: 400px; height: 300px; background-color: #203040;">)"
                                R"(</div></body></html>)";
        const std::string window = R"(<!DOCTYPE html><html><head><meta charset="utf-8"></head><body>)"
                                   R"(<div id="forge-dim" style="position: absolute; left: 0; top: 0; width: 400px; height: 300px; background-color: rgba(0, 0, 0, 0.5);"></div>)"
                                   R"(<div id="bag" forge-screen="playing" forge-size="400 300" forge-pauses="1" forge-dim="1" forge-appear="zoom")"
                                   R"( forge-appear-time="0.4" style="position: absolute; left: 0; top: 0; width: 400px; height: 300px; background-color: #604020;">)"
                                   R"(</div></body></html>)";
        REQUIRE(screens.load_page(context, "hud", hud, "hud.html"));
        REQUIRE(screens.load_page(context, "bag", window, "bag.html"));
        game::Vars vars;
        auto step = [&](u32 n) {
            for (u32 i = 0; i < n; ++i) {
                ++frames;
                screens.update(vars, true, false, 400, 300);
                context->Update();
            }
        };
        // In a web page an element's opacity fades it with its contents as one picture.
        auto opacity = [&](const char* id) {
            Rml::Element* e = nullptr;
            for (int i = 0; i < context->GetNumDocuments() && !e; ++i) e = context->GetDocument(i)->GetElementById(id);
            REQUIRE(e);
            return e->GetComputedValues().group_opacity();
        };
        step(1);
        CHECK(opacity("hud") < 0.2f); // coming in
        CHECK(opacity("bag") < 0.2f);
        CHECK(opacity("forge-dim") < 0.2f); // the veil comes with its window
        step(7);
        CHECK(opacity("hud") > 0.2f);
        CHECK(opacity("hud") < 1.0f);
        CHECK(opacity("bag") < 1.0f);
        step(17); // past both movements
        CHECK(opacity("hud") == doctest::Approx(1.0f));
        CHECK(opacity("bag") == doctest::Approx(1.0f));
        CHECK(opacity("forge-dim") == doctest::Approx(1.0f));
        // Hidden with the game and shown again: they come in again and end whole too.
        screens.update(vars, false, false, 400, 300);
        context->Update();
        step(30);
        CHECK(opacity("hud") == doctest::Approx(1.0f));
        CHECK(opacity("bag") == doctest::Approx(1.0f));
        CHECK(opacity("forge-dim") == doctest::Approx(1.0f));
        // And when the frames come slowly (a loaded computer): a few big steps, the last one past the end.
        screens.update(vars, false, false, 400, 300);
        context->Update();
        frames += 2;
        screens.update(vars, true, false, 400, 300);
        context->Update();
        frames += 20;
        screens.update(vars, true, false, 400, 300);
        context->Update();
        CHECK(opacity("hud") == doctest::Approx(1.0f));
        CHECK(opacity("bag") == doctest::Approx(1.0f));
    }
    Rml::Shutdown();
}
