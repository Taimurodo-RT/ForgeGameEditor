#include "match3_rules.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace match3 {

// --- the generator ------------------------------------------------------------------------------------------------

Rng::Rng(u64 seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}

u64 Rng::next() {
    ++calls;
    u64 x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state = x;
    return x;
}

u32 Rng::below(u32 n) { return n ? static_cast<u32>(next() % n) : 0; }

// --- the board ----------------------------------------------------------------------------------------------------

Board::Board(i32 width, i32 height, u8 fill) : w(width), h(height), cells(static_cast<std::size_t>(width * height), fill) {}

u32 Board::cell_count() const {
    u32 n = 0;
    for (u8 c : cells) n += c != kHole;
    return n;
}

std::vector<Run> find_runs(const Board& b) {
    std::vector<Run> out;
    for (i32 y = 0; y < b.h; ++y)
        for (i32 x = 0; x < b.w;) {
            const u8 g = b.at(x, y);
            i32 n = 1;
            while (g < kNone && b.at(x + n, y) == g) ++n;
            if (g < kNone && n >= 3) out.push_back({g, x, y, n, true});
            x += n;
        }
    for (i32 x = 0; x < b.w; ++x)
        for (i32 y = 0; y < b.h;) {
            const u8 g = b.at(x, y);
            i32 n = 1;
            while (g < kNone && b.at(x, y + n) == g) ++n;
            if (g < kNone && n >= 3) out.push_back({g, x, y, n, false});
            y += n;
        }
    return out;
}

std::vector<Group> find_groups(const Board& b) {
    const std::vector<Run> runs = find_runs(b);
    // Rows that share a cell are one group (a cell has one gem, so they are of one gem).
    std::vector<usize> parent(runs.size());
    std::iota(parent.begin(), parent.end(), usize{0});
    auto root = [&](usize i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::vector<i32> owner(b.cells.size(), -1);
    for (usize r = 0; r < runs.size(); ++r)
        for (i32 k = 0; k < runs[r].length; ++k) {
            const i32 cell = runs[r].across ? b.index(runs[r].x + k, runs[r].y) : b.index(runs[r].x, runs[r].y + k);
            i32& o = owner[static_cast<usize>(cell)];
            if (o >= 0) parent[root(static_cast<usize>(o))] = root(r);
            else o = static_cast<i32>(r);
        }
    std::vector<Group> groups;
    std::vector<i32> group_of(runs.size(), -1);
    for (usize r = 0; r < runs.size(); ++r) {
        const usize top = root(r);
        if (group_of[top] < 0) {
            group_of[top] = static_cast<i32>(groups.size());
            groups.push_back({runs[r].gem, {}, 0});
        }
        Group& g = groups[static_cast<usize>(group_of[top])];
        g.longest = std::max(g.longest, runs[r].length);
        for (i32 k = 0; k < runs[r].length; ++k)
            g.cells.push_back(runs[r].across ? b.index(runs[r].x + k, runs[r].y) : b.index(runs[r].x, runs[r].y + k));
    }
    for (Group& g : groups) {
        std::sort(g.cells.begin(), g.cells.end());
        g.cells.erase(std::unique(g.cells.begin(), g.cells.end()), g.cells.end());
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& c) { return a.cells.front() < c.cells.front(); });
    return groups;
}

u64 group_points(const Group& g, const Scoring& s, u32 step) {
    const f64 each = g.gem < s.points.size() ? static_cast<f64>(s.points[g.gem]) : 0.0;
    const f64 length = g.longest >= 5 ? s.five : g.longest == 4 ? s.four : 1.0;
    const f64 chain = 1.0 + static_cast<f64>(step > 0 ? step - 1 : 0) * s.chain;
    const f64 v = each * static_cast<f64>(g.cells.size()) * length * chain;
    // Rounded down; a hair up first so 3 × 10 × 1,5 is 45, not 44,999….
    return v > 0 ? static_cast<u64>(std::floor(v + 1e-9)) : 0;
}

void remove_groups(Board& b, const std::vector<Group>& groups) {
    for (const Group& g : groups)
        for (i32 c : g.cells) b.cells[static_cast<usize>(c)] = kNone;
}

namespace {

// The cells of a column that are not holes, from the top.
std::vector<i32> column(const Board& b, i32 x) {
    std::vector<i32> out;
    for (i32 y = 0; y < b.h; ++y)
        if (b.cell(x, y)) out.push_back(b.index(x, y));
    return out;
}

// A row through the cell (x, y) with its gem as it is.
bool run_through(const Board& b, i32 x, i32 y) {
    const u8 g = b.at(x, y);
    if (g >= kNone) return false;
    i32 across = 1, down = 1;
    for (i32 k = 1; b.at(x - k, y) == g; ++k) ++across;
    for (i32 k = 1; b.at(x + k, y) == g; ++k) ++across;
    for (i32 k = 1; b.at(x, y - k) == g; ++k) ++down;
    for (i32 k = 1; b.at(x, y + k) == g; ++k) ++down;
    return across >= 3 || down >= 3;
}

// Fills the cells without a gem, none making a row with what is there; false when a cell has no gem that fits.
bool fill_without_runs(Board& b, Rng& rng, u32 gems) {
    for (usize i = 0; i < b.cells.size(); ++i) {
        if (b.cells[i] != kNone) continue;
        const i32 x = b.x_of(static_cast<i32>(i)), y = b.y_of(static_cast<i32>(i));
        u8 fits[kMaxGems];
        u32 n = 0;
        for (u32 g = 0; g < gems && g < kMaxGems; ++g) {
            b.cells[i] = static_cast<u8>(g);
            if (!run_through(b, x, y)) fits[n++] = static_cast<u8>(g);
        }
        if (n == 0) {
            b.cells[i] = kNone;
            return false;
        }
        b.cells[i] = fits[rng.below(n)];
    }
    return true;
}

} // namespace

std::vector<Fall> fall(Board& b) {
    std::vector<Fall> out;
    for (i32 x = 0; x < b.w; ++x) {
        const std::vector<i32> col = column(b, x);
        i32 to = static_cast<i32>(col.size()) - 1; // the lowest cell not yet given a gem
        for (i32 k = static_cast<i32>(col.size()) - 1; k >= 0; --k) {
            const u8 g = b.cells[static_cast<usize>(col[static_cast<usize>(k)])];
            if (g >= kNone) continue;
            if (k != to) {
                b.cells[static_cast<usize>(col[static_cast<usize>(to)])] = g;
                b.cells[static_cast<usize>(col[static_cast<usize>(k)])] = kNone;
                out.push_back({col[static_cast<usize>(k)], col[static_cast<usize>(to)], g});
            }
            --to;
        }
    }
    return out;
}

std::vector<Drop> fill(Board& b, Rng& rng, u32 gems) {
    std::vector<Drop> out;
    for (i32 x = 0; x < b.w; ++x) {
        const std::vector<i32> col = column(b, x);
        i32 k = 0; // how many of the column's empty cells got a gem, from the bottom
        for (i32 j = static_cast<i32>(col.size()) - 1; j >= 0; --j) {
            const i32 c = col[static_cast<usize>(j)];
            if (b.cells[static_cast<usize>(c)] != kNone) continue;
            const u8 g = static_cast<u8>(rng.below(gems));
            b.cells[static_cast<usize>(c)] = g;
            // The lowest new gem comes from just above the column, the next from above it…: each falls as far.
            out.push_back({c, g, ++k});
        }
    }
    return out;
}

bool neighbours(const Board& b, i32 a, i32 c) {
    if (a < 0 || c < 0 || a >= static_cast<i32>(b.cells.size()) || c >= static_cast<i32>(b.cells.size())) return false;
    const i32 dx = std::abs(b.x_of(a) - b.x_of(c)), dy = std::abs(b.y_of(a) - b.y_of(c));
    return dx + dy == 1 && b.cells[static_cast<usize>(a)] != kHole && b.cells[static_cast<usize>(c)] != kHole;
}

bool swap_makes_run(const Board& b, i32 a, i32 c) {
    if (!neighbours(b, a, c)) return false;
    const u8 ga = b.cells[static_cast<usize>(a)], gc = b.cells[static_cast<usize>(c)];
    if (ga >= kNone || gc >= kNone || ga == gc) return false;
    Board t = b;
    std::swap(t.cells[static_cast<usize>(a)], t.cells[static_cast<usize>(c)]);
    return run_through(t, t.x_of(a), t.y_of(a)) || run_through(t, t.x_of(c), t.y_of(c));
}

std::optional<std::pair<i32, i32>> find_move(const Board& b) {
    for (i32 y = 0; y < b.h; ++y)
        for (i32 x = 0; x < b.w; ++x) {
            const i32 i = b.index(x, y);
            if (x + 1 < b.w && swap_makes_run(b, i, b.index(x + 1, y))) return std::pair{i, b.index(x + 1, y)};
            if (y + 1 < b.h && swap_makes_run(b, i, b.index(x, y + 1))) return std::pair{i, b.index(x, y + 1)};
        }
    return std::nullopt;
}

bool shape_can_move(const Board& s) {
    static constexpr i32 dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (i32 y = 0; y < s.h; ++y)
        for (i32 x = 0; x < s.w; ++x)
            for (const bool across : {true, false}) {
                const i32 dx = across ? 1 : 0, dy = across ? 0 : 1;
                if (!s.cell(x, y) || !s.cell(x + dx, y + dy) || !s.cell(x + 2 * dx, y + 2 * dy)) continue;
                for (i32 k = 0; k < 3; ++k) {
                    const i32 cx = x + k * dx, cy = y + k * dy;
                    for (const auto& d : dirs) {
                        const i32 qx = cx + d[0], qy = cy + d[1];
                        const bool in_line = across ? (qy == y && qx >= x && qx <= x + 2) : (qx == x && qy >= y && qy <= y + 2);
                        if (!in_line && s.cell(qx, qy)) return true;
                    }
                }
            }
    return false;
}

std::string cells_words(const Board& b, const std::vector<i32>& cells) {
    std::string out;
    for (i32 c : cells) out += (out.empty() ? "" : ", ") + std::to_string(b.x_of(c) + 1) + ":" + std::to_string(b.y_of(c) + 1);
    return out;
}

bool fill_start(Board& board, Rng& rng, u32 gems, const std::string& name, std::string& why) {
    const std::string field = "Поле «" + name + "»: ";
    if (gems < kMinGems || gems > kMaxGems) {
        why = field + "фишек в наборе " + std::to_string(gems) + ", а нужно от " + std::to_string(kMinGems) + " до " +
              std::to_string(kMaxGems);
        return false;
    }
    if (const std::vector<Group> groups = find_groups(board); !groups.empty()) {
        std::vector<i32> cells;
        for (const Group& g : groups) cells.insert(cells.end(), g.cells.begin(), g.cells.end());
        std::sort(cells.begin(), cells.end());
        why = field + "фишки, поставленные заранее, уже стоят в ряд (клетки " + cells_words(board, cells) + ")";
        return false;
    }
    if (!shape_can_move(board)) {
        why = field + "ходов не будет: нужно хотя бы три клетки подряд и клетка рядом для обмена";
        return false;
    }
    for (u32 t = 0; t < kTries; ++t) {
        Board b = board;
        if (fill_without_runs(b, rng, gems) && find_move(b)) {
            board = std::move(b);
            return true;
        }
    }
    why = field + "за " + std::to_string(kTries) +
          " попыток не вышло расставить фишки без готовых рядов и с ходом. Добавьте клеток или фишек в набор";
    return false;
}

Shuffle shuffle(Board& board, Rng& rng, u32 gems) {
    Shuffle out;
    std::vector<i32> cells;
    std::vector<u8> held;
    for (usize i = 0; i < board.cells.size(); ++i)
        if (board.cells[i] < kNone) {
            cells.push_back(static_cast<i32>(i));
            held.push_back(board.cells[i]);
        }
    for (u32 t = 0; t < kTries; ++t, ++out.tries) {
        std::vector<u8> g = held;
        for (usize i = g.size(); i > 1; --i) std::swap(g[i - 1], g[rng.below(static_cast<u32>(i))]);
        Board b = board;
        for (usize i = 0; i < cells.size(); ++i) b.cells[static_cast<usize>(cells[i])] = g[i];
        if (find_groups(b).empty() && find_move(b)) {
            board = std::move(b);
            out.done = true;
            ++out.tries;
            return out;
        }
    }
    for (u32 t = 0; t < kTries; ++t, ++out.tries) {
        Board b = board;
        for (u8& c : b.cells)
            if (c != kHole) c = kNone;
        if (fill_without_runs(b, rng, gems) && find_move(b)) {
            board = std::move(b);
            out.done = out.refilled = true;
            ++out.tries;
            return out;
        }
    }
    return out;
}

// --- a level being played -----------------------------------------------------------------------------------------

bool Play::start(const Board& shape, u32 gems, const Scoring& scoring, const Rules& rules, const std::string& name,
                 std::string& why) {
    Board b = shape;
    Rng rng(rules.seed);
    if (!fill_start(b, rng, gems, name, why)) return false;
    board_ = std::move(b);
    rng_ = rng;
    scoring_ = scoring;
    rules_ = rules;
    gems_ = gems;
    score_ = 0;
    moves_ = rules.moves;
    step_ = shuffles_ = 0;
    end_ = End::None;
    busy_ = false;
    return true;
}

bool Play::resume(const Saved& s, u32 gems, const Scoring& scoring, const Rules& rules, std::string& why) {
    if (s.board.w <= 0 || s.board.h <= 0 || s.board.w > kMaxSide || s.board.h > kMaxSide ||
        s.board.cells.size() != static_cast<usize>(s.board.w * s.board.h)) {
        why = "поле в сохранении не читается";
        return false;
    }
    for (u8 c : s.board.cells)
        if (c != kHole && c >= gems) {
            why = "в сохранении на поле фишка, которой нет в наборе";
            return false;
        }
    board_ = s.board;
    rng_ = Rng(s.rng);
    scoring_ = scoring;
    rules_ = rules;
    gems_ = gems;
    score_ = s.score;
    moves_ = s.moves;
    end_ = s.end;
    shuffles_ = s.shuffles;
    step_ = 0;
    busy_ = false;
    return true;
}

Play::Saved Play::saved() const { return {board_, score_, moves_, end_, rng_.state, shuffles_}; }

Swap Play::swap(i32 a, i32 b) {
    if (end_ != End::None || busy_ || !neighbours(board_, a, b)) return Swap::Refused;
    if (board_.cells[static_cast<usize>(a)] >= kNone || board_.cells[static_cast<usize>(b)] >= kNone) return Swap::Refused;
    if (!swap_makes_run(board_, a, b)) return Swap::NoRun;
    std::swap(board_.cells[static_cast<usize>(a)], board_.cells[static_cast<usize>(b)]);
    if (moves_ > 0) --moves_;
    busy_ = true;
    step_ = 0;
    return Swap::Done;
}

bool Play::step(Step& out) {
    out = {};
    if (end_ != End::None || !busy_) {
        busy_ = false;
        return false;
    }
    std::vector<Group> groups = find_groups(board_);
    if (groups.empty()) {
        busy_ = false;
        settle();
        return false;
    }
    out.number = ++step_;
    for (const Group& g : groups) {
        const u64 points = group_points(g, scoring_, step_);
        score_ += points;
        out.points.push_back(points);
        Event e;
        e.kind = Event::Kind::Match;
        e.gem = g.gem;
        e.cells = static_cast<u32>(g.cells.size());
        e.points = points;
        e.step = step_;
        said(e);
    }
    remove_groups(board_, groups);
    out.groups = std::move(groups);
    out.falls = fall(board_);
    out.drops = fill(board_, rng_, gems_);
    return true;
}

void Play::settle() {
    if (end_ != End::None) return;
    Event e;
    // The end first: the target before the moves (a last move that reaches it wins), and a board without a move is
    // shuffled only when the level goes on.
    if (score_ >= rules_.target) {
        end_ = End::Won;
        e.kind = Event::Kind::Target;
        said(e);
        return;
    }
    if (moves_ == 0) {
        end_ = End::Lost;
        e.kind = Event::Kind::MovesOut;
        said(e);
        return;
    }
    if (find_move(board_)) return;
    const Shuffle s = shuffle(board_, rng_, gems_);
    if (s.done) {
        ++shuffles_;
        e.kind = Event::Kind::Shuffled;
        e.refilled = s.refilled;
        said(e);
        return;
    }
    end_ = End::Lost;
    e.kind = Event::Kind::NoMoves;
    said(e);
}

void Play::add_moves(i32 n) {
    if (end_ != End::None) return;
    moves_ = static_cast<u32>(std::max<i64>(0, static_cast<i64>(moves_) + n));
}

void Play::add_score(i64 n) {
    if (end_ != End::None) return;
    score_ = static_cast<u64>(std::max<i64>(0, static_cast<i64>(score_) + n));
}

} // namespace match3
