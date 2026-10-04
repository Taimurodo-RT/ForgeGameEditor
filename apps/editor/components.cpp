#include "components.h"

#include "demo_art.h"

using namespace forge::editor_app;

FORGE_REFLECT(forge::editor_app::Transform, 1) {
    t.field("position", &Transform::position).label("Позиция");
    t.field("rotation", &Transform::rotation).range(-360, 360).label("Поворот");
    t.field("scale", &Transform::scale).label("Масштаб");
}

FORGE_REFLECT(forge::editor_app::Picture, 1) {
    t.value("Critter", Picture::Critter);
    t.value("Crate", Picture::Crate);
    t.value("Ball", Picture::Ball);
    t.value("Leaf", Picture::Leaf);
    t.value("Glow", Picture::Glow);
    t.value("Drill", Picture::Drill);
    t.value("Furnace", Picture::Furnace);
    t.value("Chest", Picture::Chest);
}

FORGE_REFLECT(forge::editor_app::Look, 1) {
    t.field("picture", &Look::picture).label("Картинка");
    t.field("variant", &Look::variant).range(0, 7).label("Вариант");
    t.field("tint", &Look::tint).label("Оттенок");
    t.field("layer", &Look::layer).range(0, 15).label("Слой");
    t.field("visible", &Look::visible).label("Видим");
}

FORGE_REFLECT(forge::editor_app::Mover, 1) {
    t.field("velocity", &Mover::velocity).label("Скорость");
    t.field("spin", &Mover::spin).range(-720, 720).label("Вращение");
    t.field("bounce_radius", &Mover::bounce_radius).range(0, 100).label("Радиус");
}

namespace forge::editor_app {

u32 sprite_frame(const Look& look, f64 time) {
    switch (look.picture) {
    case Picture::Critter: {
        const u32 step = static_cast<u32>(time * 4.0) & 1u;
        return (look.variant % demo::kCritterKinds) * 2 + step;
    }
    case Picture::Crate: return demo::kFrameCrate;
    case Picture::Ball: return demo::kFrameBall;
    case Picture::Leaf: return demo::kFrameLeaf;
    case Picture::Glow: return demo::kFrameGlow;
    case Picture::Drill: return demo::kFrameDrill;
    case Picture::Furnace: return demo::kFrameFurnace;
    case Picture::Chest: return demo::kFrameChest;
    }
    return demo::kFrameCrate;
}

} // namespace forge::editor_app
