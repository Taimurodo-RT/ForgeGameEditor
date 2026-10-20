#pragma once

// «Отдать игре» of a scene of «Векторная» (part 6г1 of docs/models/сцены-модель.md): the folder game/scenes/<name>/ a
// game plays the scene from (forge/vector/scene_file.h), worked out of the tab's scene and checked against it.
//
//   scene.json                 what is on the scene, its actors' clips in its time, its music, waits and transitions
//   stage.svg                  the stage's shapes and its pictures' and actors' groups, empty (their own layers draw them)
//   stage.motion.json          clip "scene" of the stage: its shapes, groups, camera, transitions' frames and its moves
//                              of the actors' parts, as a screen's clip (screen.h)
//   actors/ёжик.svg            the actor's character, its ids «ёжик/…» in a group «ёжик» (prefix_svg_ids)
//   actors/ёжик.angry.svg      its other drawings
//   actors/ёжик.motion.json    the clips the scene plays it, each with the values it plays it at: «walk», «walk 2», …
//   pictures/лес.png           the picture's PNG of «Ресурсы», byte for byte, by the first picture of the scene of it

#include "forge/pixel/scene.h"
#include "forge/vector/scene_file.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::pixel {

struct SceneGift {
    struct File {
        std::string path;  // in the folder, forward slashes: "actors/ёжик.svg"
        std::string bytes;
    };
    vector::SceneMotion motion;     // what the files say, as the game reads them
    std::vector<File> files;        // the folder's, by path
    std::vector<std::string> notes; // what the game will not show as the tab does, for the tab's line
};

// The scene recorded again and every clip its actors play recorded with the values it plays it at, as the folder of
// scene `name` given from `from` (its code's path in «Ресурсы»); the pictures read through `files`. The folder's files
// are then read back as a game reads them and checked against the tab (check_scene_gift). False with why: the scene or a
// clip that does not record, a name that makes no file or two that make one, a picture that does not read, a check that
// fails.
bool build_scene_gift(Scene& scene, std::string_view name, std::string_view from, const SceneFiles& files, SceneGift& out, std::string& why);

// Whether a game poses the scene of `motion` as the tab poses `scene` (recorded): at every 1/30 s and at each start and
// end of an actor's clip, a stop and a transition; with no waits, with 0.5 s at each stop, and standing at each stop.
// The phase, the music, each actor's clip and its moment, the drawing it shows, its copies and order, and every value of
// every part of the stage and the actors where the files keep it exactly (a frames piece between two of its frames mixes
// them, as on a screen of the game, part 5в). False with why: «в игре сцена разошлась бы со вкладкой: 3,2 с, «ёжик/head»,
// поворот».
bool check_scene_gift(Scene& scene, const vector::SceneMotion& motion, std::string& why);

} // namespace forge::pixel
