#pragma once

// Painting tiles: the shapes a brush covers, and a stroke that remembers
// what it changed so one Ctrl+Z takes the whole stroke back.
//
//   TileStroke* stroke = new TileStroke(level, "Кисть «Камень»");
//   stroke->paint_disc(layer, x, y, radius, value);   // while the mouse moves
//   history.execute(std::unique_ptr<Command>(stroke)); // on release
//
// A stroke applies as it goes (the author sees it at once); executing it
// afterwards only records it. Undo and redo load the area first, so a stroke
// made far from the view still comes back.

#include "forge/editor/undo.h"
#include "forge/level/level.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace forge::level {

struct Cell {
    i32 x, y;
    friend bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }
};

// Cells of a filled circle of the given radius (0: one cell) around (x, y).
void disc_cells(i32 x, i32 y, i32 radius, std::vector<Cell>& out);
// Cells on the straight line between two cells, both ends included.
void line_cells(i32 x0, i32 y0, i32 x1, i32 y1, std::vector<Cell>& out);
// Cells of the rectangle with these corners (any order); outline only, or filled.
void rect_cells(i32 x0, i32 y0, i32 x1, i32 y1, bool filled, std::vector<Cell>& out);

// Cells of the area around (x, y) holding the same value on the layer,
// joined by sides, within loaded chunks. Stops at limit cells; *capped
// tells whether it did.
void fill_cells(const Level& level, u32 layer, i32 x, i32 y, usize limit, std::vector<Cell>& out,
                bool* capped = nullptr);

class TileStroke final : public editor::Command {
public:
    TileStroke(Level& level, std::string label);

    // Paints and remembers the cells' old values. Cells already painted in
    // this stroke keep their first old value. Returns cells changed.
    usize paint(u32 layer, std::span<const Cell> cells, world::TileId value);
    usize paint_disc(u32 layer, i32 x, i32 y, i32 radius, world::TileId value);

    void apply(editor::Document& doc) override;
    void revert(editor::Document& doc) override;
    std::string label() const override { return label_; }

    usize size() const { return changes_.size(); }
    bool empty() const { return changes_.empty(); }
    // Tiles touched, for loading before undo and redo.
    const world::Rect& bounds() const { return bounds_; }

private:
    struct Change {
        i32 x, y;
        u32 layer;
        world::TileId before, after;
    };
    struct Key {
        u32 layer;
        i32 x, y;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const {
            u64 h = (static_cast<u64>(static_cast<u32>(k.x)) << 32 | static_cast<u32>(k.y)) * 0x9e3779b97f4a7c15ull;
            return static_cast<usize>(h ^ (h >> 29) ^ k.layer);
        }
    };
    void set_all(bool after);

    Level& level_;
    std::string label_;
    std::vector<Change> changes_;
    std::unordered_map<Key, usize, KeyHash> index_;
    world::Rect bounds_{};
    std::vector<Cell> scratch_;
};

} // namespace forge::level
