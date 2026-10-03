#include "forge/sim/factory.h"

#include "forge/core/jobs.h"
#include "forge/core/profile.h"
#include "forge/core/time.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace forge::sim {

namespace {

constexpr u32 kNone = ~0u;
constexpr u64 kNoTile = ~0ull;
constexpr i32 kDx[4] = {1, 0, -1, 0};
constexpr i32 kDy[4] = {0, 1, 0, -1};

u64 key(i32 x, i32 y) { return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y); }
i32 key_x(u64 k) { return static_cast<i32>(static_cast<u32>(k >> 32)); }
i32 key_y(u64 k) { return static_cast<i32>(static_cast<u32>(k)); }
u64 step(u64 k, Dir d) { return key(key_x(k) + kDx[static_cast<u8>(d)], key_y(k) + kDy[static_cast<u8>(d)]); }
Dir opposite(Dir d) { return static_cast<Dir>((static_cast<u8>(d) + 2) & 3); }

constexpr i32 kPoleCell = 16; // tiles per cell of the pole lookup grid

struct Writer {
    std::vector<u8>& out;
    template <typename T>
    void put(const T& v) {
        const usize at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &v, sizeof(T));
    }
};

struct Reader {
    const u8* p;
    const u8* end;
    bool ok = true;
    template <typename T>
    T get() {
        T v{};
        if (static_cast<usize>(end - p) < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
};

constexpr u32 kMagic = 0x43414646; // "FFAC"
constexpr u32 kVersion = 1;

} // namespace

struct Factory::Impl {
    // --- belts ---------------------------------------------------------------
    struct BeltTile {
        Dir dir = Dir::East;
        u8 speed = 8;
        u32 line = kNone;
        u32 index = 0; // place along its line, from the start
        u64 next = kNoTile; // the belt it hands items to
        u64 pred = kNoTile; // one of the belts handing items to it
        u8 in = 0;          // how many do
        u32 mark = 0, pred_mark = 0, touch = 0; // rebuild bookkeeping
    };
    struct Slot {
        u32 gap; // free steps in front of this item (to the line end for the first)
        ItemId item;
    };
    struct Line {
        bool alive = false;
        std::vector<u64> path; // tiles, start to end
        u32 length = 0;        // steps
        u32 speed = 8;
        f32 carry = 0; // part of a step left over (time not at full speed)
        std::vector<Slot> ring;
        u32 head = 0, count = 0;
        u32 first_moving = 0; // items before it are packed against the end
        u32 back_free = 0;    // steps from the last item to the line start
        u32 next = kNone;     // line its front items go on to
        MachineId sink = kNoMachine;
        MachineId source = kNoMachine;
        world::Rect box;

        Slot& at(u32 i) {
            u32 k = head + i;
            if (k >= ring.size()) k -= static_cast<u32>(ring.size());
            return ring[k];
        }
        const Slot& at(u32 i) const { return const_cast<Line*>(this)->at(i); }
        bool arrived() const { return count != 0 && at(0).gap == 0; }
        bool can_push() const { return count == 0 || (back_free >= kItemSpacing && count < ring.size()); }
        ItemId pop() {
            const ItemId item = at(0).item;
            if (++head == ring.size()) head = 0;
            --count;
            if (count != 0) at(0).gap += kItemSpacing;
            else back_free = length;
            first_moving = 0;
            return item;
        }
        void push(ItemId item) {
            const u32 gap = count == 0 ? length : back_free - kItemSpacing;
            u32 k = head + count;
            if (k >= ring.size()) k -= static_cast<u32>(ring.size());
            ring[k] = {gap, item};
            ++count;
            back_free = 0;
        }
        // Everything moves by the belt speed (times k, the share of a 1/60 s
        // tick that passed) except what is packed in front.
        void move(f32 k) {
            const f32 want = static_cast<f32>(speed) * k + carry;
            const u32 v = static_cast<u32>(want);
            carry = want - static_cast<f32>(v);
            if (count == 0 || v == 0) return;
            u32 prev = 0; // how far the item in front moved
            bool all = false;
            for (u32 i = first_moving; i < count; ++i) {
                Slot& s = at(i);
                const u32 moved = std::min(v, prev + s.gap);
                s.gap = s.gap + prev - moved;
                prev = moved;
                if (moved == v) {
                    all = true; // the rest move freely, gaps unchanged
                    break;
                }
            }
            back_free += all ? v : prev;
            while (first_moving < count && at(first_moving).gap == 0) ++first_moving;
        }
        // Positions (steps from the end) of every item, front first.
        void positions(std::vector<std::pair<u32, ItemId>>& out) const {
            u32 p = 0;
            for (u32 i = 0; i < count; ++i) {
                const Slot& s = at(i);
                p += s.gap + (i ? kItemSpacing : 0);
                out.push_back({p, s.item});
            }
        }
        // Replaces the items: sorted by position, kept apart, the rest dropped.
        void set_items(std::vector<std::pair<u32, ItemId>>& items) {
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            head = count = first_moving = 0;
            u32 last = 0;
            for (const auto& [p0, item] : items) {
                u32 p = p0;
                if (count != 0) p = std::max(p, last + kItemSpacing);
                if (p > length || count == ring.size()) break;
                ring[count] = {count == 0 ? p : p - last - kItemSpacing, item};
                ++count;
                last = p;
            }
            back_free = count == 0 ? length : length - last;
            while (first_moving < count && at(first_moving).gap == 0) ++first_moving;
        }
    };

    std::unordered_map<u64, BeltTile> belts;
    std::vector<Line> lines;
    std::vector<u32> free_lines;
    std::vector<u32> live_lines;   // alive line ids
    std::vector<u32> linked_lines; // lines with a next line
    bool lines_changed = false;    // the two lists above need a refresh
    std::unordered_set<u32> dirty_lines;
    std::vector<u64> pending_belts, removed_belts;
    std::vector<u32> relink_lines; // lines next to machines that came or went
    u32 epoch = 0;
    struct Snap {
        u64 tile;
        u32 offset; // steps from the tile's start
        ItemId item;
    };
    std::vector<Snap> loaded_items;
    u32 transfer_turn = 0;

    // --- machines --------------------------------------------------------------
    struct Machine {
        bool alive = false;
        MachineDesc d;
        MachineState s;
        bool want = false; // wants power next tick
        u32 net = kNone;
        u32 in_seg = kNone, out_seg = kNone;
        std::vector<u32> inputs, outputs; // lines
        u32 turn = 0;
        f32 fluid_in_cap = 0, fluid_out_cap = 0;
        FluidId fluid_in_kind() const { return d.kind == MachineKind::Generator ? d.fuel : d.recipe.fluid_in; }
        FluidId fluid_out_kind() const { return d.kind == MachineKind::Crafter ? d.recipe.fluid_out : 0; }
        bool needs_power() const { return d.kind == MachineKind::Generator || d.kind == MachineKind::Accumulator || (d.kind == MachineKind::Crafter && d.power > 0); }
    };
    std::vector<Machine> machines;
    std::vector<MachineId> free_machines;
    std::vector<MachineId> live_machines;
    std::unordered_map<u64, MachineId> machine_tiles;

    // --- power -------------------------------------------------------------------
    struct Pole {
        f32 supply, wire;
        u32 net = kNone;
    };
    std::unordered_map<u64, Pole> poles;
    std::unordered_map<u64, std::vector<u64>> pole_grid; // cell -> poles in it
    struct Net {
        bool alive = false;
        std::vector<u64> poles;
        std::vector<MachineId> consumers, generators, accumulators;
        f32 satisfaction = 1;
    };
    std::vector<Net> nets;
    std::vector<u32> free_nets, live_nets;
    std::unordered_set<u32> dirty_nets;
    std::vector<u64> pending_poles;
    std::vector<MachineId> pending_power; // machines to connect

    // --- pipes ---------------------------------------------------------------------
    std::unordered_map<u64, u32> pipes; // tile -> segment
    struct Segment {
        bool alive = false;
        bool relabel = false; // got its fluid this tick
        FluidId fluid = 0;
        f32 amount = 0, capacity = 0;
        std::vector<u64> tiles;
        std::vector<MachineId> producers, consumers;
    };
    std::vector<Segment> segments;
    std::vector<u32> free_segs, live_segs;
    std::unordered_set<u32> dirty_segs;
    std::vector<u64> pending_pipes;
    std::vector<MachineId> pending_ports; // machines to connect to pipes
    struct FluidSnap {
        u64 tile;
        FluidId fluid;
        f32 amount;
    };
    std::vector<FluidSnap> loaded_fluids;
    std::atomic<bool> relabeled{false};
    FactoryStats stats;

    bool occupied(u64 k) const {
        return belts.count(k) || pipes.count(k) || poles.count(k) || machine_tiles.count(k);
    }

    template <typename T, typename Fn>
    static u32 take_id(std::vector<T>& list, std::vector<u32>& free, Fn&& reset) {
        u32 id;
        if (!free.empty()) {
            id = free.back();
            free.pop_back();
        } else {
            id = static_cast<u32>(list.size());
            list.emplace_back();
        }
        reset(list[id]);
        return id;
    }
    template <typename T>
    static void erase_value(std::vector<T>& v, T x) {
        auto it = std::find(v.begin(), v.end(), x);
        if (it != v.end()) {
            *it = v.back();
            v.pop_back();
        }
    }
    // Tiles around a machine's footprint.
    template <typename Fn>
    static void each_around(const MachineDesc& d, Fn&& fn) {
        for (i32 x = d.x; x < d.x + d.w; ++x) {
            fn(x, d.y - 1);
            fn(x, d.y + d.h);
        }
        for (i32 y = d.y; y < d.y + d.h; ++y) {
            fn(d.x - 1, y);
            fn(d.x + d.w, y);
        }
    }

    // ---------------------------------------------------------------------------
    // Belt topology

    // The belt tile a belt hands its items to (kNoTile at a dead end).
    u64 find_next(u64 t, const BeltTile& b) const {
        const u64 n = step(t, b.dir);
        auto it = belts.find(n);
        if (it == belts.end() || it->second.dir == opposite(b.dir)) return kNoTile;
        return n;
    }
    // Who hands items to t: how many, and the last one found.
    void find_incoming(u64 t, BeltTile& b) const {
        b.in = 0;
        b.pred = kNoTile;
        for (u8 d = 0; d < 4; ++d) {
            const u64 q = step(t, static_cast<Dir>(d));
            auto it = belts.find(q);
            if (it != belts.end() && it->second.next == t) {
                ++b.in;
                b.pred = q;
            }
        }
    }
    // The tile a's items run on to within one line, if any.
    BeltTile* chain_next(const BeltTile& a) {
        if (a.next == kNoTile) return nullptr;
        auto it = belts.find(a.next);
        if (it == belts.end() || it->second.speed != a.speed || it->second.in != 1) return nullptr;
        return &it->second;
    }

    void mark_around(i32 x, i32 y) {
        for (i32 d = -1; d < 4; ++d) {
            const u64 k = d < 0 ? key(x, y) : key(x + kDx[d], y + kDy[d]);
            auto it = belts.find(k);
            if (it != belts.end() && it->second.line != kNone) dirty_lines.insert(it->second.line);
        }
    }

    void unlink(u32 l) {
        Line& line = lines[l];
        if (line.sink != kNoMachine) erase_value(machines[line.sink].inputs, l);
        if (line.source != kNoMachine) erase_value(machines[line.source].outputs, l);
        line.sink = line.source = kNoMachine;
        line.next = kNone;
    }

    // Where a line's items go at its end, and which machine feeds its start.
    void link(u32 l) {
        unlink(l);
        Line& line = lines[l];
        const u64 end = line.path.back();
        const BeltTile& b = belts.at(end);
        if (b.next != kNoTile) {
            line.next = belts.at(b.next).line;
        } else {
            auto mt = machine_tiles.find(step(end, b.dir));
            if (mt != machine_tiles.end()) {
                const MachineKind k = machines[mt->second].d.kind;
                if (k == MachineKind::Crafter || k == MachineKind::Sink) {
                    line.sink = mt->second;
                    machines[mt->second].inputs.push_back(l);
                }
            }
        }
        const u64 start = line.path.front();
        auto mt = machine_tiles.find(step(start, opposite(belts.at(start).dir)));
        if (mt != machine_tiles.end() && machines[mt->second].d.kind == MachineKind::Crafter) {
            line.source = mt->second;
            machines[mt->second].outputs.push_back(l);
        }
        lines_changed = true;
    }

    // Belts touching a machine link again (it came or went), at the next rebuild.
    void link_around(const MachineDesc& d) {
        each_around(d, [&](i32 x, i32 y) {
            auto it = belts.find(key(x, y));
            if (it != belts.end() && it->second.line != kNone) relink_lines.push_back(it->second.line);
        });
    }

    // Rebuilds the lines next to changed belts, keeping their items.
    bool rebuild_belts() {
        if (dirty_lines.empty() && pending_belts.empty() && removed_belts.empty() && loaded_items.empty())
            return false;
        const u32 e = ++epoch;

        // Fresh next / incoming around every edit.
        std::vector<std::pair<u64, BeltTile*>> touched;
        auto touch = [&](u64 k) {
            auto it = belts.find(k);
            if (it == belts.end() || it->second.touch == e) return;
            it->second.touch = e;
            touched.push_back({k, &it->second});
        };
        for (const auto* list : {&pending_belts, &removed_belts})
            for (u64 k : *list) {
                touch(k);
                for (u8 d = 0; d < 4; ++d) touch(step(k, static_cast<Dir>(d)));
            }
        for (auto& [k, b] : touched) b->next = find_next(k, *b);
        for (auto& [k, b] : touched) find_incoming(k, *b);
        removed_belts.clear();

        std::vector<u64> set, work;
        auto add_line = [&](u32 l) {
            if (!dirty_lines.insert(l).second) return;
            for (u64 t : lines[l].path) work.push_back(t);
        };
        for (u32 l : dirty_lines)
            for (u64 t : lines[l].path) work.push_back(t);
        work.insert(work.end(), pending_belts.begin(), pending_belts.end());
        pending_belts.clear();
        // Lines outside whose ends now run on into (or receive from) the
        // rebuilt tiles join in.
        while (!work.empty()) {
            const u64 t = work.back();
            work.pop_back();
            auto it = belts.find(t);
            if (it == belts.end() || it->second.mark == e) continue;
            BeltTile& b = it->second;
            b.mark = e;
            set.push_back(t);
            if (BeltTile* n = chain_next(b); n && n->line != kNone) add_line(n->line);
            if (b.in == 1) {
                BeltTile& p = belts.at(b.pred);
                if (p.line != kNone && chain_next(p) == &b) add_line(p.line);
            }
        }

        // Items of the old lines, by tile.
        std::vector<Snap> snaps = std::move(loaded_items);
        loaded_items.clear();
        std::vector<std::pair<u32, ItemId>> pos;
        for (u32 l : dirty_lines) {
            Line& line = lines[l];
            if (!line.alive) continue;
            unlink(l);
            pos.clear();
            line.positions(pos);
            for (const auto& [p, item] : pos) {
                const u32 d = line.length - p;
                const u32 idx = std::min<u32>(d / kBeltTileLength, static_cast<u32>(line.path.size()) - 1);
                snaps.push_back({line.path[idx], d - idx * kBeltTileLength, item});
            }
            for (u64 t : line.path) {
                auto it = belts.find(t);
                if (it != belts.end()) it->second.line = kNone;
            }
            line = Line{};
            free_lines.push_back(l);
        }
        dirty_lines.clear();

        // New lines over the set: chains from their starts, then loops.
        for (u64 t : set) {
            BeltTile* n = chain_next(belts.at(t));
            if (n && n->mark == e) n->pred_mark = e;
        }
        std::vector<u32> made;
        auto walk = [&](u64 start) {
            const u32 id = take_id(lines, free_lines, [](Line& l) {
                l = Line{};
                l.alive = true;
            });
            made.push_back(id);
            Line& line = lines[id];
            BeltTile* b = &belts.at(start);
            line.speed = b->speed;
            line.box = {key_x(start), key_y(start), key_x(start) + 1, key_y(start) + 1};
            for (u64 t = start;;) {
                b->line = id;
                b->index = static_cast<u32>(line.path.size());
                line.path.push_back(t);
                line.box.x0 = std::min(line.box.x0, key_x(t));
                line.box.y0 = std::min(line.box.y0, key_y(t));
                line.box.x1 = std::max(line.box.x1, key_x(t) + 1);
                line.box.y1 = std::max(line.box.y1, key_y(t) + 1);
                BeltTile* n = chain_next(*b);
                if (!n || n->mark != e || n->line != kNone) break; // the end, or back at a loop's start
                t = b->next;
                b = n;
            }
            line.length = static_cast<u32>(line.path.size()) * kBeltTileLength;
            line.ring.resize(line.length / kItemSpacing + 1);
            line.back_free = line.length;
        };
        for (u64 t : set)
            if (belts.at(t).pred_mark != e) walk(t);
        for (u64 t : set)
            if (belts.at(t).line == kNone) walk(t);

        // Items back, by their new lines.
        std::unordered_map<u32, std::vector<std::pair<u32, ItemId>>> by_line;
        for (const Snap& s : snaps) {
            auto it = belts.find(s.tile);
            if (it == belts.end() || it->second.line == kNone) continue;
            const Line& line = lines[it->second.line];
            const u32 d = std::min(it->second.index * kBeltTileLength + s.offset, line.length);
            by_line[it->second.line].push_back({line.length - d, s.item});
        }
        for (auto& [l, items] : by_line) lines[l].set_items(items);

        // Links: the new lines, and lines outside that run into them.
        for (u32 l : made) link(l);
        for (u64 t : set) {
            const BeltTile& b = belts.at(t);
            if (b.in == 0 || (b.in == 1 && belts.at(b.pred).mark == e)) continue;
            for (u8 d = 0; d < 4; ++d) {
                const u64 q = step(t, static_cast<Dir>(d));
                auto it = belts.find(q);
                if (it == belts.end() || it->second.mark == e || it->second.line == kNone) continue;
                if (it->second.next == t) link(it->second.line);
            }
        }
        lines_changed = true;
        return true;
    }

    void refresh_line_lists() {
        live_lines.clear();
        linked_lines.clear();
        for (u32 i = 0; i < lines.size(); ++i) {
            if (!lines[i].alive) continue;
            live_lines.push_back(i);
            if (lines[i].next != kNone) linked_lines.push_back(i);
        }
        lines_changed = false;
    }

    // ---------------------------------------------------------------------------
    // Power networks: poles joined by wires; machines join through a pole
    // whose supply square touches them.

    static u64 pole_cell(i32 x, i32 y) {
        return key(static_cast<i32>(std::floor(static_cast<f32>(x) / kPoleCell)),
                   static_cast<i32>(std::floor(static_cast<f32>(y) / kPoleCell)));
    }
    template <typename Fn>
    void poles_near(f32 x, f32 y, Fn&& fn) const {
        const i32 cx = static_cast<i32>(std::floor(x / kPoleCell)), cy = static_cast<i32>(std::floor(y / kPoleCell));
        for (i32 dy = -1; dy <= 1; ++dy)
            for (i32 dx = -1; dx <= 1; ++dx) {
                auto it = pole_grid.find(key(cx + dx, cy + dy));
                if (it == pole_grid.end()) continue;
                for (u64 p : it->second)
                    if (!fn(p)) return;
            }
    }

    void detach(MachineId m) {
        Machine& mc = machines[m];
        if (mc.net == kNone) return;
        Net& net = nets[mc.net];
        erase_value(net.consumers, m);
        erase_value(net.generators, m);
        erase_value(net.accumulators, m);
        mc.net = kNone;
    }

    // Connects a machine through any pole that reaches it.
    void attach(MachineId m) {
        Machine& mc = machines[m];
        if (!mc.alive || !mc.needs_power() || mc.net != kNone) return;
        const f32 x0 = static_cast<f32>(mc.d.x), y0 = static_cast<f32>(mc.d.y);
        const f32 x1 = x0 + mc.d.w, y1 = y0 + mc.d.h;
        poles_near((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, [&](u64 k) {
            const Pole& p = poles.at(k);
            const f32 px = static_cast<f32>(key_x(k)) + 0.5f, py = static_cast<f32>(key_y(k)) + 0.5f;
            if (p.net == kNone || !(px + p.supply > x0 && px - p.supply < x1 && py + p.supply > y0 && py - p.supply < y1))
                return true;
            mc.net = p.net;
            return false;
        });
        if (mc.net == kNone) return;
        Net& net = nets[mc.net];
        if (mc.d.kind == MachineKind::Crafter) net.consumers.push_back(m);
        else if (mc.d.kind == MachineKind::Generator) net.generators.push_back(m);
        else net.accumulators.push_back(m);
    }

    // Rebuilds the networks touched by pole changes.
    bool rebuild_power() {
        if (dirty_nets.empty() && pending_poles.empty() && pending_power.empty()) return false;
        std::unordered_map<u64, u32> index; // affected pole -> union-find slot
        std::vector<u64> list, work;
        auto add_net = [&](u32 n) {
            if (!dirty_nets.insert(n).second) return;
            for (u64 p : nets[n].poles) work.push_back(p);
        };
        for (u32 n : dirty_nets)
            for (u64 p : nets[n].poles) work.push_back(p);
        for (u64 p : pending_poles) work.push_back(p);
        std::vector<MachineId> reattach = std::move(pending_power);
        pending_power.clear();
        // New poles pull in every network their wires reach, and machines
        // in their supply square.
        for (u64 k : pending_poles) {
            auto it = poles.find(k);
            if (it == poles.end()) continue;
            const Pole& a = it->second;
            const f32 x = static_cast<f32>(key_x(k)), y = static_cast<f32>(key_y(k));
            poles_near(x, y, [&](u64 o) {
                const Pole& b = poles.at(o);
                const f32 reach = std::min(a.wire, b.wire);
                const f32 dx = static_cast<f32>(key_x(o)) - x, dy = static_cast<f32>(key_y(o)) - y;
                if (b.net != kNone && dx * dx + dy * dy <= reach * reach) add_net(b.net);
                return true;
            });
            const i32 s = static_cast<i32>(std::ceil(a.supply));
            for (i32 ty = key_y(k) - s; ty <= key_y(k) + s; ++ty)
                for (i32 tx = key_x(k) - s; tx <= key_x(k) + s; ++tx) {
                    auto mt = machine_tiles.find(key(tx, ty));
                    if (mt != machine_tiles.end() && machines[mt->second].net == kNone) reattach.push_back(mt->second);
                }
        }
        pending_poles.clear();
        for (u64 k : work)
            if (poles.count(k) && !index.count(k)) {
                index[k] = static_cast<u32>(list.size());
                list.push_back(k);
            }
        for (u32 n : dirty_nets) {
            Net& net = nets[n];
            if (!net.alive) continue;
            for (auto* v : {&net.consumers, &net.generators, &net.accumulators})
                for (MachineId m : *v) {
                    machines[m].net = kNone;
                    reattach.push_back(m);
                }
            net = Net{};
            free_nets.push_back(n);
        }
        dirty_nets.clear();

        std::vector<u32> parent(list.size());
        for (u32 i = 0; i < parent.size(); ++i) parent[i] = i;
        auto find = [&](u32 a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        };
        for (u32 i = 0; i < list.size(); ++i) {
            const Pole& a = poles.at(list[i]);
            const f32 x = static_cast<f32>(key_x(list[i])), y = static_cast<f32>(key_y(list[i]));
            poles_near(x, y, [&](u64 o) {
                auto j = index.find(o);
                if (j == index.end() || j->second <= i) return true;
                const Pole& b = poles.at(o);
                const f32 reach = std::min(a.wire, b.wire);
                const f32 dx = static_cast<f32>(key_x(o)) - x, dy = static_cast<f32>(key_y(o)) - y;
                if (dx * dx + dy * dy <= reach * reach) parent[find(i)] = find(j->second);
                return true;
            });
        }
        std::unordered_map<u32, u32> net_of;
        for (u32 i = 0; i < list.size(); ++i) {
            const u32 r = find(i);
            auto it = net_of.find(r);
            if (it == net_of.end())
                it = net_of.emplace(r, take_id(nets, free_nets, [](Net& n) {
                                        n = Net{};
                                        n.alive = true;
                                    })).first;
            poles.at(list[i]).net = it->second;
            nets[it->second].poles.push_back(list[i]);
        }
        for (MachineId m : reattach) attach(m);
        live_nets.clear();
        for (u32 i = 0; i < nets.size(); ++i)
            if (nets[i].alive) live_nets.push_back(i);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Pipe segments: connected pipe tiles, sharing one fluid.

    void unport(MachineId m) {
        Machine& mc = machines[m];
        if (mc.in_seg != kNone && segments[mc.in_seg].alive) erase_value(segments[mc.in_seg].consumers, m);
        if (mc.out_seg != kNone && segments[mc.out_seg].alive) erase_value(segments[mc.out_seg].producers, m);
        mc.in_seg = mc.out_seg = kNone;
    }

    // Which segment a machine takes fluid from and gives it to: the one
    // holding that fluid, or for output an empty one (which then takes it).
    void port(MachineId m) {
        unport(m);
        Machine& mc = machines[m];
        if (!mc.alive) return;
        const FluidId fin = mc.fluid_in_kind(), fout = mc.fluid_out_kind();
        if (fin == 0 && fout == 0) return;
        u32 near[16];
        u32 count = 0;
        each_around(mc.d, [&](i32 x, i32 y) {
            auto it = pipes.find(key(x, y));
            if (it == pipes.end() || it->second == kNone || count == 16) return;
            if (std::find(near, near + count, it->second) == near + count) near[count++] = it->second;
        });
        for (u32 i = 0; i < count && fin != 0; ++i)
            if (segments[near[i]].fluid == fin) {
                mc.in_seg = near[i];
                break;
            }
        for (u32 i = 0; i < count && fout != 0; ++i)
            if (segments[near[i]].fluid == fout) {
                mc.out_seg = near[i];
                break;
            }
        for (u32 i = 0; i < count && fout != 0 && mc.out_seg == kNone; ++i)
            if (segments[near[i]].fluid == 0 && near[i] != mc.in_seg) mc.out_seg = near[i];
        if (mc.in_seg != kNone) segments[mc.in_seg].consumers.push_back(m);
        if (mc.out_seg != kNone) segments[mc.out_seg].producers.push_back(m);
    }

    template <typename Fn>
    void machines_around_tile(u64 t, Fn&& fn) {
        for (u8 d = 0; d < 4; ++d) {
            auto mt = machine_tiles.find(step(t, static_cast<Dir>(d)));
            if (mt != machine_tiles.end()) fn(mt->second);
        }
    }

    bool rebuild_pipes() {
        if (dirty_segs.empty() && pending_pipes.empty() && loaded_fluids.empty() && pending_ports.empty() &&
            !relabeled.load())
            return false;
        std::vector<MachineId> reports = std::move(pending_ports);
        pending_ports.clear();
        if (relabeled.exchange(false))
            for (u32 s : live_segs)
                if (segments[s].relabel) {
                    segments[s].relabel = false;
                    for (u64 t : segments[s].tiles) machines_around_tile(t, [&](MachineId m) { reports.push_back(m); });
                }

        // What each tile held, to share out over the new segments.
        std::unordered_map<u64, std::pair<FluidId, f32>> share;
        std::vector<u64> set = std::move(pending_pipes);
        pending_pipes.clear();
        for (u32 s : dirty_segs) {
            Segment& seg = segments[s];
            if (!seg.alive) continue;
            const f32 each = seg.tiles.empty() ? 0 : seg.amount / static_cast<f32>(seg.tiles.size());
            for (u64 t : seg.tiles) {
                set.push_back(t);
                share[t] = {seg.fluid, each};
            }
            for (MachineId m : seg.producers) reports.push_back(m);
            for (MachineId m : seg.consumers) reports.push_back(m);
        }
        for (const FluidSnap& f : loaded_fluids) share[f.tile] = {f.fluid, f.amount};
        loaded_fluids.clear();
        for (u32 s : dirty_segs) {
            if (!segments[s].alive) continue;
            for (MachineId m : segments[s].producers) machines[m].out_seg = kNone;
            for (MachineId m : segments[s].consumers) machines[m].in_seg = kNone;
            segments[s] = Segment{};
            free_segs.push_back(s);
        }
        dirty_segs.clear();
        for (u64 t : set) {
            auto it = pipes.find(t);
            if (it != pipes.end()) it->second = kNone;
        }

        std::vector<u64> stack;
        for (u64 k0 : set) {
            auto it0 = pipes.find(k0);
            if (it0 == pipes.end() || it0->second != kNone) continue;
            const u32 id = take_id(segments, free_segs, [](Segment& s) {
                s = Segment{};
                s.alive = true;
            });
            Segment& seg = segments[id];
            std::unordered_map<FluidId, f32> held;
            it0->second = id;
            stack.push_back(k0);
            while (!stack.empty()) {
                const u64 t = stack.back();
                stack.pop_back();
                seg.tiles.push_back(t);
                machines_around_tile(t, [&](MachineId m) { reports.push_back(m); });
                auto sh = share.find(t);
                if (sh != share.end() && sh->second.first != 0) held[sh->second.first] += sh->second.second;
                for (u8 d = 0; d < 4; ++d) {
                    auto it = pipes.find(step(t, static_cast<Dir>(d)));
                    if (it == pipes.end() || it->second != kNone) continue;
                    it->second = id;
                    stack.push_back(it->first);
                }
            }
            seg.capacity = static_cast<f32>(seg.tiles.size()) * kPipeTileCapacity;
            for (const auto& [fluid, amount] : held)
                if (seg.fluid == 0 || amount > seg.amount) {
                    seg.fluid = fluid;
                    seg.amount = amount;
                }
            seg.amount = std::min(seg.amount, seg.capacity);
        }
        std::sort(reports.begin(), reports.end());
        reports.erase(std::unique(reports.begin(), reports.end()), reports.end());
        for (MachineId m : reports) port(m);
        live_segs.clear();
        for (u32 i = 0; i < segments.size(); ++i)
            if (segments[i].alive) live_segs.push_back(i);
        return true;
    }

    // ---------------------------------------------------------------------------

    MachineId add_machine(const MachineDesc& d) {
        const u8 w = std::max<u8>(d.w, 1), h = std::max<u8>(d.h, 1);
        for (i32 y = d.y; y < d.y + h; ++y)
            for (i32 x = d.x; x < d.x + w; ++x)
                if (occupied(key(x, y))) return kNoMachine;
        const MachineId id = take_id(machines, free_machines, [](Machine& m) { m = Machine{}; });
        Machine& m = machines[id];
        m.alive = true;
        m.d = d;
        m.d.w = w;
        m.d.h = h;
        m.s.alive = true;
        if (d.kind == MachineKind::Generator) {
            m.fluid_in_cap = d.fuel_energy > 0 ? d.power / d.fuel_energy : 0; // a second at full power
        } else {
            m.fluid_in_cap = d.recipe.fluid_in_amount * 2;
            m.fluid_out_cap = d.recipe.fluid_out_amount * 2;
        }
        for (i32 y = m.d.y; y < m.d.y + m.d.h; ++y)
            for (i32 x = m.d.x; x < m.d.x + m.d.w; ++x) machine_tiles[key(x, y)] = id;
        live_machines.push_back(id);
        link_around(m.d);
        pending_power.push_back(id);
        pending_ports.push_back(id);
        return id;
    }

    bool remove_machine(MachineId id) {
        if (id >= machines.size() || !machines[id].alive) return false;
        Machine& mc = machines[id];
        detach(id);
        unport(id);
        for (u32 l : std::vector<u32>(mc.inputs)) unlink(l);
        for (u32 l : std::vector<u32>(mc.outputs)) unlink(l);
        for (i32 y = mc.d.y; y < mc.d.y + mc.d.h; ++y)
            for (i32 x = mc.d.x; x < mc.d.x + mc.d.w; ++x) machine_tiles.erase(key(x, y));
        const MachineDesc d = mc.d;
        mc = Machine{};
        free_machines.push_back(id);
        erase_value(live_machines, id);
        link_around(d); // what ran into it now ends there
        return true;
    }

    void rebuild() {
        const u64 t0 = time_now_ns();
        u32 n = 0;
        n += rebuild_belts();
        for (u32 l : relink_lines)
            if (lines[l].alive) link(l);
        relink_lines.clear();
        if (lines_changed) refresh_line_lists();
        n += rebuild_power();
        n += rebuild_pipes();
        stats.rebuilds = n;
        stats.rebuild_ms = ns_to_ms(time_now_ns() - t0);
    }

    // ---------------------------------------------------------------------------
    // Tick phases

    void step_power(f32 dt) {
        jobs::parallel_for(static_cast<u32>(live_nets.size()), 16, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                Net& net = nets[live_nets[i]];
                f32 demand = 0, supply = 0, discharge = 0, charge_cap = 0;
                for (MachineId m : net.consumers)
                    if (machines[m].want) demand += machines[m].d.power;
                auto available = [&](const Machine& g) {
                    if (g.d.fuel == 0) return g.d.power;
                    return std::min(g.d.power, g.s.fluid_in * g.d.fuel_energy / dt);
                };
                for (MachineId m : net.generators) supply += available(machines[m]);
                for (MachineId m : net.accumulators) {
                    const Machine& a = machines[m];
                    discharge += std::min(a.d.power, a.s.stored / dt);
                    charge_cap += std::min(a.d.power, (a.d.capacity - a.s.stored) / dt);
                }
                f32 load, sat, acc_rate; // acc_rate > 0 charges, < 0 gives
                if (supply >= demand) {
                    sat = 1;
                    const f32 charge = std::min(supply - demand, charge_cap);
                    load = supply > 0 ? (demand + charge) / supply : 0;
                    acc_rate = charge_cap > 0 ? charge / charge_cap : 0;
                } else {
                    const f32 give = std::min(demand - supply, discharge);
                    sat = demand > 0 ? (supply + give) / demand : 1;
                    load = 1;
                    acc_rate = discharge > 0 ? -give / discharge : 0;
                }
                net.satisfaction = sat;
                for (MachineId m : net.generators) {
                    Machine& g = machines[m];
                    const f32 out = available(g) * load;
                    g.s.output = out;
                    if (g.d.fuel != 0 && g.d.fuel_energy > 0)
                        g.s.fluid_in = std::max(0.0f, g.s.fluid_in - out * dt / g.d.fuel_energy);
                    g.s.satisfaction = load;
                }
                for (MachineId m : net.accumulators) {
                    Machine& a = machines[m];
                    const f32 rate = acc_rate >= 0 ? std::min(a.d.power, (a.d.capacity - a.s.stored) / dt) * acc_rate
                                                   : std::min(a.d.power, a.s.stored / dt) * acc_rate;
                    a.s.stored = std::clamp(a.s.stored + rate * dt, 0.0f, a.d.capacity);
                    a.s.output = -rate;
                }
                for (MachineId m : net.consumers) machines[m].s.satisfaction = sat;
            }
        });
    }

    void step_belts(f32 share) {
        std::atomic<u32> items{0};
        jobs::parallel_for(static_cast<u32>(live_lines.size()), 256, [&](u32 b, u32 e) {
            u32 n = 0;
            for (u32 i = b; i < e; ++i) {
                Line& l = lines[live_lines[i]];
                l.move(share);
                n += l.count;
            }
            items.fetch_add(n, std::memory_order_relaxed);
        });
        stats.items = items.load();
        // Hand-overs between lines: few, in turn so merging belts share.
        const usize n = linked_lines.size();
        if (n == 0) return;
        const usize first = transfer_turn++ % n;
        for (usize k = 0; k < n; ++k) {
            Line& l = lines[linked_lines[(first + k) % n]];
            if (!l.arrived()) continue;
            Line& t = lines[l.next];
            if (&t == &l) {
                if (l.count == 1 || l.back_free >= kItemSpacing) l.push(l.pop());
            } else if (t.can_push()) {
                t.push(l.pop());
            }
        }
    }

    bool can_start(const Machine& m) const {
        const Recipe& r = m.d.recipe;
        for (const Recipe::Part& p : r.in)
            if (p.count != 0) {
                const u32 slot = static_cast<u32>(&p - r.in);
                if (m.s.in[slot] < p.count) return false;
            }
        if (r.fluid_in != 0 && m.s.fluid_in < r.fluid_in_amount) return false;
        if (r.out.count != 0 && m.s.out + r.out.count > std::max<u32>(r.out.count * 2u, 4u)) return false;
        if (r.fluid_out != 0 && m.s.fluid_out + r.fluid_out_amount > m.fluid_out_cap) return false;
        return true;
    }

    void step_machines(f32 dt) {
        // Take from belts and craft. Each belt runs into one machine at most.
        jobs::parallel_for(static_cast<u32>(live_machines.size()), 64, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                Machine& m = machines[live_machines[i]];
                const Recipe& r = m.d.recipe;
                for (u32 l : m.inputs) {
                    Line& line = lines[l];
                    if (!line.arrived()) continue;
                    const ItemId item = line.at(0).item;
                    if (m.d.kind == MachineKind::Sink) {
                        line.pop();
                        ++m.s.made;
                        continue;
                    }
                    for (u32 k = 0; k < 3; ++k)
                        if (r.in[k].count != 0 && r.in[k].item == item && m.s.in[k] < r.in[k].count * 2u + 1u) {
                            line.pop();
                            ++m.s.in[k];
                            break;
                        }
                }
                if (m.d.kind != MachineKind::Crafter) continue;
                if (!m.s.working && can_start(m)) {
                    for (u32 k = 0; k < 3; ++k) m.s.in[k] = static_cast<u16>(m.s.in[k] - r.in[k].count);
                    m.s.fluid_in -= r.fluid_in_amount;
                    m.s.working = true;
                }
                m.want = m.s.working;
                if (!m.s.working) continue;
                const f32 speed = m.d.power > 0 ? (m.net == kNone ? 0.0f : m.s.satisfaction) : 1.0f;
                m.s.progress += dt / std::max(r.seconds, 1e-3f) * speed;
                if (m.s.progress >= 1) {
                    m.s.out = static_cast<u16>(m.s.out + r.out.count);
                    m.s.fluid_out += r.fluid_out_amount;
                    ++m.s.made;
                    m.s.progress = 0;
                    m.s.working = false;
                }
            }
        });
        // Put results on belts. Each belt starts at one machine at most.
        jobs::parallel_for(static_cast<u32>(live_machines.size()), 64, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                Machine& m = machines[live_machines[i]];
                const usize n = m.outputs.size();
                for (usize k = 0; k < n && m.s.out != 0; ++k) {
                    Line& line = lines[m.outputs[(m.turn + k) % n]];
                    if (!line.can_push()) continue;
                    line.push(m.d.recipe.out.item);
                    --m.s.out;
                    ++m.turn;
                }
            }
        });
    }

    void step_fluids() {
        jobs::parallel_for(static_cast<u32>(live_segs.size()), 64, [&](u32 b, u32 e) {
            for (u32 i = b; i < e; ++i) {
                Segment& seg = segments[live_segs[i]];
                for (MachineId m : seg.producers) {
                    Machine& p = machines[m];
                    const FluidId f = p.d.recipe.fluid_out;
                    if (p.s.fluid_out <= 0 || (seg.fluid != 0 && seg.fluid != f)) continue;
                    if (seg.fluid == 0) {
                        // Machines around may take it from here now.
                        seg.fluid = f;
                        seg.relabel = true;
                        relabeled.store(true, std::memory_order_relaxed);
                    }
                    const f32 put = std::min(p.s.fluid_out, seg.capacity - seg.amount);
                    p.s.fluid_out -= put;
                    seg.amount += put;
                }
                if (seg.amount <= 0 || seg.consumers.empty()) continue;
                f32 want = 0;
                for (MachineId m : seg.consumers)
                    if (machines[m].fluid_in_kind() == seg.fluid)
                        want += std::max(0.0f, machines[m].fluid_in_cap - machines[m].s.fluid_in);
                if (want <= 0) continue;
                const f32 share = std::min(1.0f, seg.amount / want);
                for (MachineId m : seg.consumers) {
                    Machine& c = machines[m];
                    if (c.fluid_in_kind() != seg.fluid) continue;
                    const f32 give = std::max(0.0f, c.fluid_in_cap - c.s.fluid_in) * share;
                    c.s.fluid_in += give;
                    seg.amount = std::max(0.0f, seg.amount - give);
                }
            }
        });
    }

    void tick(f32 dt) {
        FORGE_ZONE_N("Factory");
        const u64 t0 = time_now_ns();
        rebuild();
        const u64 t1 = time_now_ns();
        if (dt <= 0) {
            // Only apply building changes (and refresh the counts).
            u32 items = 0;
            for (u32 l : live_lines) items += lines[l].count;
            stats.items = items;
            stats.total_ms = ns_to_ms(t1 - t0);
            stats.belt_tiles = static_cast<u32>(belts.size());
            stats.lines = static_cast<u32>(live_lines.size());
            stats.machines = static_cast<u32>(live_machines.size());
            stats.power_nets = static_cast<u32>(live_nets.size());
            stats.pipe_segments = static_cast<u32>(live_segs.size());
            return;
        }
        step_power(dt);
        const u64 t2 = time_now_ns();
        step_belts(dt * 60.0f);
        const u64 t3 = time_now_ns();
        step_machines(dt);
        const u64 t4 = time_now_ns();
        step_fluids();
        const u64 t5 = time_now_ns();
        stats.power_ms = ns_to_ms(t2 - t1);
        stats.belts_ms = ns_to_ms(t3 - t2);
        stats.machines_ms = ns_to_ms(t4 - t3);
        stats.fluids_ms = ns_to_ms(t5 - t4);
        stats.total_ms = ns_to_ms(t5 - t0);
        stats.belt_tiles = static_cast<u32>(belts.size());
        stats.lines = static_cast<u32>(live_lines.size());
        stats.machines = static_cast<u32>(live_machines.size());
        stats.power_nets = static_cast<u32>(live_nets.size());
        stats.pipe_segments = static_cast<u32>(live_segs.size());
    }
};

// -----------------------------------------------------------------------------

Factory::Factory() : impl_(new Impl) {}
Factory::~Factory() = default;

bool Factory::place_belt(i32 x, i32 y, Dir dir, u8 speed) {
    Impl& m = *impl_;
    const u64 k = key(x, y);
    if (m.occupied(k)) return false;
    m.mark_around(x, y);
    m.belts[k] = {dir, std::max<u8>(speed, 1)};
    m.pending_belts.push_back(k);
    return true;
}

bool Factory::remove_belt(i32 x, i32 y) {
    Impl& m = *impl_;
    if (!m.belts.count(key(x, y))) return false;
    m.mark_around(x, y);
    m.belts.erase(key(x, y));
    m.removed_belts.push_back(key(x, y));
    return true;
}

bool Factory::place_pipe(i32 x, i32 y) {
    Impl& m = *impl_;
    if (m.occupied(key(x, y))) return false;
    m.pipes[key(x, y)] = kNone;
    m.pending_pipes.push_back(key(x, y));
    for (u8 d = 0; d < 4; ++d) {
        auto it = m.pipes.find(key(x + kDx[d], y + kDy[d]));
        if (it != m.pipes.end() && it->second != kNone) m.dirty_segs.insert(it->second);
    }
    return true;
}

bool Factory::remove_pipe(i32 x, i32 y) {
    Impl& m = *impl_;
    auto it = m.pipes.find(key(x, y));
    if (it == m.pipes.end()) return false;
    if (it->second != kNone) m.dirty_segs.insert(it->second);
    m.pipes.erase(it);
    return true;
}

bool Factory::place_pole(i32 x, i32 y, f32 supply, f32 wire) {
    Impl& m = *impl_;
    if (m.occupied(key(x, y))) return false;
    m.poles[key(x, y)] = {std::min(supply, static_cast<f32>(kPoleCell)), std::min(wire, static_cast<f32>(kPoleCell)), kNone};
    m.pole_grid[Impl::pole_cell(x, y)].push_back(key(x, y));
    m.pending_poles.push_back(key(x, y));
    return true;
}

bool Factory::remove_pole(i32 x, i32 y) {
    Impl& m = *impl_;
    auto it = m.poles.find(key(x, y));
    if (it == m.poles.end()) return false;
    if (it->second.net != kNone) {
        m.dirty_nets.insert(it->second.net);
        Impl::erase_value(m.nets[it->second.net].poles, key(x, y));
    }
    m.poles.erase(it);
    Impl::erase_value(m.pole_grid[Impl::pole_cell(x, y)], key(x, y));
    return true;
}

MachineId Factory::place_machine(const MachineDesc& desc) { return impl_->add_machine(desc); }

bool Factory::remove_machine(MachineId id) { return impl_->remove_machine(id); }

bool Factory::insert_item(i32 x, i32 y, ItemId item) {
    Impl& m = *impl_;
    auto it = m.belts.find(key(x, y));
    if (it == m.belts.end() || it->second.line == kNone || item == 0) return false;
    Impl::Line& line = m.lines[it->second.line];
    const u32 p = line.length - (it->second.index * kBeltTileLength + kBeltTileLength / 2);
    std::vector<std::pair<u32, ItemId>> items;
    line.positions(items);
    for (const auto& [q, other] : items)
        if ((q > p ? q - p : p - q) < kItemSpacing) return false;
    if (items.size() >= line.ring.size()) return false;
    items.push_back({p, item});
    line.set_items(items);
    return true;
}

void Factory::tick(f32 dt) { impl_->tick(dt); }

void Factory::for_each_item(const world::Rect& r, const std::function<void(f32, f32, ItemId)>& fn) const {
    const Impl& m = *impl_;
    std::vector<std::pair<u32, ItemId>> pos;
    for (u32 l : m.live_lines) {
        const Impl::Line& line = m.lines[l];
        if (line.count == 0 || line.box.x1 <= r.x0 || line.box.x0 >= r.x1 || line.box.y1 <= r.y0 || line.box.y0 >= r.y1)
            continue;
        pos.clear();
        line.positions(pos);
        for (const auto& [p, item] : pos) {
            const u32 d = line.length - std::min(p, line.length);
            const u32 idx = std::min<u32>(d / kBeltTileLength, static_cast<u32>(line.path.size()) - 1);
            const u64 t = line.path[idx];
            const i32 tx = key_x(t), ty = key_y(t);
            if (tx < r.x0 || tx >= r.x1 || ty < r.y0 || ty >= r.y1) continue;
            // Along the tile in its direction: 0 at its back edge, 1 at the front.
            const f32 along = static_cast<f32>(d - idx * kBeltTileLength) / static_cast<f32>(kBeltTileLength);
            const u8 dir = static_cast<u8>(m.belts.at(t).dir);
            fn(static_cast<f32>(tx) + 0.5f + static_cast<f32>(kDx[dir]) * (along - 0.5f),
               static_cast<f32>(ty) + 0.5f + static_cast<f32>(kDy[dir]) * (along - 0.5f), item);
        }
    }
}

MachineState Factory::machine(MachineId id) const {
    const Impl& m = *impl_;
    if (id >= m.machines.size()) return {};
    return m.machines[id].s;
}

MachineId Factory::machine_at(i32 x, i32 y) const {
    auto it = impl_->machine_tiles.find(key(x, y));
    return it == impl_->machine_tiles.end() ? kNoMachine : it->second;
}

bool Factory::belt_at(i32 x, i32 y, Dir* dir) const {
    auto it = impl_->belts.find(key(x, y));
    if (it == impl_->belts.end()) return false;
    if (dir) *dir = it->second.dir;
    return true;
}

bool Factory::pipe_at(i32 x, i32 y, FluidId* fluid, f32* amount) const {
    const Impl& m = *impl_;
    auto it = m.pipes.find(key(x, y));
    if (it == m.pipes.end()) return false;
    const bool seg = it->second != kNone && it->second < m.segments.size() && !m.segments[it->second].tiles.empty();
    if (fluid) *fluid = seg ? m.segments[it->second].fluid : 0;
    if (amount) *amount = seg ? m.segments[it->second].amount / static_cast<f32>(m.segments[it->second].tiles.size()) : 0;
    return true;
}

bool Factory::pole_at(i32 x, i32 y) const { return impl_->poles.count(key(x, y)) != 0; }

f32 Factory::power_at(MachineId id) const {
    const Impl& m = *impl_;
    if (id >= m.machines.size() || m.machines[id].net == kNone || m.machines[id].net >= m.nets.size()) return 1;
    return m.nets[m.machines[id].net].satisfaction;
}

MachineId Factory::machine_capacity() const { return static_cast<MachineId>(impl_->machines.size()); }

const MachineDesc* Factory::machine_desc(MachineId id) const {
    const Impl& m = *impl_;
    return id < m.machines.size() && m.machines[id].alive ? &m.machines[id].d : nullptr;
}

const FactoryStats& Factory::stats() const { return impl_->stats; }

std::vector<u8> Factory::save() const {
    const Impl& m = *impl_;
    std::vector<u8> out;
    Writer w{out};
    w.put(kMagic);
    w.put(kVersion);
    w.put(static_cast<u32>(m.belts.size()));
    for (const auto& [k, b] : m.belts) {
        w.put(k);
        w.put(static_cast<u8>(b.dir));
        w.put(b.speed);
    }
    w.put(static_cast<u32>(m.pipes.size()));
    for (const auto& [k, s] : m.pipes) w.put(k);
    w.put(static_cast<u32>(m.poles.size()));
    for (const auto& [k, p] : m.poles) {
        w.put(k);
        w.put(p.supply);
        w.put(p.wire);
    }
    w.put(static_cast<u32>(m.machines.size()));
    for (const Impl::Machine& mc : m.machines) {
        w.put(static_cast<u8>(mc.alive));
        if (!mc.alive) continue;
        MachineDesc d;
        std::memset(static_cast<void*>(&d), 0, sizeof(d)); // no stray padding bytes in saves
        d.kind = mc.d.kind;
        d.x = mc.d.x;
        d.y = mc.d.y;
        d.w = mc.d.w;
        d.h = mc.d.h;
        d.recipe = mc.d.recipe;
        d.power = mc.d.power;
        d.fuel = mc.d.fuel;
        d.fuel_energy = mc.d.fuel_energy;
        d.capacity = mc.d.capacity;
        w.put(d);
        w.put(mc.s.working);
        w.put(mc.s.progress);
        w.put(mc.s.stored);
        w.put(mc.s.made);
        for (u16 v : mc.s.in) w.put(v);
        w.put(mc.s.out);
        w.put(mc.s.fluid_in);
        w.put(mc.s.fluid_out);
    }
    // Items by tile (pending belt changes are applied at the next tick:
    // items on lines still count).
    std::vector<std::pair<u32, ItemId>> pos;
    std::vector<Impl::Snap> items;
    for (u32 l : m.live_lines) {
        const Impl::Line& line = m.lines[l];
        pos.clear();
        line.positions(pos);
        for (const auto& [p, item] : pos) {
            const u32 d = line.length - p;
            const u32 idx = std::min<u32>(d / kBeltTileLength, static_cast<u32>(line.path.size()) - 1);
            items.push_back({line.path[idx], d - idx * kBeltTileLength, item});
        }
    }
    w.put(static_cast<u32>(items.size()));
    for (const Impl::Snap& s : items) {
        w.put(s.tile);
        w.put(static_cast<u16>(s.offset));
        w.put(s.item);
    }
    // Fluids: each pipe tile's share.
    w.put(static_cast<u32>(m.pipes.size()));
    for (const auto& [k, s] : m.pipes) {
        const bool seg = s != kNone && s < m.segments.size() && !m.segments[s].tiles.empty();
        w.put(k);
        w.put(seg ? m.segments[s].fluid : FluidId{0});
        w.put(seg ? m.segments[s].amount / static_cast<f32>(m.segments[s].tiles.size()) : 0.0f);
    }
    return out;
}

bool Factory::load(const u8* data, usize size) {
    Reader r{data, data + size};
    if (r.get<u32>() != kMagic || r.get<u32>() != kVersion) return false;
    auto fresh = std::make_unique<Impl>();
    Impl& m = *fresh;
    const u32 belts = r.get<u32>();
    for (u32 i = 0; i < belts && r.ok; ++i) {
        const u64 k = r.get<u64>();
        const Dir dir = static_cast<Dir>(r.get<u8>() & 3);
        const u8 speed = r.get<u8>();
        m.belts[k] = {dir, std::max<u8>(speed, 1)};
        m.pending_belts.push_back(k);
    }
    const u32 pipes = r.get<u32>();
    for (u32 i = 0; i < pipes && r.ok; ++i) {
        const u64 k = r.get<u64>();
        m.pipes[k] = kNone;
        m.pending_pipes.push_back(k);
    }
    const u32 poles = r.get<u32>();
    for (u32 i = 0; i < poles && r.ok; ++i) {
        const u64 k = r.get<u64>();
        const f32 supply = r.get<f32>();
        const f32 wire = r.get<f32>();
        m.poles[k] = {supply, wire, kNone};
        m.pole_grid[Impl::pole_cell(key_x(k), key_y(k))].push_back(k);
        m.pending_poles.push_back(k);
    }
    const u32 machines = r.get<u32>();
    for (u32 i = 0; i < machines && r.ok; ++i) {
        if (!r.get<u8>()) {
            m.machines.emplace_back();
            m.free_machines.push_back(i);
            continue;
        }
        const MachineDesc d = r.get<MachineDesc>();
        // Placed at the same id: every earlier slot exists already.
        m.machines.emplace_back();
        m.free_machines.push_back(i);
        const MachineId id = m.add_machine(d);
        if (id != i) return false;
        Impl::Machine& mc = m.machines[id];
        mc.s.working = r.get<bool>();
        mc.s.progress = r.get<f32>();
        mc.s.stored = r.get<f32>();
        mc.s.made = r.get<u32>();
        for (u16& v : mc.s.in) v = r.get<u16>();
        mc.s.out = r.get<u16>();
        mc.s.fluid_in = r.get<f32>();
        mc.s.fluid_out = r.get<f32>();
    }
    const u32 items = r.get<u32>();
    for (u32 i = 0; i < items && r.ok; ++i) {
        const u64 t = r.get<u64>();
        const u16 off = r.get<u16>();
        const ItemId item = r.get<ItemId>();
        m.loaded_items.push_back({t, off, item});
    }
    const u32 fluids = r.get<u32>();
    for (u32 i = 0; i < fluids && r.ok; ++i) {
        const u64 t = r.get<u64>();
        const FluidId f = r.get<FluidId>();
        const f32 a = r.get<f32>();
        m.loaded_fluids.push_back({t, f, a});
    }
    if (!r.ok) return false;
    m.rebuild();
    impl_ = std::move(fresh);
    return true;
}

} // namespace forge::sim
