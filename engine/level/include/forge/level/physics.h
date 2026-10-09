#pragma once

// The «Физика» mode's edits that are not objects or tiles: the world's pull
// as an undoable command, and the flow trial: liquids and falling tiles run
// over the loaded part of a level in the editor, then go back exactly as
// they were.
//
//   FlowTrial trial;
//   trial.start(level, view);  // remembers every ready chunk
//   trial.update(dt);          // each frame; keep trial.keep() loaded
//   trial.reset();             // tiles and saved state as before the start
//
// The trial changes no history and nothing on disk: chunks put back count
// as saved again if they were saved before it.

#include "forge/editor/undo.h"
#include "forge/level/level.h"
#include "forge/sim/cells.h"
#include "forge/sim/tiles.h"
#include "forge/sim/zones.h"

#include <memory>
#include <string>
#include <vector>

namespace forge::level {

// Sets the level's physics; edits with the same label merge while the
// history is not sealed (a slider's drag is one entry).
class SetPhysics final : public editor::Command {
public:
    SetPhysics(Level& level, LevelPhysics before, LevelPhysics after, std::string label);
    void apply(editor::Document&) override { level_.set_physics(after_); }
    void revert(editor::Document&) override { level_.set_physics(before_); }
    std::string label() const override { return label_; }
    std::string merge_key() const override { return "physics:" + label_; }
    bool try_merge(const editor::Command& next) override;

private:
    Level& level_;
    LevelPhysics before_, after_;
    std::string label_;
};

class FlowTrial {
public:
    FlowTrial() = default;
    ~FlowTrial();
    FlowTrial(const FlowTrial&) = delete;
    FlowTrial& operator=(const FlowTrial&) = delete;

    // Starts over the chunks of the level that are ready now, the zones
    // around focus (tiles: the view) awake; liquids and sand fall along the
    // level's world pull (sim::cells_down). False when the game has no
    // liquids (LevelModule::make_cells) or nothing is loaded; *error says why.
    bool start(Level& level, const world::Rect& focus, std::string* error = nullptr);
    // The ticks due for dt seconds: 60 a second, at most 4 a call.
    u32 update(f64 dt);
    // Puts every remembered chunk back. False when one was no longer loaded
    // (keep() was not kept loaded): what could be put back is.
    bool reset();

    bool running() const { return level_ != nullptr; }
    // Tiles that must stay loaded while it runs: every chunk it remembered.
    const world::Rect& keep() const { return keep_; }
    // No world pull: nothing falls.
    bool still() const { return down_ == ~0u; }
    u32 down() const { return down_; }
    u64 ticks() const { return tick_; }
    u32 chunks() const { return static_cast<u32>(saved_.size()); }
    // Cells that changed in the last tick.
    u32 moved() const { return cells_ ? cells_->stats().moved : 0; }

private:
    struct Saved {
        world::ChunkCoord coord;
        std::vector<world::TileId> tiles;
        u32 revision = 0;
        bool edited = false;
        bool clean = false; // matched the saved level
        u32 saved_revision = 0;
    };
    Level* level_ = nullptr;
    std::unique_ptr<sim::CellSim> cells_;
    sim::CollisionRules rules_;
    sim::Zones zones_;
    world::Rect focus_{}, keep_{};
    std::vector<Saved> saved_;
    u32 down_ = ~0u;
    u64 tick_ = 0;
    f64 due_ = 0;
};

} // namespace forge::level
