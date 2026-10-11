#pragma once

// The game of the template «Платформер» (step 14.2d), made from its sources.
//
// games/platformer is an ordinary game folder: three levels with their own tiles («Луг», «Холмы», «Вершина»), the
// templates of the hero's picture, the enemies, the coin, the spikes, the flag and the sign, the links between them,
// the HUD and the windows of victory and defeat. An author changes it in the editor's tabs like any game; the copy in
// the repository is what this makes from
//   games/templates/platformer/assets/картинки/*.png — the pictures (the template's «Ресурсы», with their .meta),
//   games/templates/platformer/tiles.png             — the levels' tiles (16 of 32 px in a row, ids 256..271),
//   games/modules/slice/{kinds,verbs,ideas}.json     — the module's files,
// so a new picture of the same size is a new template after
//   forge_editor --make-template platformer <games>/platformer
// and the editor's self-test (platformer-template) makes it again and compares it with the repository byte for byte.
// The maker is slice's (step 14.3a): the editor finds it among the module's templates (slice_module.cpp,
// forge::modules::TemplateMaker "platformer"), not by this header.

#include <filesystem>
#include <string>

namespace platformer_template {

// The game, made in out from the games folder of the repository (games/). It is made beside out first and takes its
// place when it is whole: sources that do not make it leave out as it was. False: why says.
bool make(const std::filesystem::path& games, const std::filesystem::path& out, std::string& why);

} // namespace platformer_template
