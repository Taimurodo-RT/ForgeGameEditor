#pragma once

// The rules of a «Три в ряд» board (step 14.3b, 14.3-три-в-ряд-модель.md, «Правила поля»): no window, no files, no
// clock, so every rule is tested on its own (tests/test_match3.cpp) and the game only animates what they did.
//
// A board is cells in rows from the top; a cell holds a gem (its number in the game's set of gems), no gem (between a
// removal and the fill), or is a hole (no cell: nothing is there, gems fall past it). As in CS50's «Match 3» (lecture
// 3), finding rows, removing them, falling and filling are separate steps; Forge adds what CS50 has no answer for:
// groups of rows that cross, holes, a generator the saves keep, and a board that cannot be played.
//
//   Play play;
//   if (!play.start(shape, gems, scoring, rules, "Первый", why)) ...  // the shape with its holes and preset gems
//   play.on_event = [&](const Event& e) { ... };                      // «собирает» links: play.add_moves(2)
//   if (play.swap(a, b) == Swap::Done)
//       for (Step s; play.step(s);) animate(s);                       // till the board is calm, then the end or a shuffle

#include "forge/core/types.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace match3 {

using forge::f32;
using forge::f64;
using forge::i32;
using forge::i64;
using forge::u32;
using forge::u64;
using forge::u8;
using forge::usize;

// The biggest board, each way.
inline constexpr i32 kMaxSide = 10;
// How many different gems a board plays with.
inline constexpr u32 kMinGems = 3, kMaxGems = 8;
// How many times a start or a shuffle tries before it gives up: there is no endless trying again.
inline constexpr u32 kTries = 100;

// A cell with no gem (removed, not filled yet), and no cell at all.
inline constexpr u8 kNone = 0xfe;
inline constexpr u8 kHole = 0xff;

// The board's own generator (xorshift, 64 bits of state): the same seed, the same gems, and the saves keep it.
struct Rng {
    u64 state = 0;
    u64 calls = 0; // how many numbers it gave (a test sees that trying ends)

    explicit Rng(u64 seed = 0);
    u64 next();
    // 0 … n-1 (n > 0).
    u32 below(u32 n);
};

struct Board {
    i32 w = 0, h = 0;
    std::vector<u8> cells; // row by row from the top

    Board() = default;
    Board(i32 width, i32 height, u8 fill = kNone);
    bool inside(i32 x, i32 y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    i32 index(i32 x, i32 y) const { return y * w + x; }
    // Outside the board is a hole.
    u8 at(i32 x, i32 y) const { return inside(x, y) ? cells[static_cast<std::size_t>(index(x, y))] : kHole; }
    void set(i32 x, i32 y, u8 v) { cells[static_cast<std::size_t>(index(x, y))] = v; }
    bool cell(i32 x, i32 y) const { return at(x, y) != kHole; }
    bool gem(i32 x, i32 y) const { return at(x, y) < kNone; }
    i32 x_of(i32 i) const { return i % w; }
    i32 y_of(i32 i) const { return i / w; }
    // Cells that are not holes.
    u32 cell_count() const;
    bool operator==(const Board&) const = default;
};

// A row: three or more of one gem one after another, across (a row of the board) or down (a column).
struct Run {
    u8 gem = 0;
    i32 x = 0, y = 0; // its first cell (leftmost, topmost)
    i32 length = 0;
    bool across = true;
};
// The rows of the board: the rows of the board first (from the top, left to right), then its columns (from the left,
// top to bottom). A row is found across or down, never along a diagonal.
std::vector<Run> find_runs(const Board& board);

// Rows of one gem that share a cell («Г», «Т», «+»): one group, one event «match». A cell where they cross is the
// group's once. Two rows of one gem that do not touch are two groups.
struct Group {
    u8 gem = 0;
    std::vector<i32> cells; // cell indices, ascending, each once
    i32 longest = 0;        // its longest row
};
// In the order of their top left cell (by row, then column): with one seed the result does not depend on how the board
// was walked.
std::vector<Group> find_groups(const Board& board);

// What a group scores: the points of its cells (once each), times the bonus for its longest row (3: ×1, 4: four, 5
// and more: five), times the chain's (step 1: ×1, each next step of a cascade +chain), rounded down.
struct Scoring {
    std::vector<u32> points; // by gem number
    f64 four = 1.5, five = 2.0, chain = 0.5;
};
u64 group_points(const Group& group, const Scoring& scoring, u32 step);

// The groups' cells left without a gem.
void remove_groups(Board& board, const std::vector<Group>& groups);

// A gem falling: from a cell to a cell below it in its column (holes between them are passed).
struct Fall {
    i32 from = 0, to = 0;
    u8 gem = 0;
};
// Every gem falls down its column into the lowest cells without a gem, past holes, keeping the order of the column.
std::vector<Fall> fall(Board& board);
// A new gem in a cell: it comes into the column from above, `above` places over the column's top cell.
struct Drop {
    i32 cell = 0;
    u8 gem = 0;
    i32 above = 1;
};
// Every cell without a gem gets a new one from the generator (the top cells of their columns are the ones left empty
// by fall()); column by column from the left, each from the bottom up.
std::vector<Drop> fill(Board& board, Rng& rng, u32 gems);

bool neighbours(const Board& board, i32 a, i32 b);
// Swapping the gems of two neighbouring cells makes a row through one of them.
bool swap_makes_run(const Board& board, i32 a, i32 b);
// A swap that makes a row: the first by cell (with its right, then its lower neighbour); none: no move on the board.
std::optional<std::pair<i32, i32>> find_move(const Board& board);
// The shape alone (cells and holes, whatever gems) allows a move: somewhere three cells one after another in a line and
// a cell beside one of them to swap with. Without that no fill ever has a move.
bool shape_can_move(const Board& shape);

// The cells «А1»-style for the author: «3:1, 4:1, 5:1» (column:row from 1).
std::string cells_words(const Board& board, const std::vector<i32>& cells);

// Fills every cell without a gem so that no row stands and a move exists, keeping the gems already there (an author's
// presets). Tries kTries times; false (the board as it was, why in words): presets already in a row, a shape with no
// move, fewer than kMinGems or more than kMaxGems gems, or no fill found. name: the level's, for the words.
bool fill_start(Board& board, Rng& rng, u32 gems, const std::string& name, std::string& why);

// A calm board with no move gets one: its gems change places (the same gems, the same cells, holes stay) up to kTries
// times; then it is filled anew from the generator up to kTries times. False: neither worked (the board as it was).
struct Shuffle {
    bool done = false;
    bool refilled = false; // the gems are new, not the old ones moved
    u32 tries = 0;
};
Shuffle shuffle(Board& board, Rng& rng, u32 gems);

// --- a level being played -----------------------------------------------------------------------------------------

struct Rules {
    u32 moves = 20;   // «Ходов»
    u64 target = 1000; // «Цель, очков»
    u64 seed = 0;     // «Зерно»; 0: the game picks one when a new game starts
    f64 four = 1.5, five = 2.0, chain = 0.5;
};

enum class End : u8 { None, Won, Lost };

struct Event {
    enum class Kind : u8 {
        Match,    // a group of one gem: gem, cells, points (already in the score), step
        Target,   // the score reached the target: the level is won
        MovesOut, // no moves left and the target not reached: the level is lost
        NoMoves,  // no move on the board, and neither a shuffle nor a new fill made one: the level is lost
        Shuffled, // no move on the board: the gems changed places (refilled: new ones)
    };
    Kind kind = Kind::Match;
    u8 gem = 0;
    u32 cells = 0;
    u64 points = 0;
    u32 step = 0;
    bool refilled = false;
};

// One step of a cascade, as it happened, for the game to show.
struct Step {
    u32 number = 0; // 1, 2, … within the move
    std::vector<Group> groups;
    std::vector<u64> points; // each group's
    std::vector<Fall> falls;
    std::vector<Drop> drops;
};

enum class Swap : u8 {
    Done,    // a row came of it: a move spent, the cascade is under way
    NoRun,   // no row: the gems go back, no move spent
    Refused, // the level is over, a cascade is under way, not neighbours, or not two gems
};

class Play {
public:
    // Starts the level on shape (its holes, and the gems an author put there before: they stay where they are). False
    // (why, in words): too few or too many gems, presets in a row, a shape with no move, no fill in kTries.
    bool start(const Board& shape, u32 gems, const Scoring& scoring, const Rules& rules, const std::string& name,
               std::string& why);
    // Goes on from what a save kept (a calm board).
    struct Saved {
        Board board;
        u64 score = 0;
        u32 moves = 0;
        End end = End::None;
        u64 rng = 0;
        u32 shuffles = 0;
    };
    bool resume(const Saved& saved, u32 gems, const Scoring& scoring, const Rules& rules, std::string& why);
    Saved saved() const;

    Swap swap(i32 a, i32 b);
    // A cascade is under way: step() has more to do.
    bool busy() const { return busy_; }
    // The next step of the cascade: rows found, scored and said (Event::Match, the links answer at once), removed,
    // fallen, filled. False when the board is calm: then, once a move, the target (won), else no moves left (lost),
    // else no move on the board (a shuffle, or lost when none works). Nothing after the level ended.
    bool step(Step& out);

    // What links do (only while the level goes on).
    void add_moves(i32 n);
    void add_score(i64 n);

    std::function<void(const Event&)> on_event;

    const Board& board() const { return board_; }
    u64 score() const { return score_; }
    u32 moves() const { return moves_; }
    u64 target() const { return rules_.target; }
    End end() const { return end_; }
    u32 shuffles() const { return shuffles_; }
    const Rng& rng() const { return rng_; }
    u32 gems() const { return gems_; }

    // What a calm board means once a move is over: the target, else no moves left, else no move on the board (a
    // shuffle). step() calls it when the cascade ends; a test calls it on a resumed board.
    void settle();

private:
    void said(const Event& e) {
        if (on_event) on_event(e);
    }

    Board board_;
    Rng rng_;
    Scoring scoring_;
    Rules rules_;
    u32 gems_ = 0;
    u64 score_ = 0;
    u32 moves_ = 0;
    u32 step_ = 0;
    u32 shuffles_ = 0;
    End end_ = End::None;
    bool busy_ = false;
};

} // namespace match3
