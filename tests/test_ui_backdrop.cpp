// Part 6г4 of docs/models/сцены-модель.md: the snapshot a <backdrop> takes of the frame under it, and pictures made by
// code straight into /gpu/ textures (a scene's masks), drawn on the GPU (software Vulkan on CI).

#include "forge/render/offscreen.h"
#include "forge/ui/ui.h"

#include <RmlUi/Core.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

using namespace forge;

namespace {

constexpr u32 kW = 400, kH = 240;

void fill(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, f32 r, f32 g, f32 b) {
    SDL_GPUColorTargetInfo color{};
    color.texture = texture;
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    color.clear_color = {r, g, b, 1};
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &color, 1, nullptr));
}

std::array<int, 4> at(const std::vector<u8>& rgba, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * kW + x) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

// Within 2 of each channel: the edges of pictures scaled are smoothed.
bool near(const std::array<int, 4>& a, const std::array<int, 4>& b) {
    for (int c = 0; c < 4; ++c)
        if (std::abs(a[c] - b[c]) > 2) return false;
    return true;
}

constexpr std::array<int, 4> kYellow{255, 255, 0, 255}, kCyan{0, 255, 255, 255}, kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255},
    kGreen{0, 255, 0, 255};

} // namespace

TEST_CASE("Снимок под <backdrop>: мир и то, что нарисовано под ним, ничего выше, без мира — чёрный; скрытый — снимка нет; картинки кода в /gpu/") {
    SDL_GPUDevice* device = render::create_offscreen_device();
    REQUIRE(device);
    SDL_GPUTexture* world = render::create_render_target(device, kW, kH);
    SDL_GPUTexture* target = render::create_render_target(device, kW, kH);
    REQUIRE(world);
    REQUIRE(target);
    {
        ui::Ui ui;
        ui::UiConfig config;
        config.root = std::filesystem::path(FORGE_SOURCE_DIR) / "ui";
        config.hot_reload = false;
        config.debugger = false;
        config.display_scale = 1;
        REQUIRE(ui.init(device, nullptr, config));
        Rml::Context* context = ui.create_context("backdrop", kW, kH);
        REQUIRE(context);
        const char* kStyle = "<style>body { position: absolute; left: 0px; top: 0px; width: 100%; height: 100%; }"
                             " div, img, backdrop { display: block; position: absolute; }</style>";
        // Under it: a red box on the world.
        Rml::ElementDocument* below = context->LoadDocumentFromMemory(
            std::string("<rml><head>") + kStyle +
            "</head><body><div id=\"box\" style=\"left: 20px; top: 20px; width: 80px; height: 80px; background-color: #ff0000;\"/></body></rml>");
        REQUIRE(below);
        below->Show();
        // Over it: the snapshot first, then a magenta box (not in it), green over all, the snapshot shown at half its size in
        // the bottom right quarter and a blue square masked by a picture of code.
        Rml::ElementDocument* over = context->LoadDocumentFromMemory(
            std::string("<rml><head>") + kStyle +
            "</head><body>"
            "<backdrop id=\"snap\" name=\"snap\" style=\"left: 0px; top: 0px; width: 100%; height: 100%;\"/>"
            "<div style=\"left: 300px; top: 20px; width: 80px; height: 80px; background-color: #ff00ff;\"/>"
            "<div style=\"left: 0px; top: 0px; width: 400px; height: 240px; background-color: #00ff00;\"/>"
            "<img src=\"/gpu/snap\" style=\"left: 200px; top: 120px; width: 200px; height: 120px;\"/>"
            "<div style=\"left: 0px; top: 120px; width: 100px; height: 100px; background-color: #0000ff; mask-image: url(/gpu/mask);"
            " mask-size: 100% 100%;\"/>"
            "</body></rml>");
        REQUIRE(over);
        over->Show();
        // The mask, a pixel to each of the square's: white on the left half, nothing on the right.
        auto halves = [](u32 w, u32 h) {
            std::vector<u8> out(static_cast<usize>(w) * h * 4, 0);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                    if (w > h ? x < w / 2 : y < h / 2) std::fill_n(out.begin() + static_cast<std::ptrdiff_t>((y * w + x) * 4), 4, u8(255));
            return out;
        };
        const std::vector<u8> left_right = halves(100, 1);
        ui.set_gpu_image("mask", left_right.data(), 100, 1);

        std::vector<u8> rgba;
        auto frame = [&](f32 r, f32 g, f32 b, bool with_world = true) {
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            REQUIRE(cmd);
            // As the game draws a frame that needs the world under the UI: the world into its own texture, then to the
            // frame.
            fill(cmd, world, r, g, b);
            fill(cmd, target, r, g, b);
            if (with_world) ui.set_world(world);
            ui.update();
            ui.render(cmd, target, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kW, kH);
            ui.set_world(nullptr);
            REQUIRE(SDL_SubmitGPUCommandBuffer(cmd));
            REQUIRE(render::read_pixels(device, target, kW, kH, rgba));
        };

        frame(1, 1, 0);
        CHECK(near(at(rgba, 5, 5), kGreen));
        // The snapshot (x, y) is at (200 + x / 2, 120 + y / 2): the world and the red box under it, not the magenta box
        // drawn after it nor the green.
        CHECK(near(at(rgba, 230, 150), kRed));
        CHECK(near(at(rgba, 205, 125), kYellow));
        CHECK(near(at(rgba, 370, 150), kYellow));
        CHECK(near(at(rgba, 275, 220), kYellow));
        // The square shows where its mask is.
        CHECK(near(at(rgba, 10, 170), kBlue));
        CHECK(near(at(rgba, 90, 170), kGreen));

        // Hidden, it takes none: the picture is the last one taken, whatever is under it now.
        Rml::Element* snap = over->GetElementById("snap");
        REQUIRE(snap);
        snap->SetProperty("display", "none");
        below->GetElementById("box")->SetProperty("background-color", "#0000ff");
        frame(0, 1, 1);
        CHECK(near(at(rgba, 230, 150), kRed));
        CHECK(near(at(rgba, 205, 125), kYellow));
        snap->RemoveProperty("display");
        frame(0, 1, 1);
        CHECK(near(at(rgba, 230, 150), kBlue));
        CHECK(near(at(rgba, 205, 125), kCyan));

        // A picture of code made again of another size keeps its name: white at the top, nothing at the bottom.
        const std::vector<u8> top_bottom = halves(1, 100);
        ui.set_gpu_image("mask", top_bottom.data(), 1, 100);
        frame(0, 1, 1);
        CHECK(near(at(rgba, 50, 125), kBlue));
        CHECK(near(at(rgba, 50, 215), kGreen));
        CHECK(near(at(rgba, 90, 125), kBlue));
        // Two pictures of the same name before a frame: only the newer goes to the GPU (white at the bottom).
        std::vector<u8> bottom_top = top_bottom;
        for (u8& c : bottom_top) c = static_cast<u8>(255 - c);
        frame(0, 1, 1);
        CHECK(ui.stats().uploads == 0);
        ui.set_gpu_image("mask", top_bottom.data(), 1, 100);
        ui.set_gpu_image("mask", bottom_top.data(), 1, 100);
        frame(0, 1, 1);
        CHECK(ui.stats().uploads == 1);
        CHECK(near(at(rgba, 50, 125), kGreen));
        CHECK(near(at(rgba, 50, 215), kBlue));
        // Forgotten, it shows nothing: the square masked by nothing is not there.
        CHECK(ui.gpu_images() == 2);
        ui.drop_gpu_image("mask");
        CHECK(ui.gpu_images() == 1);
        frame(0, 1, 1);
        CHECK(near(at(rgba, 50, 125), kGreen));
        // No world this frame: black under what the UI drew, not the frame's colour.
        frame(0, 1, 1, false);
        CHECK(near(at(rgba, 230, 150), kBlue));
        CHECK(near(at(rgba, 205, 125), (std::array<int, 4>{0, 0, 0, 255})));
        // The snapshot forgotten too.
        ui.drop_gpu_image("snap");
        CHECK(ui.gpu_images() == 0);
        ui.shutdown();
    }
    SDL_ReleaseGPUTexture(device, world);
    SDL_ReleaseGPUTexture(device, target);
    render::destroy_offscreen_device(device);
}
