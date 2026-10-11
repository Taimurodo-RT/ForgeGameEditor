#pragma once

// main() of a Forge game: opens the window (or runs offscreen for
// screenshots and self-tests), sets up the shell and drives it.
//
//   my_game                          play
//   my_game --screenshot out.png [--frames N] [--test]   offscreen
//   my_game --test --window [--no-vsync]  the self-test in a real window
//   my_game --ui DIR --data DIR --user DIR --theme NAME --no-vsync
//   my_game --play                   a new game at once, past the main menu

#include "forge/game/shell.h"

#include <filesystem>
#include <functional>

namespace forge::game {

struct GameMain {
    // Where the files are when running from the build folder; a packaged
    // game finds them in "data" next to the executable instead.
    std::filesystem::path dev_ui_dir;
    std::filesystem::path dev_game_dir;
    // The module the game is ("slice"): its slots record it, and the shell loads no slot of another (step 14.3a).
    std::string module;
    // Self-test (offscreen, or in a window with --window): called before
    // each frame with the frame number; return false when done. failures
    // counts what went wrong.
    std::function<bool(Shell& shell, u32 frame, int& failures)> test;
};

int run_game(Game& game, const GameMain& main, int argc, char** argv);

// The game's data folder as run_game takes it from a command line: the last --data, else data/game next to the
// executable when data/ui is there too (a packaged game), else dev_game_dir. The launcher reads the game's module
// there before run_game starts (step 14.3a), so both read the same folder.
std::filesystem::path game_data_dir(int argc, char** argv, const std::filesystem::path& dev_game_dir);

} // namespace forge::game
