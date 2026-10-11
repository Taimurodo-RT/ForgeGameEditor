// The launcher of Forge's games (step 14.3a): one program for a game of any module built in.
//
//   forge_game [--data GAME] [what the module's game takes: --test, --scene, --play, --user, …]
//
// It finds the game's data as the game itself does (forge::game::game_data_dir: the last --data, else data/game next
// to it in a package, else games/slice of the sources), reads the module the game names in its game.json
// (forge::game::game_module: none is a game from before step 14.3, «Старая шахта», "slice") and hands the command line
// over to that module's game as it got it. A module this build has not, or a game.json whose module cannot be read
// (an empty one, a number, no game.json), stops it with the reason, before anything of the game starts: in a window
// for a player, in the log for tests (--test, --screenshot). It is never "slice" for want of a better guess.
//
// The same program is built as forge_slice (build/apps/slice: scripts and shortcuts that start the old name keep
// working, with the same arguments) and packaged as OldMine. The test module «Проба» is built in only for --test.

#include "builtin.h"

#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/game/game_module.h"
#include "forge/game/runner.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // the window-only entry point on Windows, and the arguments in UTF-8

#include <cstring>
#include <string>

#ifndef FORGE_DEV_GAME_DIR
#define FORGE_DEV_GAME_DIR "games/slice"
#endif

using namespace forge;

namespace {

bool has(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const bool tests = has(argc, argv, "--test");
    const bool quiet = tests || has(argc, argv, "--screenshot");
    auto refuse = [&](const std::string& why) {
        FORGE_ERROR("Игра не запустилась: %s", why.c_str());
        if (!quiet) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Игра не запустилась", why.c_str(), nullptr);
        return 1;
    };
    modules::Registry registry;
    if (std::string error; !builtin::add_modules(registry, tests, &error)) return refuse("модули сборки: " + error);
    const std::filesystem::path data = game::game_data_dir(argc, argv, utf8_path(FORGE_DEV_GAME_DIR));
    std::string id, error;
    const game::GameModule found = game::game_module(data, id, &error);
    if (found == game::GameModule::Broken) return refuse(error);
    const modules::ModuleDef* module = registry.find(id);
    if (!module) return refuse(modules::missing_module(id));
    FORGE_INFO("Игра %s: модуль «%s»%s", path_to_utf8(data).c_str(), id.c_str(),
               found == game::GameModule::Old ? " (в game.json не назван: игра до шага 14.3)" : "");
    return module->run(argc, argv);
}
