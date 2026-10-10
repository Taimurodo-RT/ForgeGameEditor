#pragma once

// What an author made in a game through the editor (step 14.1b, «Находка»), for the game's scene project to check
// with --edits FILE: the editor's self-test writes it from what its tabs hold, the game reads it and plays the game
// as a player would. Nothing here is the game's data; the game reads only its own folder.
//
// An own object with a picture of one colour stands on the level; when the hero touches it, a link of «Логика»
// adds to a value of the game and shows a screen of «Интерфейс»: a text with that value, the same picture moving
// by its keys, a button that plays a sound and closes the window. Once only.
// absent: another game of the same template, which must have none of it, and a player's folder of its own.
//
// A game of several levels (step 14.2a): the level the game plays, by its id in the game's list, and cells the author
// put into that level, which the game must have; without an object only these are checked.

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <filesystem>
#include <string>
#include <vector>

namespace slice {

struct ProjectEdits {
    std::string object;      // the template's id ("o…")
    std::string object_name; // its name, for messages
    double x = 0, y = 0;     // where its copy stands on the level (its centre, in cells), as the editor has it
    std::vector<int> color;  // r, g, b: its picture, one colour all over
    std::string var;         // the value of the game the link adds to
    double value = 0;        // what it is after the touch
    std::string screen;      // the screen the link shows (its file's name)
    std::string text;        // what its text layer says then
    forge::u32 text_node = 0, picture_node = 0, button_node = 0; // their layers (#n<id> on the page)
    std::string sound;       // the button's sound, a file of the game's sounds
    forge::u32 sound_frames = 0; // its length
    bool absent = false;     // none of it is in this game
    std::string level;       // the level the game plays (its id in levels.json); empty: not checked
    std::string level_name;  // its name, for messages
    std::vector<int> cells;  // layer, x, y, tile, layer, x, y, tile, …: cells of that level as the author left them
};

// False (error says why) when the file cannot be read.
bool read_edits(const std::filesystem::path& file, ProjectEdits& out, std::string& error);
bool write_edits(const std::filesystem::path& file, const ProjectEdits& edits);

} // namespace slice

FORGE_REFLECT_DECLARE(slice::ProjectEdits)
