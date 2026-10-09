#include "slice_art.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slice {

using namespace forge;
using namespace forge::world;

namespace {

u32 h3(u32 a, u32 b, u32 c) {
    u32 h = a * 374761393u + b * 668265263u + c * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct Rgb {
    u8 r, g, b;
};

Rgb shade(Rgb c, i32 d) {
    auto f = [d](u8 v) { return static_cast<u8>(std::clamp(static_cast<i32>(v) + d, 0, 255)); };
    return {f(c.r), f(c.g), f(c.b)};
}

// Writes pixels into an RGBA image of the given width.
struct Canvas {
    std::vector<u8>& px;
    u32 width;
    i32 ox = 0, oy = 0, w = 16, h = 16;
    void put(i32 x, i32 y, Rgb c, u8 a = 255) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        u8* p = &px[(static_cast<usize>(oy + y) * width + static_cast<usize>(ox + x)) * 4];
        p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = a;
    }
    void rect(i32 x0, i32 y0, i32 x1, i32 y1, Rgb c, u8 a = 255) const {
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) put(x, y, c, a);
    }
};

// A villager, 16 × 32, facing right. step: 0 standing, 1 and 2 walking, 3 jumping.
struct Look {
    Rgb skin, hair, shirt, pants, boots;
    Rgb hat{0, 0, 0};
    bool helmet = false, beard = false, apron = false, scarf = false;
};

void person(const Canvas& c, const Look& l, u32 step) {
    // Legs.
    const i32 lf = step == 1 ? -1 : (step == 2 ? 1 : 0);
    const i32 bend = step == 3 ? -2 : 0;
    c.rect(5 + lf, 21, 7 + lf, 27 + bend, l.pants);
    c.rect(8 - lf, 21, 10 - lf, 27 + bend, shade(l.pants, -18));
    c.rect(5 + lf, 28 + bend, 7 + lf + 1, 30 + bend, l.boots);
    c.rect(8 - lf, 28 + bend, 10 - lf + 1, 30 + bend, shade(l.boots, -10));
    // Body.
    c.rect(4, 11, 11, 21, l.shirt);
    c.rect(4, 11, 5, 21, shade(l.shirt, -25));
    c.rect(4, 20, 11, 21, shade(l.pants, -30)); // belt
    if (l.apron) c.rect(6, 13, 11, 23, Rgb{90, 62, 40});
    if (l.scarf) c.rect(5, 10, 11, 12, Rgb{196, 58, 52});
    // Arm swinging with the step.
    const i32 arm = step == 1 ? 1 : (step == 2 ? -1 : 0);
    c.rect(8 + arm, 12, 9 + arm, 18, shade(l.shirt, -12));
    c.rect(8 + arm, 19, 9 + arm, 20, l.skin);
    // Head.
    for (i32 y = 2; y <= 10; ++y)
        for (i32 x = 4; x <= 11; ++x) {
            const f32 dx = (static_cast<f32>(x) - 7.5f) / 4.0f, dy = (static_cast<f32>(y) - 6.0f) / 4.5f;
            if (dx * dx + dy * dy <= 1.0f) c.put(x, y, dx < -0.5f ? shade(l.skin, -20) : l.skin);
        }
    c.put(10, 6, {30, 24, 24}); // eye
    c.put(11, 8, shade(l.skin, -40)); // mouth
    if (l.helmet) {
        c.rect(3, 1, 12, 3, l.hat);
        c.rect(4, 0, 11, 0, shade(l.hat, 20));
        c.rect(11, 2, 12, 3, {255, 250, 200}); // lamp
    } else {
        c.rect(4, 1, 11, 2, l.hair);
        c.rect(4, 3, 5, 6, l.hair);
    }
    if (l.beard) {
        c.rect(7, 8, 11, 10, l.hair);
        c.rect(8, 11, 10, 12, l.hair);
    }
}

} // namespace

bool is_solid(TileId t) {
    switch (t) {
    case TileGrass: case TileDirt: case TileStone: case TileSand: case TileCopper: case TileIron: case TileGold:
    case TilePlanks: case TileRoof: case TileBrick: case TileDoor: return true;
    default: return false;
    }
}

f32 hand_time(TileId t) {
    switch (t) {
    case TileGrass: case TileDirt: return 0.45f;
    case TileSand: return 0.3f;
    case TilePlanks: return 0.8f;
    default: return 0.0f;
    }
}

f32 pickaxe_time(TileId t) {
    switch (t) {
    case TileGrass: case TileDirt: return 0.2f;
    case TileSand: return 0.15f;
    case TilePlanks: return 0.35f;
    case TileStone: case TileBrick: return 0.5f;
    case TileCopper: return 0.7f;
    case TileIron: return 0.9f;
    case TileGold: return 1.1f;
    case TileRoof: return 0.4f;
    default: return 0.0f;
    }
}

const char* drop_of(TileId t) {
    switch (t) {
    case TileGrass: case TileDirt: return "dirt";
    case TileStone: case TileBrick: return "stone";
    case TileSand: return "sand";
    case TileCopper: return "copper";
    case TileIron: return "iron";
    case TileGold: return "gold";
    case TilePlanks: case TileRoof: return "wood";
    default: return "";
    }
}

std::vector<u8> make_atlas() {
    std::vector<u8> px = demo::make_tile_atlas();
    const u32 size = demo::kTileCellPx * demo::kTileCells;
    auto cell = [&](TileId id) {
        return Canvas{px, size, static_cast<i32>((id % demo::kTileCells) * demo::kTileCellPx),
                      static_cast<i32>((id / demo::kTileCells) * demo::kTileCellPx), 16, 16};
    };
    // Boards: horizontal planks with seams and nails.
    {
        const Canvas c = cell(TilePlanks);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const i32 row = y / 4;
                Rgb col = shade(Rgb{168, 116, 66}, static_cast<i32>(h3(static_cast<u32>(x / 3), static_cast<u32>(y), 17) % 13) - 6 - row * 3);
                if (y % 4 == 3) col = Rgb{96, 62, 34};
                if ((x + row * 7) % 16 == 0) col = Rgb{110, 72, 40};
                if (y % 4 == 1 && (x == 2 + (row % 2) * 8 || x == 13 - (row % 2) * 8)) col = Rgb{70, 64, 60};
                c.put(x, y, col);
            }
    }
    // Background boards: vertical, darker.
    auto plank_wall = [&](const Canvas& c) {
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                Rgb col = shade(Rgb{120, 84, 52}, static_cast<i32>(h3(static_cast<u32>(x / 4), static_cast<u32>(y / 3), 18) % 11) - 5);
                if (x % 4 == 3) col = Rgb{72, 48, 30};
                c.put(x, y, col);
            }
    };
    plank_wall(cell(TilePlankWall));
    // Torch on boards: the stick (the flame is a sprite, so it is not dimmed).
    {
        const Canvas c = cell(TileTorch);
        plank_wall(c);
        c.rect(7, 6, 8, 14, {92, 60, 34});
        c.rect(6, 4, 9, 6, {60, 40, 24});
    }
    // Beam: a post over boards.
    {
        const Canvas c = cell(TileBeam);
        plank_wall(c);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 4; x <= 11; ++x) c.put(x, y, shade(Rgb{110, 74, 42}, x == 4 ? -30 : (x == 11 ? -40 : static_cast<i32>(h3(static_cast<u32>(x), static_cast<u32>(y / 4), 20) % 9) - 4)));
    }
    // Roof: overlapping shingles.
    {
        const Canvas c = cell(TileRoof);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const i32 row = y / 4;
                const i32 sx = (x + (row % 2) * 4) % 8;
                Rgb col = shade(Rgb{156, 62, 46}, -row * 4 + (sx == 0 ? -30 : 0) + (y % 4 == 3 ? -36 : 0));
                c.put(x, y, col);
            }
    }
    // Window: a frame, glass and a cross.
    {
        const Canvas c = cell(TileWindow);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                Rgb col = shade(Rgb{150, 196, 220}, (x + y < 10) ? 30 : 0);
                if (x < 2 || y < 2 || x > 13 || y > 13 || x == 7 || x == 8 || y == 7 || y == 8) col = Rgb{98, 66, 38};
                c.put(x, y, col);
            }
    }
    // Brick.
    {
        const Canvas c = cell(TileBrick);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const i32 row = y / 4;
                const bool mortar = y % 4 == 3 || (x + (row % 2) * 4) % 8 == 7;
                c.put(x, y, mortar ? Rgb{176, 168, 150} : shade(Rgb{160, 72, 52}, static_cast<i32>(h3(static_cast<u32>(x / 8 + row * 3), static_cast<u32>(row), 23) % 21) - 10));
            }
    }
    // TileDoor stays empty: the door object draws itself over its column.
    return px;
}

demo::SheetImage make_sheet() {
    constexpr u32 kCell = 18, kCols = 8, kRows = 9;
    const demo::SheetImage base = demo::make_sprite_sheet();
    demo::SheetImage img;
    img.width = kCell * kCols;
    img.height = kCell * kRows;
    img.rgba.assign(static_cast<usize>(img.width) * img.height * 4, 0);
    // The demo sheet has the same width: its rows go on top unchanged.
    std::memcpy(img.rgba.data(), base.rgba.data(), std::min(base.rgba.size(), img.rgba.size()));
    img.frames = base.frames;
    img.frames.resize(FrameHero);

    // People: rows 5 and 6, one column each, 16 × 32.
    auto tall = [&](u32 column) {
        return Canvas{img.rgba, img.width, static_cast<i32>(column * kCell + 1), static_cast<i32>(5 * kCell + 1), 16, 32};
    };
    const Look hero{{236, 188, 150}, {96, 60, 36}, {60, 104, 176}, {64, 60, 70}, {70, 46, 30}, {}, false, false, false, true};
    const Look miner{{226, 176, 140}, {150, 150, 150}, {150, 110, 60}, {80, 70, 60}, {50, 40, 30}, {226, 186, 52}, true, true, false, false};
    const Look smith{{210, 160, 120}, {40, 34, 30}, {170, 70, 50}, {60, 50, 46}, {40, 30, 24}, {}, false, true, true, false};
    for (u32 s = 0; s < 4; ++s) person(tall(s), hero, s);
    person(tall(4), miner, 1);
    person(tall(5), miner, 2);
    person(tall(6), smith, 1);
    person(tall(7), smith, 2);
    for (u32 i = 0; i < 8; ++i) img.frames.push_back({i * kCell + 1, 5 * kCell + 1, 16, 32});

    // Items and effects: rows 7 and 8.
    auto small = [&](u32 f) {
        const u32 i = f - FramePickaxe;
        return Canvas{img.rgba, img.width, static_cast<i32>((i % kCols) * kCell + 1), static_cast<i32>((7 + i / kCols) * kCell + 1), 16, 16};
    };
    {
        const Canvas c = small(FramePickaxe);
        for (i32 i = 0; i < 12; ++i) c.rect(3 + i, 13 - i, 4 + i, 13 - i, shade(Rgb{140, 96, 54}, i % 3 == 0 ? -20 : 0)); // handle
        for (i32 x = 2; x <= 14; ++x) {
            const f32 t = (static_cast<f32>(x) - 8.0f) / 6.0f;
            const i32 y = 3 + static_cast<i32>(t * t * 4.0f);
            c.rect(x, y - 1, x, y + 1, shade(Rgb{170, 176, 186}, static_cast<i32>(-t * 30)));
        }
    }
    {
        const Canvas c = small(FrameCoins);
        auto coin = [&](f32 cx, f32 cy) {
            for (i32 y = 0; y < 16; ++y)
                for (i32 x = 0; x < 16; ++x) {
                    const f32 dx = static_cast<f32>(x) - cx, dy = (static_cast<f32>(y) - cy) * 1.6f;
                    const f32 d = std::sqrt(dx * dx + dy * dy);
                    if (d < 4.5f) c.put(x, y, d > 3.4f ? Rgb{176, 120, 30} : shade(Rgb{246, 200, 64}, static_cast<i32>((-dx - dy) * 6)));
                }
        };
        coin(6, 11);
        coin(10, 8);
        coin(6, 5);
    }
    {
        const Canvas c = small(FrameDust);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 7.5f;
                const f32 a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / 7.5f, 0.0f, 1.0f);
                c.put(x, y, {255, 255, 255}, static_cast<u8>(std::sqrt(a) * 255.0f));
            }
    }
    small(FrameSpark).rect(5, 5, 10, 10, {255, 255, 255});
    {
        const Canvas c = small(FrameWood);
        for (i32 i = 0; i < 3; ++i) {
            c.rect(2, 4 + i * 3, 13, 5 + i * 3, shade(Rgb{168, 116, 66}, -i * 12));
            c.put(2, 4 + i * 3, {120, 80, 44});
            c.put(2, 5 + i * 3, {120, 80, 44});
        }
    }
    auto lump = [&](u32 f, Rgb col, u32 salt) {
        const Canvas c = small(f);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const f32 dx = static_cast<f32>(x) - 7.5f, dy = (static_cast<f32>(y) - 9.0f) * 1.3f;
                if (std::sqrt(dx * dx + dy * dy) < 6.0f + static_cast<f32>(h3(static_cast<u32>(x), static_cast<u32>(y), salt) % 3) - 1.0f)
                    c.put(x, y, shade(col, static_cast<i32>((-dx - dy) * 4) + static_cast<i32>(h3(static_cast<u32>(x / 2), static_cast<u32>(y / 2), salt) % 15) - 7));
            }
    };
    lump(FrameStone, {130, 130, 138}, 1);
    lump(FrameDirt, {128, 88, 54}, 2);
    lump(FrameSand, {220, 198, 132}, 3);
    {
        const Canvas c = small(FrameTorch);
        c.rect(7, 6, 8, 15, {110, 74, 40});
        c.rect(6, 3, 9, 6, {255, 180, 60});
        c.rect(7, 1, 8, 3, {255, 240, 150});
    }
    {
        const Canvas c = small(FrameAnvil);
        c.rect(1, 5, 14, 8, {84, 86, 94});
        c.rect(1, 5, 14, 5, {130, 132, 140});
        c.rect(0, 6, 1, 7, {84, 86, 94});
        c.rect(5, 9, 10, 11, {70, 72, 80});
        c.rect(3, 12, 12, 15, {60, 62, 70});
    }
    {
        const Canvas c = small(FrameFlame);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const f32 dx = (static_cast<f32>(x) - 7.5f) / 4.0f;
                const f32 t = static_cast<f32>(y) / 15.0f; // 0 top
                const f32 half = t * t * 1.0f + 0.05f;
                if (std::fabs(dx) > half * (1.0f + t)) continue;
                const Rgb col = std::fabs(dx) < half * 0.5f && t > 0.4f ? Rgb{255, 246, 190} : Rgb{255, 150, 40};
                c.put(x, y, col, static_cast<u8>(200 + 55 * t));
            }
    }
    {
        const Canvas c = small(FrameDrop);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const f32 dx = static_cast<f32>(x) - 7.5f, dy = static_cast<f32>(y) - 9.0f;
                const f32 d = std::sqrt(dx * dx + dy * dy);
                if (d < 4.5f || (dy < 0 && std::fabs(dx) < (dy + 9.0f) * 0.5f)) c.put(x, y, {255, 255, 255});
            }
    }
    {
        // Key: a ring, a shaft and two teeth.
        const Canvas c = small(FrameKey);
        const Rgb gold{232, 186, 64}, dark{160, 116, 30};
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                const f32 dx = static_cast<f32>(x) - 4.5f, dy = static_cast<f32>(y) - 7.5f;
                const f32 d = std::sqrt(dx * dx + dy * dy);
                if (d < 3.6f && d > 1.6f) c.put(x, y, d > 2.8f ? dark : gold);
            }
        c.rect(8, 7, 14, 8, gold);
        c.rect(8, 8, 14, 8, dark);
        c.rect(11, 9, 11, 11, gold);
        c.rect(13, 9, 14, 10, gold);
    }
    // Door: vertical boards in a frame with iron bands; a closed door is a
    // column of these.
    {
        const Canvas c = small(FrameDoor);
        for (i32 y = 0; y < 16; ++y)
            for (i32 x = 0; x < 16; ++x) {
                Rgb col = shade(Rgb{132, 86, 46}, static_cast<i32>(h3(static_cast<u32>(x / 4), static_cast<u32>(y / 5), 31) % 11) - 5);
                if (x % 4 == 3) col = Rgb{86, 54, 28};
                if (x < 2 || x > 13) col = Rgb{74, 46, 24};
                if (y == 3 || y == 12) col = Rgb{70, 72, 80};
                c.put(x, y, col);
            }
    }
    for (u32 f = FramePickaxe; f < FrameCount; ++f) {
        const u32 i = f - FramePickaxe;
        img.frames.push_back({(i % kCols) * kCell + 1, (7 + i / kCols) * kCell + 1, 16, 16});
    }
    return img;
}

render::LightRules light_rules() {
    render::LightRules rules = demo::side_view_light_rules();
    rules.kinds.resize(TileSliceCount, render::LightKind::Open);
    rules.glow.resize(TileSliceCount, Color{0, 0, 0, 1});
    for (TileId t = 1; t < TileSliceCount; ++t)
        rules.kinds[t] = is_solid(t) ? render::LightKind::Solid : render::LightKind::Open;
    rules.kinds[TileWater] = render::LightKind::Dense;
    rules.kinds[TileDeepWater] = render::LightKind::Dense;
    rules.ambient = {0.05f, 0.05f, 0.07f, 1.0f};
    return rules;
}

std::span<const render::SkyKey> day_sky() {
    static const std::vector<render::SkyKey> keys = [] {
        const Color day = light_rules().sky_color; // the light the game always had
        const Color night{0.10f, 0.12f, 0.22f, 1.0f};
        return std::vector<render::SkyKey>{{4.5f, night},
                                           {6.5f, Color{0.85f, 0.62f, 0.55f, 1.0f}},
                                           {8.0f, day},
                                           {17.0f, day},
                                           {19.0f, Color{0.95f, 0.58f, 0.38f, 1.0f}},
                                           {21.0f, night}};
    }();
    return keys;
}

Color sky_light(f64 hour) { return render::sky_at(day_sky(), hour, light_rules().sky_color); }

} // namespace slice
