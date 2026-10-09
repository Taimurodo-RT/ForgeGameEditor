#pragma once

// The «Свет» mode's edits that are not objects: the level's time of day as an
// undoable command. Light sources are objects of the level (LightSource),
// placed, moved and changed like any other.

#include "forge/editor/undo.h"
#include "forge/level/level.h"

#include <string>

namespace forge::level {

// Sets the level's time of day; edits with the same label merge while the
// history is not sealed (a slider's drag is one entry).
class SetLight final : public editor::Command {
public:
    SetLight(Level& level, LevelLight before, LevelLight after, std::string label);
    void apply(editor::Document&) override { level_.set_light(after_); }
    void revert(editor::Document&) override { level_.set_light(before_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return "light:" + label_; }
    bool try_merge(const editor::Command& next) override;

private:
    Level& level_;
    LevelLight before_, after_;
    std::string label_;
};

// Adds the light sources of a scene (those loaded now) to a frame's lights:
// the game and the editor's «как в игре» light them the same way.
void add_light_sources(scene::Scene& scene, render::LightRenderer& lights);

// An hour as a clock shows it: "18:30" (to the minute).
std::string clock_text(f64 hours);
// "18:30", "18" or "18.5" (a comma too) as hours; false when it is not one of
// those or not finite. The range is the caller's to check.
bool parse_clock(const std::string& text, f64& hours);

} // namespace forge::level
