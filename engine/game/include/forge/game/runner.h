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
    // Self-test (offscreen, or in a window with --window): called before
    // each frame with the frame number; return false when done. failures
    // counts what went wrong.
    std::function<bool(Shell& shell, u32 frame, int& failures)> test;
};

int run_game(Game& game, const GameMain& main, int argc, char** argv);

} // namespace forge::game
