#pragma once

// What an author made in a game through the editor (step 14.1b, «Находка»), for the game's scene project to check
// with --edits FILE: the editor's self-test writes it from what its tabs hold, the game reads it and plays the game
// as a player would. Nothing here is the game's data; the game reads only its own folder. A file of the tests between
// the editor and a module's game (step 14.3a: apps/common, so the editor names no module's own header).
//
// An own object with a picture of one colour stands on the level; when the hero touches it, a link of «Логика»
// adds to a value of the game and shows a screen of «Интерфейс»: a text with that value, the same picture moving
// by its keys, a button that plays a sound and closes the window. Once only.
// absent: another game of the same template, which must have none of it, and a player's folder of its own.
//
// A game of several levels (step 14.2a): the level the game plays, by its id in the game's list, and cells the author
// put into that level, which the game must have; without an object only these are checked. And going between its
// levels (step 14.2b): the areas the hero comes into, where it must be after each. The platformer (step 14.2c): the
// author's enemy, coin, trap and goal, the zone the hero falls into, the HUD and the windows the game shows at its end.
// The template «Платформер» (step 14.2d): what the author changed in a game made of it. The tab «Анимация»: the
// animations the author made, played by the hero, a walker and an object that stands. The template «RPG сверху» (step
// 14.4b): what the author changed in a game made of it, played by the module rpg. The template «Три в ряд» (step 14.3c):
// the same, played by the module match3.

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <filesystem>
#include <string>
#include <vector>

namespace forge::edits {

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
    // The picture's movement by its own keys, as the author keyed it: its scale at the middle key (100 % at both
    // ends) and the seconds of one pass; 0: not looked at.
    double motion_scale = 0, motion_seconds = 0;
    bool absent = false;     // none of it is in this game
    std::string level;       // the level the game plays (its id in levels.json); empty: not checked
    std::string level_name;  // its name, for messages
    std::vector<int> cells;  // layer, x, y, tile, layer, x, y, tile, …: cells of that level as the author left them
    // Going between levels (step 14.2b) by links «уходит через» the author made in «Логика». go_continue: the game
    // starts at its main menu and «Продолжить» takes the save go_save (made by another process), the hero where that
    // save has it. On the level go_start, the hero comes into the area go_through[i] (its middle, from the level's spawn
    // point) and must then be on the level go_level[i] (its id in levels.json), in the area go_arrive[i] ("" at its
    // spawn point). On the level go_mark_put the game puts a crate of its own; on go_mark_find it must find it there
    // (the level as it was left, not as the author made it). Then saved as go_save (when not continued).
    bool go_continue = false;
    std::string go_start;
    std::vector<std::string> go_through, go_level, go_arrive;
    std::string go_mark_put, go_mark_find;
    std::string go_save;
    // The platformer (step 14.2c) as the author made it in the tabs: the templates of «Враг», «Подбираемое», «Ловушка»
    // and the goal (an own picture object, which a link «доходит до» names) by their ids; pl_at: where their copies stand
    // (the middles, as the editor has them): x, y of the enemy, the coin, the trap, the goal; the values the author gave
    // them; the zone of a link «падает в» (its thing id, "area:…"); the HUD (a screen over the game) with its text
    // layer, the windows «Показывается сам: при поражении» (its text, its button «Новая игра») and «при победе» (its
    // text). pl_goal_color: the goal's picture, one colour all over.
    std::string pl_enemy, pl_coin, pl_trap, pl_goal;
    std::vector<double> pl_at;
    double pl_damage = 0, pl_enemy_score = 0, pl_coin_score = 0, pl_trap_damage = 0, pl_trap_half_w = 0;
    std::string pl_pit;
    std::string pl_hud, pl_lose, pl_win;
    forge::u32 pl_hud_text = 0, pl_lose_text = 0, pl_lose_again = 0, pl_win_text = 0;
    std::vector<int> pl_goal_color;
    // The template «Платформер» (step 14.2d) as the author changed it in the tabs of a game made of it. tp_hero: the
    // hero's picture replaced in «Ресурсы», r, g, b of the middle of each of its four frames (stands, step, step, in the
    // air); the coin's template and its «Очки», a copy of it the author put (x, y of its middle); the enemy's template,
    // its «Скорость» and «Урон»; cells made solid over a pit (x0, x1, y: e.cells has them); the start level's exit (its
    // area) and where its link leads now (a level, an area of it); there the pit (its area) and the goal (the flag's
    // template); the HUD's score layer, the windows of the end: the lose window's «Ещё раз», the win window's title
    // layer and its button «Ещё раз» (the button, its label's layer) and what they say now.
    std::vector<int> tp_hero;
    std::string tp_coin;
    std::vector<double> tp_coin_at;
    double tp_coin_score = 0;
    std::string tp_enemy;
    double tp_enemy_speed = 0, tp_enemy_damage = 0;
    std::vector<int> tp_bridge;
    std::string tp_exit, tp_exit_level, tp_exit_arrive, tp_pit, tp_goal;
    std::string tp_hud, tp_lose, tp_win;
    forge::u32 tp_hud_score = 0, tp_again = 0, tp_win_title = 0, tp_win_again = 0, tp_win_again_label = 0;
    std::string tp_win_text, tp_win_again_text;
    std::string tp_twin; // the author's «Копия» of tp_enemy (its template's id)
    // The tab «Анимация» (the first of the order after 14.3a) as the author set it: their animations, each of the
    // template an_of[i] in its state an_state[i] ("walk", "air", "idle"): an_count[i] frames of its strip, taken in turn
    // from an_frames (all of them, one animation after another), an_fps[i] frames a second, starting over when an_loop[i]
    // is 1. an_hero: the hero's template (it walks and jumps); an_walker: a walker's (a copy the game puts beside the
    // hero walks); an_still: an object's that stands (a copy put there too).
    std::vector<std::string> an_of, an_state;
    std::vector<int> an_count, an_frames, an_loop;
    std::vector<double> an_fps;
    std::string an_hero, an_walker, an_still;
    // Their marks (part 5а of docs/models/движение-модель.md), from the code of «Векторная»: mark i of the animation of
    // mk_of[i] in mk_state[i], named mk_name[i], a sound when mk_sound[i] is 1 (else emit), on its place mk_frame[i] in the
    // animation's frames, a window of mk_frames[i] places. mk_scheme: the template whose own scheme of «Логика» adds 1 to
    // the game's variable mk_count_var on «При метке анимации» mk_mark and sets mk_going_var on «Каждый шаг» to
    // «Идёт метка анимации» of itself and mk_mark; with it the game checks the marks it fires tick by tick, without it
    // only that it reads them with the animations.
    std::vector<std::string> mk_of, mk_state, mk_name;
    std::vector<int> mk_sound, mk_frame, mk_frames;
    std::string mk_scheme, mk_mark, mk_count_var, mk_going_var;
    // The tab «Ввод» as the author set it: each action in_action[i] with in_count[i] binds, taken in turn from in_binds
    // ("key:K", "pad:south"; all of them, one action after another). in_jump: keys (SDL's names of their places) the hero
    // jumps on now; in_no_jump: keys it no longer jumps on.
    std::vector<std::string> in_action, in_binds, in_jump, in_no_jump;
    std::vector<int> in_count;
    // The template «RPG сверху» (step 14.4b) as the author changed it in the tabs of a game made of it, for the module
    // rpg's scene project. rp_elder: the elder's picture replaced in «Ресурсы», r, g, b, one colour all over. rp_villager:
    // the author's villager (a «Копия» of the elder, renamed rp_villager_name), standing at rp_villager_at (its cell
    // x, y on the start map, e.level), saying rp_talk, the author's dialogue of «Сюжет»: its first line rp_talk_line, its
    // answer rp_choice (the first) gives rp_reward coins once (rp_once: the variable it sets). rp_place: the author's
    // place at rp_place_at, a link «доходит до» to it passes the map. rp_coins: the coins' template, its sound now
    // rp_sound (a file of the game's sounds). rp_walk: the hero's frames while he walks, rp_walk_fps of them a second.
    // rp_hint: the HUD's hint as the author left it (its layer rp_hint_node of the screen rp_hud). Cells the author laid
    // are e.cells. rp_copy_at: another copy of rp_villager on that map, at that cell, saying its own rp_copy_talk (a
    // dialogue added in «Сюжет» after the copy was put, chosen in its properties on «Уровень»; the template still says
    // rp_talk): its first line rp_copy_line, no answers.
    std::vector<int> rp_elder;
    std::string rp_villager, rp_villager_name, rp_talk, rp_talk_line, rp_choice, rp_once;
    std::vector<int> rp_villager_at;
    double rp_reward = 0;
    std::string rp_place, rp_place_name;
    std::vector<int> rp_place_at;
    std::string rp_coins, rp_sound;
    std::vector<int> rp_walk;
    double rp_walk_fps = 0;
    std::string rp_hud, rp_hint;
    forge::u32 rp_hint_node = 0;
    std::vector<int> rp_copy_at;
    std::string rp_copy_talk, rp_copy_line;
    // The template «Три в ряд» (step 14.3c) as the author changed it in the tabs of a game made of it, for the module
    // match3's scenes project and project_continue. m3_refusal: the game as saved then must not start, saying this (the
    // author's field is wrong); nothing else is looked at. m3_star: the author's gem, a «Копия» of «Рубин» named
    // m3_star_name, its «Очки» m3_star_points, its «Звук сбора» m3_star_sound (a file of the game's sounds), its picture
    // one colour all over (m3_star_color, r, g, b); a link «Поле даёт +2 хода за» it. m3_ruby: «Рубин»'s picture replaced
    // in «Ресурсы», one colour; its «Очки» m3_ruby_points. The start level: its own moves and target (m3_first_moves,
    // m3_first_target), its holes m3_holes (x, y, x, y, … in the level's cells), of them those with a stone m3_stones,
    // the author's gems put before m3_star_at (x, y, …). m3_kind_moves: «Поле»'s «Ходов», which a level whose copy has
    // none of its own plays (m3_inherit: that level's id). The HUD's moves layer (m3_hud, node m3_hud_node) says
    // m3_hud_text, {board.moves} in it; the lose window's title (m3_lose, node m3_lose_node) m3_lose_title. m3_gone
    // (scene project_continue): the author deleted m3_star (named m3_star_name) in «Объекты» after the save m3-author:
    // «Продолжить» must be refused with these words, the save left as it was; nothing else is looked at.
    std::string m3_refusal, m3_gone;
    std::string m3_star, m3_star_name, m3_star_sound;
    std::vector<int> m3_star_color, m3_ruby;
    double m3_star_points = 0, m3_ruby_points = 0;
    double m3_first_moves = 0, m3_first_target = 0, m3_kind_moves = 0;
    std::vector<int> m3_holes, m3_stones, m3_star_at;
    std::string m3_inherit;
    std::string m3_hud, m3_hud_text, m3_lose, m3_lose_title;
    forge::u32 m3_hud_node = 0, m3_lose_node = 0;
    // A game of «Три в ряд» whose pictures the author drew in the tab «Изображения» (the editor's self-test images), for
    // the module match3's scene images. im_ruby: «Рубин»'s 4 frames, the r g b in the middle of each (12 numbers);
    // im_chip: the author's gem (its id), named im_chip_name, drawn by a strip made of a 2 × 2 sheet, the middle of each
    // of its 4 frames im_chip_rgb; im_at: the cell (x, y) a «Рубин» was put on, on the start level; im_px: pixels of
    // «Рубин»'s picture drawn with the picker's colours, "x y r g b a" each (alpha and all).
    std::vector<int> im_ruby, im_chip_rgb, im_at, im_px;
    std::string im_chip, im_chip_name;
    // SVG pictures of «Ресурсы» the author put on a screen of «Интерфейс» (the editor's self-test ui-scale), for the module
    // slice's scene project. vs_screen: the screen (its file's name). vs_picture: its layer filled by the game's own file
    // vs_picture_file, which says vs_picture_svg byte for byte; vs_frame: its layer framed (border-image) by vs_frame_file,
    // saying vs_frame_svg, its slice vs_frame_slice of the picture drawn at vs_frame_size × vs_frame_size. vs_picture_box,
    // vs_frame_box: their boxes on the screen's page (x, y, w, h). The game runs at the screen's own size (--size), so each
    // must be drawn as lunasvg draws its file at that size: the picture whole, the frame's corners and edges.
    std::string vs_screen, vs_picture_file, vs_picture_svg, vs_frame_file, vs_frame_svg;
    forge::u32 vs_picture = 0, vs_frame = 0, vs_frame_size = 0, vs_frame_slice = 0;
    std::vector<int> vs_picture_box, vs_frame_box;
    // A live SVG picture (step 4 of docs/models/вектор-модель.md; the editor's self-test vector-live), for the module
    // slice's scene project. On the screen vl_screen the layers vl_layer (named vl_layer_name) and vl_other show the
    // game's file vl_file, which says vl_svg byte for byte; vl_layer_box, vl_other_box: their boxes on the screen's page
    // (x, y, w, h). The author's own scheme of the thing vl_thing: at the start the part vl_color_part of the layer named
    // vl_layer_name takes the colour vl_color; each step its part vl_part morphs into vl_target by k = ease("smooth", the
    // world's time). Once k is 1 that layer is drawn as lunasvg draws vl_end (the file with the part made the other shape
    // and coloured), the other as lunasvg draws the file. The game at any size: the screen fitted onto it, the pictures
    // rasterized at the fit's scale.
    std::string vl_screen, vl_file, vl_svg, vl_layer_name, vl_thing, vl_part, vl_target, vl_color_part, vl_color, vl_end;
    forge::u32 vl_layer = 0, vl_other = 0;
    std::vector<int> vl_layer_box, vl_other_box;
    // With them, the author's own Luau too (a link's «Код»; vector-live after F5): half a second into a game it says
    // of the parts vlc_parts of the layer vl_other what makes it as lunasvg draws vlc_said, at a second it takes that
    // layer back to its file, at a second and a half it says it again; vl_layer goes on as the scheme says. The player
    // closes the game to its main menu while vl_other is so and goes on («Продолжить»): the screen shows again with both
    // pictures as their files; then vl_other stays so, vl_layer takes what the scheme says each step again and is drawn
    // as lunasvg draws vlc_again.
    std::string vlc_said, vlc_again;
    std::vector<std::string> vlc_parts;
    // A character's clips given to a screen (part 5в of docs/models/движение-модель.md; the editor's self-test
    // pixel-motion-m-again), for the module slice's scene project. On the screen ms_screen the layer ms_layer (named
    // ms_layer_name, its box ms_layer_box) shows the game's ms_file, which says ms_svg byte for byte, beside it the file
    // of clips «Отдать экрану» wrote. The own scheme of a thing plays «idle» on it at the start and, on the mark
    // «приземлился», a clip on the layer ms_other_name, whose picture has no file of clips: the error ms_scheme_error.
    // The author's Luau (a link's «Код», ms_code) plays «jump» at 1.2 s of the world, stops at 2.2 s, plays «idle» twice
    // as fast at 2.7 s, takes the picture back to its file at 3.3 s, plays «idle» at 3.8 s and at 4.8 s a clip there is
    // not: the error ms_code_error. ms_tab: the tab's frame of each clip at each 120th of a second, one line each:
    // "clip k part a b c d e f" (the part's matrix as the tab's picture has it) or "clip k part morph share target".
    // The sound mark is the game's sound ms_sound; ms_not_drawn: the line the game says once of what it does not draw.
    std::string ms_screen, ms_layer_name, ms_other_name, ms_file, ms_svg, ms_sound, ms_scheme_error, ms_code, ms_code_error, ms_not_drawn;
    forge::u32 ms_layer = 0;
    std::vector<int> ms_layer_box;
    std::vector<std::string> ms_tab;
};

// False (error says why) when the file cannot be read.
bool read_edits(const std::filesystem::path& file, ProjectEdits& out, std::string& error);
bool write_edits(const std::filesystem::path& file, const ProjectEdits& edits);

} // namespace forge::edits

FORGE_REFLECT_DECLARE(forge::edits::ProjectEdits)
