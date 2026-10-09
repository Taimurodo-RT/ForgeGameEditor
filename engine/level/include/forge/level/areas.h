#pragma once

// The «Зоны» mode's edits and what a game does with a level's areas: the
// music of the place a point is in, who came in and went out (as a trigger
// reports it), the areas and spawn point as an undoable command. The data
// (Area, LevelAreas, areas.json) is in level.h.

#include "forge/editor/undo.h"
#include "forge/level/level.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace forge::level {

// The area whose music plays at a point: of the areas there that have music,
// the smallest; of equal ones, the later in the list. Null: none.
const Area* music_area(const LevelAreas& a, f64 x, f64 y);

// Who came in and went out, as a trigger reports it: a point (the hero's
// centre) checked once a step against the areas.
struct AreaEvent {
    u64 area = 0;
    bool entered = false; // false: left
};

class AreaWatch {
public:
    // Inside nothing: the areas the point is in come in at the next step (a
    // new game, a hero placed anew).
    void clear() { inside_.clear(); }
    // The areas the point is in now count as entered already, with no events
    // (a loaded save: the hero was in them when it was saved).
    void settle(const LevelAreas& a, f64 x, f64 y);
    // The point now: the areas it left, then the ones it came into, each in
    // list order. Staying in an area says nothing.
    void step(const LevelAreas& a, f64 x, f64 y, std::vector<AreaEvent>& out);
    bool inside(u64 id) const;
    const std::vector<u64>& inside() const { return inside_; }

private:
    std::vector<u64> inside_; // in list order
};

// Sets the level's areas and spawn point. Edits with the same non-empty
// merge key merge while the history is not sealed (a name typed letter by
// letter: "name:" and the area's id).
class SetAreas final : public editor::Command {
public:
    SetAreas(Level& level, LevelAreas before, LevelAreas after, std::string label, std::string merge = {});
    void apply(editor::Document&) override;
    void revert(editor::Document&) override;
    std::string label() const override { return label_; }
    std::string merge_key() const override { return merge_.empty() ? std::string() : "areas:" + merge_; }
    bool try_merge(const editor::Command& next) override;

private:
    Level& level_;
    LevelAreas before_, after_;
    std::string label_, merge_;
};

} // namespace forge::level
