#include "forge/sim/factory_sample.h"

namespace forge::sim {

SampleBlock build_sample_block(Factory& f, i32 ox, i32 oy) {
    SampleBlock b{};
    auto machine = [&](MachineKind kind, i32 x, i32 y, u8 size) {
        MachineDesc d;
        d.kind = kind;
        d.x = ox + x;
        d.y = oy + y;
        d.w = d.h = size;
        return d;
    };
    auto belts = [&](i32 x0, i32 x1, i32 y) {
        for (i32 x = x0; x <= x1; ++x) f.place_belt(ox + x, oy + y, Dir::East);
    };

    for (i32 i = 0; i < 2; ++i) {
        MachineDesc d = machine(MachineKind::Crafter, 0, i, 1);
        d.recipe.out = {kItemOre, 1};
        d.recipe.seconds = 1;
        b.ore_drills[i] = f.place_machine(d);
        belts(1, 9, i);
    }
    MachineDesc smelter = machine(MachineKind::Crafter, 10, 0, 2);
    smelter.recipe.in[0] = {kItemOre, 1};
    smelter.recipe.out = {kItemPlate, 1};
    smelter.recipe.seconds = 0.8f;
    smelter.power = 90;
    b.smelter = f.place_machine(smelter);
    belts(12, 19, 0);
    MachineDesc assembler = machine(MachineKind::Crafter, 20, 0, 2);
    assembler.recipe.in[0] = {kItemPlate, 2};
    assembler.recipe.out = {kItemGear, 1};
    assembler.recipe.seconds = 1;
    assembler.power = 150;
    b.assembler = f.place_machine(assembler);
    belts(22, 29, 0);
    b.sink = f.place_machine(machine(MachineKind::Sink, 30, 0, 1));

    for (i32 x : {3, 10, 17, 21}) f.place_pole(ox + x, oy + 3);

    MachineDesc pump = machine(MachineKind::Crafter, 0, 5, 1);
    pump.recipe.fluid_out = kFluidWater;
    pump.recipe.fluid_out_amount = 20;
    pump.recipe.seconds = 0.1f;
    b.pump = f.place_machine(pump);
    for (i32 x = 1; x <= 3; ++x) f.place_pipe(ox + x, oy + 5);
    MachineDesc boiler = machine(MachineKind::Crafter, 4, 5, 1);
    boiler.recipe.in[0] = {kItemCoal, 1};
    boiler.recipe.fluid_in = kFluidWater;
    boiler.recipe.fluid_in_amount = 60;
    boiler.recipe.fluid_out = kFluidSteam;
    boiler.recipe.fluid_out_amount = 60;
    boiler.recipe.seconds = 1;
    b.boiler = f.place_machine(boiler);
    for (i32 x = 5; x <= 7; ++x) f.place_pipe(ox + x, oy + 5);
    MachineDesc engine = machine(MachineKind::Generator, 8, 5, 1);
    engine.power = 600;
    engine.fuel = kFluidSteam;
    engine.fuel_energy = 10;
    b.engine = f.place_machine(engine);

    MachineDesc coal = machine(MachineKind::Crafter, 4, 9, 1);
    coal.recipe.out = {kItemCoal, 1};
    coal.recipe.seconds = 0.5f;
    b.coal_drill = f.place_machine(coal);
    for (i32 y = 6; y <= 8; ++y) f.place_belt(ox + 4, oy + y, Dir::North);
    return b;
}

} // namespace forge::sim
