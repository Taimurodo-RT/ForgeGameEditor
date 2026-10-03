#pragma once

// Always-on factories (like Factorio): belts, pipes and power networks that
// keep working all over the world, whether its chunks are loaded or not.
//
// Nothing here is a per-item game object. Factory data lives in its own
// arrays, apart from the chunks:
//
//   belts    chains of belt tiles merge into lines. A line keeps its items as
//            gaps between neighbours, so moving a whole line is a change of
//            one or two numbers: a million items on belts cost about as
//            much as the lines they are on.
//   pipes    connected pipe tiles form one segment holding one fluid; the
//            fluid is shared out to machines in bulk (no flow per tile).
//   power    poles connected by wires form networks; each tick a network
//            sums what its machines want and its generators can give, and
//            every machine on it works at that share.
//   machines craft by recipe: take items from belts that run into them,
//            fluid from pipes beside them, power from a pole that reaches
//            them, and put the result on belts that start next to them.
//
// Every part runs on all cores (lines, machines, networks and segments are
// independent within a phase). Building changes are applied at the next
// tick; belts rebuild only the lines next to the change.

#include "forge/core/types.h"
#include "forge/world/coords.h"

#include <functional>
#include <memory>
#include <vector>

namespace forge::sim {

using ItemId = u16;  // 0: no item
using FluidId = u16; // 0: no fluid
using MachineId = u32;
constexpr MachineId kNoMachine = ~0u;

// y grows downwards: South is +y.
enum class Dir : u8 { East, South, West, North };

// A belt tile is 256 steps long; items keep at least 64 steps apart (four
// per tile). Belt speed is in steps per 1/60 s: 8 is 1.875 tiles a second.
constexpr u32 kBeltTileLength = 256;
constexpr u32 kItemSpacing = 64;
constexpr f32 kPipeTileCapacity = 100;

struct Recipe {
    struct Part {
        ItemId item = 0;
        u16 count = 0;
    };
    Part in[3];
    Part out;
    FluidId fluid_in = 0;
    f32 fluid_in_amount = 0;
    FluidId fluid_out = 0;
    f32 fluid_out_amount = 0;
    f32 seconds = 1;
};

enum class MachineKind : u8 {
    Crafter,     // recipe; with no inputs it is a drill or a pump
    Generator,   // gives power, burning fluid (steam) or nothing (solar)
    Accumulator, // stores spare power and gives it back when short
    Sink,        // takes any item from belts and counts it
};

struct MachineDesc {
    MachineKind kind = MachineKind::Crafter;
    i32 x = 0, y = 0; // top-left tile
    u8 w = 1, h = 1;
    Recipe recipe;       // Crafter
    f32 power = 0;       // kW: drawn while crafting (0: needs none), max output, max rate
    FluidId fuel = 0;    // Generator: fluid it burns (0: none needed)
    f32 fuel_energy = 0; // kJ per unit of fuel
    f32 capacity = 0;    // Accumulator: kJ
};

struct MachineState {
    bool alive = false;
    bool working = false;
    f32 progress = 0;     // 0..1 of the current craft
    f32 satisfaction = 0; // share of the power it wanted that it got
    f32 output = 0;       // kW given (Generator, Accumulator)
    f32 stored = 0;       // kJ (Accumulator)
    u32 made = 0;         // crafts done (Sink: items taken)
    u16 in[3] = {};
    u16 out = 0;
    f32 fluid_in = 0, fluid_out = 0;
};

struct FactoryStats {
    u32 belt_tiles = 0;
    u32 lines = 0;
    u32 items = 0;
    u32 machines = 0;
    u32 power_nets = 0;
    u32 pipe_segments = 0;
    u32 rebuilds = 0; // building changes applied in the last tick
    f64 rebuild_ms = 0;
    f64 belts_ms = 0;
    f64 machines_ms = 0;
    f64 power_ms = 0;
    f64 fluids_ms = 0;
    f64 total_ms = 0;
};

class Factory {
public:
    Factory();
    ~Factory();
    Factory(const Factory&) = delete;
    Factory& operator=(const Factory&) = delete;

    // Building. False when the place is taken (or, for removal, empty).
    bool place_belt(i32 x, i32 y, Dir dir, u8 speed = 8);
    bool remove_belt(i32 x, i32 y); // items on that tile are lost
    bool place_pipe(i32 x, i32 y);
    bool remove_pipe(i32 x, i32 y);
    // supply: half-size of the square it powers; wire: how far its wires reach.
    bool place_pole(i32 x, i32 y, f32 supply = 2.5f, f32 wire = 7.5f);
    bool remove_pole(i32 x, i32 y);
    MachineId place_machine(const MachineDesc& desc); // kNoMachine if it overlaps something
    bool remove_machine(MachineId id);

    // Puts an item on a belt tile, at the middle of it, if there is room.
    // The belt must already be built (a tick has run since placing it).
    bool insert_item(i32 x, i32 y, ItemId item);

    // One simulation tick. dt <= 0 only applies building changes.
    void tick(f32 dt);

    // Items on belts inside a tile rectangle, at their position in tiles.
    void for_each_item(const world::Rect& tiles, const std::function<void(f32 x, f32 y, ItemId item)>& fn) const;
    MachineState machine(MachineId id) const;
    MachineId machine_at(i32 x, i32 y) const;
    bool belt_at(i32 x, i32 y, Dir* dir = nullptr) const;
    bool pipe_at(i32 x, i32 y, FluidId* fluid = nullptr, f32* amount = nullptr) const;
    bool pole_at(i32 x, i32 y) const;
    // Share of the wanted power a network got last tick (1 when no pole here).
    f32 power_at(MachineId id) const;
    template <typename Fn>
    void for_each_machine(Fn&& fn) const {
        for (MachineId i = 0; i < machine_capacity(); ++i)
            if (const MachineDesc* d = machine_desc(i)) fn(i, *d);
    }
    MachineId machine_capacity() const;
    const MachineDesc* machine_desc(MachineId id) const; // nullptr when removed
    const FactoryStats& stats() const;

    // Whole factory state (buildings, items on belts, fluids, buffers).
    std::vector<u8> save() const;
    bool load(const u8* data, usize size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace forge::sim
