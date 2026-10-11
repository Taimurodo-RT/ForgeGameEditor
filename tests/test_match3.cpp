// The rules of a «Три в ряд» board (step 14.3b, 14.3-три-в-ряд-модель.md, «Как проверяется 14.3b», п. 1): rows, groups,
// scores, falling past holes, filling, the start, shuffles, the order of the end and the saves, with no window.

#include "match3_rules.h"

#include "forge/core/ease.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace match3;

namespace {

// Rows of the board from the top: 'A'… a gem, '.' a cell without a gem, '#' a hole.
Board board_of(const std::vector<std::string>& rows) {
    Board b(static_cast<i32>(rows.front().size()), static_cast<i32>(rows.size()));
    for (i32 y = 0; y < b.h; ++y)
        for (i32 x = 0; x < b.w; ++x) {
            const char c = rows[static_cast<usize>(y)][static_cast<usize>(x)];
            b.set(x, y, c == '#' ? kHole : c == '.' ? kNone : static_cast<u8>(c - 'A'));
        }
    return b;
}

std::vector<std::string> rows_of(const Board& b) {
    std::vector<std::string> out;
    for (i32 y = 0; y < b.h; ++y) {
        std::string row;
        for (i32 x = 0; x < b.w; ++x) {
            const u8 v = b.at(x, y);
            row += v == kHole ? '#' : v == kNone ? '.' : static_cast<char>('A' + v);
        }
        out.push_back(row);
    }
    return out;
}

Scoring tens(u32 gems) {
    Scoring s;
    s.points.assign(gems, 10);
    return s;
}

// Plays the move and its cascade; the steps.
std::vector<Step> play_move(Play& p, i32 a, i32 b) {
    std::vector<Step> out;
    REQUIRE(p.swap(a, b) == Swap::Done);
    for (Step s; p.step(s);) out.push_back(s);
    return out;
}

} // namespace

TEST_CASE("match3: rows are found along the board's rows and its columns, each on its own, never on a diagonal") {
    const Board b = board_of({"AAAB",
                              "BCCB",
                              "CAAB",
                              "ABCA"});
    const std::vector<Run> runs = find_runs(b);
    REQUIRE(runs.size() == 2);
    CHECK((runs[0].across && runs[0].gem == 0 && runs[0].x == 0 && runs[0].y == 0 && runs[0].length == 3));
    CHECK((!runs[1].across && runs[1].gem == 1 && runs[1].x == 3 && runs[1].y == 0 && runs[1].length == 3));
    // A diagonal of A (0,0) (1,1)… is no row; neither is a column of two.
    CHECK(find_runs(board_of({"ABC", "BAC", "CBA"})).empty());
}

TEST_CASE("match3: rows of one gem sharing a cell are one group («Г», «Т», «+»), the crossing cell counted once") {
    // «Г»: three across and three down from its corner.
    {
        const std::vector<Group> g = find_groups(board_of({"AAAB",
                                                           "ABCB",
                                                           "ACBC"}));
        REQUIRE(g.size() == 1);
        CHECK(g[0].cells == std::vector<i32>{0, 1, 2, 4, 8});
        CHECK(g[0].longest == 3);
    }
    // «Т».
    {
        const std::vector<Group> g = find_groups(board_of({"AAAB",
                                                           "BACB",
                                                           "CACB"}));
        REQUIRE(g.size() == 2); // the T of A, and B's column on the right
        CHECK(g[0].gem == 0);
        CHECK(g[0].cells == std::vector<i32>{0, 1, 2, 5, 9});
        CHECK(g[1].gem == 1);
        CHECK(g[1].cells == std::vector<i32>{3, 7, 11});
    }
    // «+»: five cells, the middle once; points: 5 cells × 10, not 6.
    {
        const Board b = board_of({"BAC",
                                  "AAA",
                                  "CAB"});
        const std::vector<Group> g = find_groups(b);
        REQUIRE(g.size() == 1);
        CHECK(g[0].cells == std::vector<i32>{1, 3, 4, 5, 7});
        CHECK(group_points(g[0], tens(3), 1) == 50);
    }
    // Two rows of one gem that do not touch: two groups, two events.
    {
        const std::vector<Group> g = find_groups(board_of({"AAAB",
                                                           "BCBC",
                                                           "AAAC"}));
        REQUIRE(g.size() == 2);
        CHECK(g[0].gem == 0);
        CHECK(g[1].gem == 0);
        CHECK(g[0].cells == std::vector<i32>{0, 1, 2});
        CHECK(g[1].cells == std::vector<i32>{8, 9, 10});
    }
}

TEST_CASE("match3: four and five in a row and the chain multiply a group's points, rounded down") {
    Scoring s = tens(3);
    s.points[1] = 7;
    Group three{0, {0, 1, 2}, 3}, four{0, {0, 1, 2, 3}, 4}, five{0, {0, 1, 2, 3, 4}, 5}, six{0, {0, 1, 2, 3, 4, 5}, 6};
    CHECK(group_points(three, s, 1) == 30);
    CHECK(group_points(four, s, 1) == 60);  // 40 × 1,5
    CHECK(group_points(five, s, 1) == 100); // 50 × 2
    CHECK(group_points(six, s, 1) == 120);  // five and more: × 2
    CHECK(group_points(three, s, 2) == 45); // the second step of a cascade: × 1,5
    CHECK(group_points(three, s, 3) == 60); // × 2
    // Rounded down: 3 × 7 × 1,5 = 31,5.
    Group sevens{1, {0, 1, 2}, 3};
    CHECK(group_points(sevens, s, 2) == 31);
    s.four = 1.25;
    Group sevens4{1, {0, 1, 2, 3}, 4};
    CHECK(group_points(sevens4, s, 1) == 35); // 28 × 1,25
}

TEST_CASE("match3: gems fall past holes, and new ones come into the top cells of their column") {
    Board b = board_of({"A.B",
                        "#.#",
                        "..C",
                        "B#."});
    const std::vector<Fall> falls = fall(b);
    // Column 0: A falls past the hole into row 2 (row 3 has B); column 2: C falls into row 3; B to row 2.
    CHECK(rows_of(b) == std::vector<std::string>{"...", "#.#", "A.B", "B#C"});
    REQUIRE(falls.size() == 3);
    CHECK((falls[0].from == 0 && falls[0].to == 6 && falls[0].gem == 0));
    CHECK((falls[1].from == 8 && falls[1].to == 11 && falls[1].gem == 2));
    CHECK((falls[2].from == 2 && falls[2].to == 8 && falls[2].gem == 1));
    Rng rng(7);
    const std::vector<Drop> drops = fill(b, rng, 3);
    // Every cell has a gem now, the holes are holes; new gems only where there was none.
    for (const std::string& row : rows_of(b)) CHECK(row.find('.') == std::string::npos);
    CHECK(b.at(0, 1) == kHole);
    CHECK(b.at(1, 3) == kHole);
    CHECK(b.at(0, 2) == 0);
    REQUIRE(drops.size() == 5); // column 0: 1; column 1: 3; column 2: 1
    // Column 1, from the bottom: row 2 comes from just above the column, row 1 from above it, row 0 from above that.
    CHECK((drops[1].cell == 7 && drops[1].above == 1));
    CHECK((drops[2].cell == 4 && drops[2].above == 2));
    CHECK((drops[3].cell == 1 && drops[3].above == 3));
    CHECK(rng.calls == 5);
}

TEST_CASE("match3: a swap without a row goes back and spends no move; a swap is of two neighbouring gems") {
    Play p;
    std::string why;
    Rules r;
    r.seed = 11;
    r.moves = 5;
    REQUIRE(p.start(Board(6, 6), 5, tens(5), r, "Первый", why));
    const Board before = p.board();
    // A pair of neighbours whose swap makes no row.
    i32 a = -1, b = -1;
    for (i32 i = 0; i < 35 && a < 0; ++i)
        if ((i + 1) % 6 != 0 && !swap_makes_run(before, i, i + 1) && before.cells[usize(i)] != before.cells[usize(i + 1)]) a = i, b = i + 1;
    REQUIRE(a >= 0);
    CHECK(p.swap(a, b) == Swap::NoRun);
    CHECK(p.board() == before);
    CHECK(p.moves() == 5);
    CHECK_FALSE(p.busy());
    CHECK(p.swap(0, 7) == Swap::Refused); // a diagonal
    CHECK(p.swap(0, 2) == Swap::Refused); // not next to each other
    CHECK(p.moves() == 5);
    // A move spends one at the swap, before the cascade.
    const auto m = find_move(p.board());
    REQUIRE(m);
    CHECK(p.swap(m->first, m->second) == Swap::Done);
    CHECK(p.moves() == 4);
    CHECK(p.busy());
    CHECK(p.swap(m->first, m->second) == Swap::Refused); // not during a cascade
}

TEST_CASE("match3: a cascade goes step by step, its groups in the order of their top left cell, the chain growing") {
    // A swap of (2,1) and (2,2)... set up so the first step has two groups and new rows come of the fall.
    Board b = board_of({"BCDB",
                        "AADC",
                        "CBAB",
                        "BBCB"});
    Play p;
    std::string why;
    Play::Saved s;
    s.board = b;
    s.moves = 3;
    s.rng = 5;
    Rules r;
    r.target = 100000;
    REQUIRE(p.resume(s, 4, tens(4), r, why));
    std::vector<Event> events;
    p.on_event = [&](const Event& e) { events.push_back(e); };
    // (2,2) A up to (2,1): «AAA» across row 1; and B at (2,1)… down to (2,2): «BB B» in row 3? no: (2,2) is in row 2.
    REQUIRE(p.swap(b.index(2, 1), b.index(2, 2)) == Swap::Done);
    Step first;
    REQUIRE(p.step(first));
    CHECK(first.number == 1);
    REQUIRE(first.groups.size() >= 1);
    CHECK(first.groups[0].cells == std::vector<i32>{4, 5, 6});
    CHECK(first.points[0] == 30);
    for (usize i = 1; i < first.groups.size(); ++i) CHECK(first.groups[i - 1].cells.front() < first.groups[i].cells.front());
    // The groups' events, in that order, each with its points, before the cells went.
    REQUIRE(events.size() == first.groups.size());
    CHECK(events[0].kind == Event::Kind::Match);
    CHECK(events[0].gem == 0);
    CHECK(events[0].cells == 3);
    CHECK(events[0].step == 1);
    // Further steps, if any, score with the chain.
    for (Step st; p.step(st);) {
        CHECK(st.number >= 2);
        for (usize i = 0; i < st.groups.size(); ++i) CHECK(st.points[i] == group_points(st.groups[i], tens(4), st.number));
    }
    CHECK_FALSE(p.busy());
    CHECK(find_groups(p.board()).empty());
}

TEST_CASE("match3: a start leaves no row and a move, keeps the author's gems and says why it cannot start") {
    std::string why;
    // A board of 8×8 with five gems, a hole and two gems put there before.
    Board shape(8, 8);
    shape.set(3, 3, kHole);
    shape.set(0, 0, 4);
    shape.set(7, 7, 2);
    for (u64 seed = 1; seed <= 20; ++seed) {
        Board b = shape;
        Rng rng(seed);
        REQUIRE(fill_start(b, rng, 5, "Первый", why));
        CHECK(find_groups(b).empty());
        CHECK(find_move(b).has_value());
        CHECK(b.at(3, 3) == kHole);
        CHECK(b.at(0, 0) == 4);
        CHECK(b.at(7, 7) == 2);
        for (u8 c : b.cells) CHECK((c == kHole || c < 5));
    }
    // The same seed, the same board.
    {
        Board a = shape, c = shape;
        Rng r1(42), r2(42);
        REQUIRE(fill_start(a, r1, 5, "Первый", why));
        REQUIRE(fill_start(c, r2, 5, "Первый", why));
        CHECK(a == c);
    }
    // The shape: two cells, or three in a row with nothing beside them, never has a move.
    {
        Board two = board_of({"..", "##"});
        Rng rng(1);
        CHECK_FALSE(fill_start(two, rng, 3, "Первый", why));
        CHECK(why == "Поле «Первый»: ходов не будет: нужно хотя бы три клетки подряд и клетка рядом для обмена");
        Board three = board_of({"...", "###"});
        CHECK_FALSE(shape_can_move(three));
        CHECK_FALSE(fill_start(three, rng, 3, "Первый", why));
        CHECK(rng.calls == 0);
        CHECK(shape_can_move(board_of({"...", "#.#"})));
        CHECK(shape_can_move(board_of({"....", "####"})));
    }
    // Gems put there before already in a row.
    {
        Board b = board_of({"AAA.",
                            "....",
                            "...."});
        Rng rng(1);
        CHECK_FALSE(fill_start(b, rng, 3, "Первый", why));
        CHECK(why == "Поле «Первый»: фишки, поставленные заранее, уже стоят в ряд (клетки 1:1, 2:1, 3:1)");
    }
    // Too few or too many gems.
    {
        Board b(4, 4);
        Rng rng(1);
        CHECK_FALSE(fill_start(b, rng, 2, "Второй", why));
        CHECK(why == "Поле «Второй»: фишек в наборе 2, а нужно от 3 до 8");
        CHECK_FALSE(fill_start(b, rng, 9, "Второй", why));
        CHECK(why == "Поле «Второй»: фишек в наборе 9, а нужно от 3 до 8");
    }
    // A line of four with three gems put there: whatever comes into the last cell, no move (or a row): 100 tries,
    // one number each, then no.
    {
        Board b = board_of({"AB.B"});
        Rng rng(3);
        CHECK(shape_can_move(b));
        CHECK_FALSE(fill_start(b, rng, 3, "Третий", why));
        CHECK(why == "Поле «Третий»: за 100 попыток не вышло расставить фишки без готовых рядов и с ходом. Добавьте клеток "
                     "или фишек в набор");
        CHECK(rng.calls == kTries);
        CHECK(rows_of(b) == std::vector<std::string>{"AB.B"});
    }
}

TEST_CASE("match3: a shuffle moves the same gems over the same cells, holes stay; then a new fill; then no") {
    // No move, but the same gems put otherwise have one.
    {
        Board b = board_of({"ABABA", "#####"});
        CHECK_FALSE(find_move(b));
        Rng rng(9);
        const Shuffle s = shuffle(b, rng, 3);
        CHECK(s.done);
        CHECK_FALSE(s.refilled);
        CHECK(find_move(b).has_value());
        CHECK(find_groups(b).empty());
        std::string row = rows_of(b)[0];
        std::sort(row.begin(), row.end());
        CHECK(row == "AAABB");
        CHECK(rows_of(b)[1] == "#####");
    }
    // Two A, a B and a C over four cells never make a move: new gems.
    {
        Board b = board_of({"ABAC"});
        CHECK_FALSE(find_move(b));
        Rng rng(9);
        const Shuffle s = shuffle(b, rng, 3);
        CHECK(s.done);
        CHECK(s.refilled);
        CHECK(s.tries > kTries);
        CHECK(find_move(b).has_value());
    }
    // Three cells alone: neither works; kTries + kTries tries, the board as it was.
    {
        Board b = board_of({"ABC"});
        Rng rng(9);
        const Shuffle s = shuffle(b, rng, 3);
        CHECK_FALSE(s.done);
        CHECK(s.tries == 2 * kTries);
        CHECK(rows_of(b) == std::vector<std::string>{"ABC"});
    }
}

TEST_CASE("match3: after the cascade the target comes first, then the moves, then a shuffle; the end only once") {
    Rules r;
    r.target = 100;
    std::string why;
    std::vector<Event> events;
    auto count = [&](Event::Kind kind) {
        return std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.kind == kind; });
    };
    // «+2 хода» on the last move's match comes before the moves are counted: the level goes on.
    {
        Play p;
        r.moves = 1;
        r.target = 1000000;
        r.seed = 21;
        REQUIRE(p.start(Board(6, 6), 4, tens(4), r, "Первый", why));
        events.clear();
        p.on_event = [&](const Event& e) {
            events.push_back(e);
            if (e.kind == Event::Kind::Match) p.add_moves(2);
        };
        const auto m = find_move(p.board());
        REQUIRE(m);
        play_move(p, m->first, m->second);
        CHECK(p.end() == End::None);
        CHECK(p.moves() >= 2);
        CHECK(count(Event::Kind::MovesOut) == 0);
    }
    // The same without the link: the last move lost the level, once.
    {
        Play p;
        REQUIRE(p.start(Board(6, 6), 4, tens(4), r, "Первый", why));
        events.clear();
        p.on_event = [&](const Event& e) { events.push_back(e); };
        const auto m = find_move(p.board());
        play_move(p, m->first, m->second);
        CHECK(p.end() == End::Lost);
        CHECK(p.moves() == 0);
        CHECK(count(Event::Kind::MovesOut) == 1);
        // Over: no swap, no step, no moves or points from links, no event.
        const usize said = events.size();
        const auto m2 = find_move(p.board());
        if (m2) CHECK(p.swap(m2->first, m2->second) == Swap::Refused);
        Step s;
        CHECK_FALSE(p.step(s));
        p.add_moves(5);
        p.add_score(5);
        CHECK(p.moves() == 0);
        CHECK(events.size() == said);
    }
    // The target reached by the last move: won, not lost.
    {
        Play p;
        r.target = 1;
        REQUIRE(p.start(Board(6, 6), 4, tens(4), r, "Первый", why));
        events.clear();
        p.on_event = [&](const Event& e) { events.push_back(e); };
        const auto m = find_move(p.board());
        play_move(p, m->first, m->second);
        CHECK(p.end() == End::Won);
        CHECK(count(Event::Kind::Target) == 1);
        CHECK(count(Event::Kind::MovesOut) == 0);
    }
}

TEST_CASE("match3: the end is looked at before a shuffle: a won level on a board with no move is not shuffled") {
    // Three cells alone: no move, and no shuffle can make one.
    Play::Saved s;
    s.board = board_of({"ABC"});
    s.rng = 77;
    Rules r;
    r.target = 100;
    std::string why;
    std::vector<Event> events;
    auto on = [&](const Event& e) { events.push_back(e); };
    // Target reached (the last move did it): won; the generator untouched, no shuffle, no «Ходов нет».
    {
        Play p;
        s.score = 100;
        s.moves = 0;
        REQUIRE(p.resume(s, 3, tens(3), r, why));
        p.on_event = on;
        events.clear();
        p.settle();
        CHECK(p.end() == End::Won);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Event::Kind::Target);
        CHECK(p.rng().state == Rng(77).state);
        CHECK(p.shuffles() == 0);
    }
    // No moves left: lost by the moves, not by «Ходов нет», no shuffle.
    {
        Play p;
        s.score = 10;
        s.moves = 0;
        REQUIRE(p.resume(s, 3, tens(3), r, why));
        p.on_event = on;
        events.clear();
        p.settle();
        CHECK(p.end() == End::Lost);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Event::Kind::MovesOut);
        CHECK(p.rng().calls == 0);
    }
    // The level goes on and the board has no move: a shuffle is tried, and when none works the level is lost so.
    {
        Play p;
        s.score = 10;
        s.moves = 3;
        REQUIRE(p.resume(s, 3, tens(3), r, why));
        p.on_event = on;
        events.clear();
        p.settle();
        CHECK(p.end() == End::Lost);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Event::Kind::NoMoves);
        CHECK(p.rng().calls > 0);
    }
    // A board that can be shuffled: shuffled once, the level goes on.
    {
        Play p;
        s.board = board_of({"ABABA"});
        s.score = 10;
        s.moves = 3;
        REQUIRE(p.resume(s, 3, tens(3), r, why));
        p.on_event = on;
        events.clear();
        p.settle();
        CHECK(p.end() == End::None);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Event::Kind::Shuffled);
        CHECK(p.shuffles() == 1);
        CHECK(find_move(p.board()).has_value());
    }
}

TEST_CASE("match3: one seed, one game; a save in between goes on with the same gems") {
    auto run = [](u64 seed, i32 save_after, Play::Saved* saved) {
        Play p;
        Rules r;
        r.seed = seed;
        r.moves = 12;
        r.target = 1000000;
        std::string why;
        REQUIRE(p.start(Board(7, 7), 5, tens(5), r, "Первый", why));
        std::vector<std::vector<std::string>> boards;
        for (i32 n = 0; p.end() == End::None; ++n) {
            if (n == save_after && saved) *saved = p.saved();
            const auto m = find_move(p.board());
            REQUIRE(m);
            play_move(p, m->first, m->second);
            boards.push_back(rows_of(p.board()));
            boards.back().push_back(std::to_string(p.score()) + "/" + std::to_string(p.moves()));
        }
        return boards;
    };
    Play::Saved at5;
    const auto first = run(1234, 5, &at5);
    CHECK(first == run(1234, -1, nullptr));
    CHECK(first != run(1235, -1, nullptr));
    CHECK(first.size() == 12);
    // Resumed from the save after five moves: the same seven moves after it.
    Play p;
    Rules r;
    r.moves = 12;
    r.target = 1000000;
    std::string why;
    REQUIRE(p.resume(at5, 5, tens(5), r, why));
    CHECK(p.moves() == 7);
    for (usize n = 5; n < first.size(); ++n) {
        const auto m = find_move(p.board());
        REQUIRE(m);
        play_move(p, m->first, m->second);
        std::vector<std::string> b = rows_of(p.board());
        b.push_back(std::to_string(p.score()) + "/" + std::to_string(p.moves()));
        CHECK(b == first[n]);
    }
    // A save that names a gem the set has not: refused.
    Play::Saved bad = at5;
    bad.board.cells[0] = 6;
    CHECK_FALSE(p.resume(bad, 5, tens(5), r, why));
    CHECK(why == "в сохранении на поле фишка, которой нет в наборе");
}

TEST_CASE("ease: the share of the way at a share of the time, for what a module moves by itself") {
    using namespace forge;
    CHECK(ease::progress(0, 0.15) == 0);
    CHECK(ease::progress(0.075, 0.15) == doctest::Approx(0.5));
    CHECK(ease::progress(1, 0.15) == 1);
    CHECK(ease::progress(-1, 0.15) == 0);
    CHECK(ease::progress(0.1, 0) == 1);
    CHECK(ease::linear(0.25) == 0.25);
    CHECK(ease::linear(2) == 1);
    CHECK(ease::in_out(0) == 0);
    CHECK(ease::in_out(0.5) == doctest::Approx(0.5));
    CHECK(ease::in_out(1) == 1);
    CHECK(ease::in_out(0.1) < 0.1); // gently at the start
    CHECK(ease::in_out(0.9) > 0.9); // and at the end
    CHECK(ease::fall(0.5) == doctest::Approx(0.25));
    CHECK(ease::fall(1) == 1);
    // From rest under g: 2 tiles at 40 tiles / s² take √(2·2/40) s, and the way at half that time is a quarter.
    CHECK(ease::fall_time(2, 40) == doctest::Approx(std::sqrt(0.1)));
    CHECK(ease::fall_time(0, 40) == 0);
    CHECK(ease::lerp(2, 6, 0.25) == 3);
}
