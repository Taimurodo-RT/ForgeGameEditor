#pragma once

// A sample production block for demos, tests and benchmarks (like the
// sample world generators): ore is mined, smelted into plates and made
// into gears; coal and water make steam for an engine that powers it all.
//
//   y 0..1   two ore drills -> belts -> smelter -> belt -> assembler -> belt -> sink
//   y 3      power poles
//   y 5      pump -> pipes -> boiler -> steam pipes -> engine
//   y 6..9   coal drill -> belt up into the boiler
//
// A block is kSampleBlockW × kSampleBlockH tiles.

#include "forge/sim/factory.h"

namespace forge::sim {

constexpr i32 kSampleBlockW = 32;
constexpr i32 kSampleBlockH = 11;

enum SampleItem : ItemId { kItemOre = 1, kItemPlate = 2, kItemGear = 3, kItemCoal = 4 };
enum SampleFluid : FluidId { kFluidWater = 1, kFluidSteam = 2 };

struct SampleBlock {
    MachineId ore_drills[2], smelter, assembler, sink, pump, boiler, coal_drill, engine;
};

SampleBlock build_sample_block(Factory& f, i32 ox, i32 oy);

} // namespace forge::sim
