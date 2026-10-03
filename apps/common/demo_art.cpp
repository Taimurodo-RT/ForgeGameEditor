#include "demo_art.h"

#include "forge/world/generators.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace forge::demo {

using namespace forge::world;

namespace {

u32 pixel_hash(u32 a, u32 b, u32 c) {
    u32 h = a * 374761393u + b * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct Rgb {
    u8 r, g, b;
};

// Placeholder art until real tile sets arrive with the asset library: each
// sample tile gets a colour, a little texture and an edge.
} // namespace

std::vector<u8> make_tile_atlas() {
    const u32 size = kTileCellPx * kTileCells;
    std::vector<u8> px(static_cast<usize>(size) * size * 4, 0);
    auto put = [&](u32 id, u32 x, u32 y, Rgb c, u8 a = 255) {
        const u32 X = (id % kTileCells) * kTileCellPx + x, Y = (id / kTileCells) * kTileCellPx + y;
        u8* p = &px[(static_cast<usize>(Y) * size + X) * 4];
        p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = a;
    };
    auto shade = [](Rgb c, i32 d) {
        auto f = [d](u8 v) { return static_cast<u8>(std::clamp(static_cast<i32>(v) + d, 0, 255)); };
        return Rgb{f(c.r), f(c.g), f(c.b)};
    };
    struct Solid {
        TileId id;
        Rgb color;
        i32 grain;
        bool edge;
    };
    const Solid solids[] = {
        {TileGrass, {86, 160, 64}, 14, true},       {TileDirt, {120, 84, 52}, 12, true},
        {TileStone, {118, 118, 126}, 14, true},     {TileSand, {214, 194, 128}, 10, true},
        {TileDirtWall, {92, 64, 42}, 6, false},     {TileStoneWall, {84, 84, 92}, 6, false},
        {TileWater, {52, 120, 196}, 6, false},      {TileDeepWater, {34, 82, 156}, 5, false},
        {TileMeadow, {104, 168, 72}, 10, false},    {TileForest, {58, 118, 52}, 10, false},
        {TileSnow, {232, 238, 244}, 6, false},
    };
    for (const Solid& s : solids) {
        for (u32 y = 0; y < kTileCellPx; ++y) {
            for (u32 x = 0; x < kTileCellPx; ++x) {
                const i32 n = static_cast<i32>(pixel_hash(s.id, x / 2, y / 2) % (2 * s.grain + 1)) - s.grain;
                Rgb c = shade(s.color, n);
                if (s.edge && (x == kTileCellPx - 1 || y == kTileCellPx - 1)) c = shade(c, -28);
                if (s.edge && (x == 0 || y == 0)) c = shade(c, 16);
                put(s.id, x, y, c);
            }
        }
    }
    // Grass: dirt with a green top.
    for (u32 y = 0; y < kTileCellPx; ++y)
        for (u32 x = 0; x < kTileCellPx; ++x)
            if (y > 4 + pixel_hash(x, 1, 2) % 3) put(TileGrass, x, y, shade(Rgb{120, 84, 52}, static_cast<i32>(pixel_hash(x, y, 9) % 20) - 10));
    // Ores: stone with coloured flecks.
    const std::pair<TileId, Rgb> ores[] = {{TileCopper, {214, 120, 60}}, {TileIron, {196, 170, 150}}, {TileGold, {246, 206, 60}}};
    for (const auto& [id, fleck] : ores)
        for (u32 y = 0; y < kTileCellPx; ++y)
            for (u32 x = 0; x < kTileCellPx; ++x) {
                Rgb c = shade(Rgb{118, 118, 126}, static_cast<i32>(pixel_hash(id, x / 2, y / 2) % 29) - 14);
                if (pixel_hash(id, x / 3, y / 3) % 5 == 0) c = fleck;
                if (x == kTileCellPx - 1 || y == kTileCellPx - 1) c = shade(c, -28);
                put(id, x, y, c);
            }
    // Objects on a transparent background: tree crowns and rocks.
    for (u32 y = 0; y < kTileCellPx; ++y)
        for (u32 x = 0; x < kTileCellPx; ++x) {
            const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.0f;
            const f32 d = std::sqrt(dx * dx + dy * dy);
            if (d < 6.5f) put(TileTree, x, y, shade(Rgb{40, 96, 40}, static_cast<i32>(pixel_hash(x, y, 3) % 30) - 15 - static_cast<i32>(d * 3)));
            const f32 ry = static_cast<f32>(y) - 9.0f;
            if (std::sqrt(dx * dx + ry * ry * 2.0f) < 6.0f) put(TileRock, x, y, shade(Rgb{128, 124, 120}, static_cast<i32>(pixel_hash(x, y, 4) % 24) - 12 - static_cast<i32>(ry * 3)));
        }
    return px;
}

SheetImage make_sprite_sheet() {
    constexpr u32 kPx = 16, kCell = kPx + 2, kCols = 8, kRows = 5;
    SheetImage img;
    img.width = kCell * kCols;
    img.height = kCell * kRows;
    img.rgba.assign(static_cast<usize>(img.width) * img.height * 4, 0);
    auto frame_origin = [&](u32 f, u32& ox, u32& oy) {
        ox = (f % kCols) * kCell + 1;
        oy = (f / kCols) * kCell + 1;
    };
    auto put = [&](u32 f, i32 x, i32 y, Rgb c, u8 a = 255) {
        if (x < 0 || y < 0 || x >= static_cast<i32>(kPx) || y >= static_cast<i32>(kPx)) return;
        u32 ox, oy;
        frame_origin(f, ox, oy);
        u8* p = &img.rgba[((static_cast<usize>(oy) + static_cast<u32>(y)) * img.width + ox + static_cast<u32>(x)) * 4];
        p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = a;
    };
    auto shade = [](Rgb c, i32 d) {
        auto f = [d](u8 v) { return static_cast<u8>(std::clamp(static_cast<i32>(v) + d, 0, 255)); };
        return Rgb{f(c.r), f(c.g), f(c.b)};
    };
    const Rgb bodies[kCritterKinds] = {{214, 82, 64},  {86, 170, 92},  {84, 132, 214}, {226, 186, 64},
                                       {170, 96, 196}, {72, 186, 186}, {222, 132, 64}, {200, 200, 210}};
    for (u32 kind = 0; kind < kCritterKinds; ++kind) {
        for (u32 step = 0; step < 2; ++step) {
            const u32 f = kind * 2 + step;
            // Legs, swapping sides between the two steps.
            for (i32 leg = 0; leg < 3; ++leg) {
                const i32 lx = 4 + leg * 4;
                const i32 dy = ((leg + static_cast<i32>(step)) & 1) ? 1 : 0;
                put(f, lx, 12 + dy, {40, 32, 30});
                put(f, lx, 13 + dy, {40, 32, 30});
            }
            // Body: an oval with a dark rim, lit from the top left.
            for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
                for (i32 x = 0; x < static_cast<i32>(kPx); ++x) {
                    const f32 dx = (static_cast<f32>(x) - 7.5f) / 6.5f, dy = (static_cast<f32>(y) - 8.0f) / 5.0f;
                    const f32 d = dx * dx + dy * dy;
                    if (d > 1.0f) continue;
                    Rgb c = d > 0.7f ? shade(bodies[kind], -70) : shade(bodies[kind], static_cast<i32>((-dx - dy) * 24));
                    if (kind == 4 && ((x + y) % 5 == 0)) c = shade(c, 40); // spots
                    put(f, x, y, c);
                }
            // Eyes looking forward (to the right).
            put(f, 10, 6, {250, 250, 250});
            put(f, 11, 6, {20, 20, 24});
            put(f, 12, 7, {250, 250, 250});
            put(f, 13, 7, {20, 20, 24});
        }
    }
    // Leaf: a pointed oval with a vein.
    for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
        for (i32 x = 0; x < static_cast<i32>(kPx); ++x) {
            const f32 t = (static_cast<f32>(x) - 1.0f) / 14.0f;
            const f32 half = 5.5f * std::sin(std::clamp(t, 0.0f, 1.0f) * 3.14159f);
            const f32 dy = static_cast<f32>(y) - 7.5f;
            if (std::fabs(dy) > half) continue;
            put(kFrameLeaf, x, y, std::fabs(dy) < 0.6f ? Rgb{70, 120, 40} : shade(Rgb{110, 170, 60}, static_cast<i32>(-dy * 4)));
        }
    // Glow: white, fading out from the centre (alpha carries the shape).
    for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
        for (i32 x = 0; x < static_cast<i32>(kPx); ++x) {
            const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
            const f32 a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / 8.0f, 0.0f, 1.0f);
            put(kFrameGlow, x, y, {255, 255, 255}, static_cast<u8>(a * a * 255.0f));
        }
    // Crate: planks with a dark frame and a cross brace.
    for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
        for (i32 x = 0; x < static_cast<i32>(kPx); ++x) {
            const bool frame = x < 2 || y < 2 || x > 13 || y > 13;
            const bool brace = std::abs(x - y) < 2 || std::abs(x + y - 15) < 2;
            Rgb c = shade(Rgb{176, 124, 70}, (y % 4 == 0) ? -22 : static_cast<i32>((x * 7 + y * 3) % 9) - 4);
            if (brace) c = Rgb{150, 100, 56};
            if (frame) c = Rgb{104, 68, 40};
            put(kFrameCrate, x, y, c);
        }
    // Ball: lit from the top left, with a stripe so its spin shows.
    for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
        for (i32 x = 0; x < static_cast<i32>(kPx); ++x) {
            const f32 dx = (static_cast<f32>(x) - 7.5f) / 7.5f, dy = (static_cast<f32>(y) - 7.5f) / 7.5f;
            const f32 d = dx * dx + dy * dy;
            if (d > 1.0f) continue;
            const Rgb base = std::fabs(dy) < 0.22f ? Rgb{245, 245, 240} : Rgb{220, 60, 60};
            put(kFrameBall, x, y, d > 0.8f ? shade(base, -80) : shade(base, static_cast<i32>((-dx - dy) * 30)));
        }

    // --- factory ---------------------------------------------------------------
    auto each_px = [&](auto&& fn) {
        for (i32 y = 0; y < static_cast<i32>(kPx); ++y)
            for (i32 x = 0; x < static_cast<i32>(kPx); ++x) fn(x, y);
    };
    // Belt: dark rails, a grey band with chevrons pointing east.
    each_px([&](i32 x, i32 y) {
        Rgb c{78, 78, 84};
        if (y < 2 || y > 13) c = Rgb{40, 40, 44};
        else if (std::abs(y - 7) + (x % 8) == 5 || std::abs(y - 8) + (x % 8) == 5) c = Rgb{226, 180, 52};
        put(kFrameBelt, x, y, c);
    });
    // Pipe: a lit tube across with a flange, joined to all sides.
    each_px([&](i32 x, i32 y) {
        const bool tube = (y >= 4 && y <= 11) || (x >= 4 && x <= 11);
        if (!tube) return;
        const i32 d = (y >= 4 && y <= 11) ? y - 4 : x - 4;
        Rgb c = shade(Rgb{120, 140, 156}, 30 - d * 8);
        if (x == 7 || x == 8 || y == 7 || y == 8) c = shade(c, -10);
        put(kFramePipe, x, y, c);
    });
    // Pole: a wooden post with a cross bar and two insulators.
    each_px([&](i32 x, i32 y) {
        if (x >= 7 && x <= 8 && y >= 2) put(kFramePole, x, y, shade(Rgb{120, 84, 50}, x == 7 ? 15 : -10));
        if (y >= 3 && y <= 4 && x >= 3 && x <= 12) put(kFramePole, x, y, Rgb{100, 70, 42});
        if (y == 2 && (x == 3 || x == 12)) put(kFramePole, x, y, Rgb{200, 220, 230});
    });
    // Machines: a body with a dark frame, each with its own mark.
    auto machine = [&](u32 f, Rgb body, auto&& mark) {
        each_px([&](i32 x, i32 y) {
            Rgb c = shade(body, static_cast<i32>((15 - x - y) * 2));
            if (x < 1 || y < 1 || x > 14 || y > 14) c = shade(body, -90);
            else if (x < 2 || y < 2 || x > 13 || y > 13) c = shade(body, -40);
            mark(x, y, c);
            put(f, x, y, c);
        });
    };
    machine(kFrameDrill, Rgb{170, 140, 60}, [&](i32 x, i32 y, Rgb& c) {
        const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
        if (dx * dx + dy * dy < 16 && (std::abs(x - y) < 2 || std::abs(x + y - 15) < 2)) c = Rgb{60, 60, 66};
    });
    machine(kFrameFurnace, Rgb{150, 110, 96}, [&](i32 x, i32 y, Rgb& c) {
        if (x >= 5 && x <= 10 && y >= 7 && y <= 12) c = y > 9 ? Rgb{255, 150, 40} : Rgb{255, 210, 90};
    });
    machine(kFrameAssembler, Rgb{90, 120, 160}, [&](i32 x, i32 y, Rgb& c) {
        const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
        const f32 r = std::sqrt(dx * dx + dy * dy);
        if (r < 5.5f && r > 2.0f && (static_cast<i32>(std::atan2(dy, dx) * 2.5f + 8) % 2 == 0 || r < 4.2f)) c = Rgb{200, 200, 210};
    });
    machine(kFrameBoiler, Rgb{176, 72, 60}, [&](i32 x, i32 y, Rgb& c) {
        if (y >= 9 && y <= 11 && x >= 4 && x <= 11) c = Rgb{255, 160, 50};
        if (y == 4 && x >= 4 && x <= 11) c = Rgb{220, 220, 220};
    });
    machine(kFrameEngine, Rgb{110, 116, 100}, [&](i32 x, i32 y, Rgb& c) {
        if (y >= 6 && y <= 9 && x >= 3 && x <= 12) c = Rgb{180, 180, 170};
        if (x >= 10 && x <= 12 && y >= 4 && y <= 11) c = Rgb{60, 64, 60};
    });
    machine(kFramePump, Rgb{70, 110, 170}, [&](i32 x, i32 y, Rgb& c) {
        const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
        if (dx * dx + dy * dy < 12) c = Rgb{120, 190, 250};
    });
    machine(kFrameChest, Rgb{150, 106, 60}, [&](i32 x, i32 y, Rgb& c) {
        if (y == 6 || y == 7) c = Rgb{90, 60, 34};
        if (y >= 6 && y <= 9 && x >= 7 && x <= 8) c = Rgb{230, 200, 80};
    });
    // Items: small, on a transparent background.
    each_px([&](i32 x, i32 y) {
        const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
        const f32 d = std::sqrt(dx * dx + dy * dy);
        // Ore: a rusty lump with bright specks.
        if (d < 6.0f + static_cast<f32>(pixel_hash(x, y, 9) % 3) - 1.0f)
            put(kFrameOre, x, y, (pixel_hash(x, y, 7) % 5 == 0) ? Rgb{240, 170, 110} : shade(Rgb{186, 100, 56}, static_cast<i32>(-dx - dy) * 5));
        // Plate: a lit square.
        if (std::fabs(dx) < 5.5f && std::fabs(dy) < 4.5f)
            put(kFramePlate, x, y, std::fabs(dx) > 4.6f || std::fabs(dy) > 3.6f ? Rgb{120, 130, 140} : shade(Rgb{190, 200, 212}, static_cast<i32>(-dx - dy) * 3));
        // Gear: teeth around a ring with a hole.
        const f32 a = std::atan2(dy, dx);
        const f32 outer = 5.0f + (std::sin(a * 8.0f) > 0 ? 2.0f : 0.0f);
        if (d < outer && d > 2.0f) put(kFrameGear, x, y, shade(Rgb{170, 176, 186}, static_cast<i32>(-dx - dy) * 4));
        // Coal: a black lump with glints.
        if (d < 5.5f) put(kFrameCoal, x, y, (pixel_hash(x, y, 5) % 7 == 0) ? Rgb{120, 120, 130} : Rgb{34, 32, 36});
    });
    for (u32 f = 0; f < kRows * kCols; ++f) {
        u32 ox, oy;
        frame_origin(f, ox, oy);
        img.frames.push_back({ox, oy, kPx, kPx});
    }
    return img;
}

render::LightRules side_view_light_rules() {
    render::LightRules rules;
    rules.blocking_layer = 1;
    rules.kinds.assign(TileSampleCount, render::LightKind::Solid);
    rules.kinds[TileAir] = render::LightKind::Open;
    rules.kinds[TileWater] = render::LightKind::Dense;
    rules.kinds[TileDeepWater] = render::LightKind::Dense;
    rules.sky = true;
    rules.sky_color = {1.0f, 0.98f, 0.92f, 1.0f};
    rules.ambient = {0.04f, 0.04f, 0.06f, 1.0f};
    // Gold glints faintly in the dark.
    rules.glow.assign(TileSampleCount, Color{0, 0, 0, 1});
    rules.glow[TileGold] = {0.35f, 0.28f, 0.05f, 1.0f};
    return rules;
}

render::LightRules top_down_light_rules(bool night) {
    render::LightRules rules;
    rules.blocking_layer = 1;
    rules.kinds.assign(TileSampleCount, render::LightKind::Open);
    rules.kinds[TileTree] = render::LightKind::Dense;
    rules.kinds[TileRock] = render::LightKind::Solid;
    rules.ambient = night ? Color{0.10f, 0.12f, 0.22f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 1.0f};
    return rules;
}

} // namespace forge::demo
