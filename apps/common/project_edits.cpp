#include "project_edits.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/json.h"

#include <span>

FORGE_REFLECT(forge::edits::ProjectEdits, 1) {
    t.field("object", &forge::edits::ProjectEdits::object);
    t.field("object_name", &forge::edits::ProjectEdits::object_name);
    t.field("x", &forge::edits::ProjectEdits::x);
    t.field("y", &forge::edits::ProjectEdits::y);
    t.field("color", &forge::edits::ProjectEdits::color);
    t.field("var", &forge::edits::ProjectEdits::var);
    t.field("value", &forge::edits::ProjectEdits::value);
    t.field("screen", &forge::edits::ProjectEdits::screen);
    t.field("text", &forge::edits::ProjectEdits::text);
    t.field("text_node", &forge::edits::ProjectEdits::text_node);
    t.field("picture_node", &forge::edits::ProjectEdits::picture_node);
    t.field("button_node", &forge::edits::ProjectEdits::button_node);
    t.field("sound", &forge::edits::ProjectEdits::sound);
    t.field("sound_frames", &forge::edits::ProjectEdits::sound_frames);
    t.field("motion_scale", &forge::edits::ProjectEdits::motion_scale);
    t.field("motion_seconds", &forge::edits::ProjectEdits::motion_seconds);
    t.field("absent", &forge::edits::ProjectEdits::absent);
    t.field("level", &forge::edits::ProjectEdits::level);
    t.field("level_name", &forge::edits::ProjectEdits::level_name);
    t.field("cells", &forge::edits::ProjectEdits::cells);
    t.field("go_continue", &forge::edits::ProjectEdits::go_continue);
    t.field("go_start", &forge::edits::ProjectEdits::go_start);
    t.field("go_through", &forge::edits::ProjectEdits::go_through);
    t.field("go_level", &forge::edits::ProjectEdits::go_level);
    t.field("go_arrive", &forge::edits::ProjectEdits::go_arrive);
    t.field("go_mark_put", &forge::edits::ProjectEdits::go_mark_put);
    t.field("go_mark_find", &forge::edits::ProjectEdits::go_mark_find);
    t.field("go_save", &forge::edits::ProjectEdits::go_save);
    t.field("pl_enemy", &forge::edits::ProjectEdits::pl_enemy);
    t.field("pl_coin", &forge::edits::ProjectEdits::pl_coin);
    t.field("pl_trap", &forge::edits::ProjectEdits::pl_trap);
    t.field("pl_goal", &forge::edits::ProjectEdits::pl_goal);
    t.field("pl_at", &forge::edits::ProjectEdits::pl_at);
    t.field("pl_damage", &forge::edits::ProjectEdits::pl_damage);
    t.field("pl_enemy_score", &forge::edits::ProjectEdits::pl_enemy_score);
    t.field("pl_coin_score", &forge::edits::ProjectEdits::pl_coin_score);
    t.field("pl_trap_damage", &forge::edits::ProjectEdits::pl_trap_damage);
    t.field("pl_trap_half_w", &forge::edits::ProjectEdits::pl_trap_half_w);
    t.field("pl_pit", &forge::edits::ProjectEdits::pl_pit);
    t.field("pl_hud", &forge::edits::ProjectEdits::pl_hud);
    t.field("pl_lose", &forge::edits::ProjectEdits::pl_lose);
    t.field("pl_win", &forge::edits::ProjectEdits::pl_win);
    t.field("pl_hud_text", &forge::edits::ProjectEdits::pl_hud_text);
    t.field("pl_lose_text", &forge::edits::ProjectEdits::pl_lose_text);
    t.field("pl_lose_again", &forge::edits::ProjectEdits::pl_lose_again);
    t.field("pl_win_text", &forge::edits::ProjectEdits::pl_win_text);
    t.field("pl_goal_color", &forge::edits::ProjectEdits::pl_goal_color);
    t.field("tp_hero", &forge::edits::ProjectEdits::tp_hero);
    t.field("tp_coin", &forge::edits::ProjectEdits::tp_coin);
    t.field("tp_coin_at", &forge::edits::ProjectEdits::tp_coin_at);
    t.field("tp_coin_score", &forge::edits::ProjectEdits::tp_coin_score);
    t.field("tp_enemy", &forge::edits::ProjectEdits::tp_enemy);
    t.field("tp_enemy_speed", &forge::edits::ProjectEdits::tp_enemy_speed);
    t.field("tp_enemy_damage", &forge::edits::ProjectEdits::tp_enemy_damage);
    t.field("tp_bridge", &forge::edits::ProjectEdits::tp_bridge);
    t.field("tp_exit", &forge::edits::ProjectEdits::tp_exit);
    t.field("tp_exit_level", &forge::edits::ProjectEdits::tp_exit_level);
    t.field("tp_exit_arrive", &forge::edits::ProjectEdits::tp_exit_arrive);
    t.field("tp_pit", &forge::edits::ProjectEdits::tp_pit);
    t.field("tp_goal", &forge::edits::ProjectEdits::tp_goal);
    t.field("tp_hud", &forge::edits::ProjectEdits::tp_hud);
    t.field("tp_lose", &forge::edits::ProjectEdits::tp_lose);
    t.field("tp_win", &forge::edits::ProjectEdits::tp_win);
    t.field("tp_hud_score", &forge::edits::ProjectEdits::tp_hud_score);
    t.field("tp_again", &forge::edits::ProjectEdits::tp_again);
    t.field("tp_win_title", &forge::edits::ProjectEdits::tp_win_title);
    t.field("tp_win_text", &forge::edits::ProjectEdits::tp_win_text);
    t.field("tp_win_again", &forge::edits::ProjectEdits::tp_win_again);
    t.field("tp_win_again_label", &forge::edits::ProjectEdits::tp_win_again_label);
    t.field("tp_win_again_text", &forge::edits::ProjectEdits::tp_win_again_text);
    t.field("tp_twin", &forge::edits::ProjectEdits::tp_twin);
    t.field("an_of", &forge::edits::ProjectEdits::an_of);
    t.field("an_state", &forge::edits::ProjectEdits::an_state);
    t.field("an_count", &forge::edits::ProjectEdits::an_count);
    t.field("an_frames", &forge::edits::ProjectEdits::an_frames);
    t.field("an_loop", &forge::edits::ProjectEdits::an_loop);
    t.field("an_fps", &forge::edits::ProjectEdits::an_fps);
    t.field("an_hero", &forge::edits::ProjectEdits::an_hero);
    t.field("an_walker", &forge::edits::ProjectEdits::an_walker);
    t.field("an_still", &forge::edits::ProjectEdits::an_still);
    t.field("in_action", &forge::edits::ProjectEdits::in_action);
    t.field("in_count", &forge::edits::ProjectEdits::in_count);
    t.field("in_binds", &forge::edits::ProjectEdits::in_binds);
    t.field("in_jump", &forge::edits::ProjectEdits::in_jump);
    t.field("in_no_jump", &forge::edits::ProjectEdits::in_no_jump);
    t.field("rp_elder", &forge::edits::ProjectEdits::rp_elder);
    t.field("rp_villager", &forge::edits::ProjectEdits::rp_villager);
    t.field("rp_villager_name", &forge::edits::ProjectEdits::rp_villager_name);
    t.field("rp_talk", &forge::edits::ProjectEdits::rp_talk);
    t.field("rp_talk_line", &forge::edits::ProjectEdits::rp_talk_line);
    t.field("rp_choice", &forge::edits::ProjectEdits::rp_choice);
    t.field("rp_once", &forge::edits::ProjectEdits::rp_once);
    t.field("rp_villager_at", &forge::edits::ProjectEdits::rp_villager_at);
    t.field("rp_reward", &forge::edits::ProjectEdits::rp_reward);
    t.field("rp_place", &forge::edits::ProjectEdits::rp_place);
    t.field("rp_place_name", &forge::edits::ProjectEdits::rp_place_name);
    t.field("rp_place_at", &forge::edits::ProjectEdits::rp_place_at);
    t.field("rp_coins", &forge::edits::ProjectEdits::rp_coins);
    t.field("rp_sound", &forge::edits::ProjectEdits::rp_sound);
    t.field("rp_walk", &forge::edits::ProjectEdits::rp_walk);
    t.field("rp_walk_fps", &forge::edits::ProjectEdits::rp_walk_fps);
    t.field("rp_hud", &forge::edits::ProjectEdits::rp_hud);
    t.field("rp_hint", &forge::edits::ProjectEdits::rp_hint);
    t.field("rp_hint_node", &forge::edits::ProjectEdits::rp_hint_node);
    t.field("rp_copy_at", &forge::edits::ProjectEdits::rp_copy_at);
    t.field("rp_copy_talk", &forge::edits::ProjectEdits::rp_copy_talk);
    t.field("rp_copy_line", &forge::edits::ProjectEdits::rp_copy_line);
    t.field("m3_refusal", &forge::edits::ProjectEdits::m3_refusal);
    t.field("m3_gone", &forge::edits::ProjectEdits::m3_gone);
    t.field("m3_star", &forge::edits::ProjectEdits::m3_star);
    t.field("m3_star_name", &forge::edits::ProjectEdits::m3_star_name);
    t.field("m3_star_sound", &forge::edits::ProjectEdits::m3_star_sound);
    t.field("m3_star_color", &forge::edits::ProjectEdits::m3_star_color);
    t.field("m3_ruby", &forge::edits::ProjectEdits::m3_ruby);
    t.field("m3_star_points", &forge::edits::ProjectEdits::m3_star_points);
    t.field("m3_ruby_points", &forge::edits::ProjectEdits::m3_ruby_points);
    t.field("m3_first_moves", &forge::edits::ProjectEdits::m3_first_moves);
    t.field("m3_first_target", &forge::edits::ProjectEdits::m3_first_target);
    t.field("m3_kind_moves", &forge::edits::ProjectEdits::m3_kind_moves);
    t.field("m3_holes", &forge::edits::ProjectEdits::m3_holes);
    t.field("m3_stones", &forge::edits::ProjectEdits::m3_stones);
    t.field("m3_star_at", &forge::edits::ProjectEdits::m3_star_at);
    t.field("m3_inherit", &forge::edits::ProjectEdits::m3_inherit);
    t.field("m3_hud", &forge::edits::ProjectEdits::m3_hud);
    t.field("m3_hud_text", &forge::edits::ProjectEdits::m3_hud_text);
    t.field("m3_lose", &forge::edits::ProjectEdits::m3_lose);
    t.field("m3_lose_title", &forge::edits::ProjectEdits::m3_lose_title);
    t.field("m3_hud_node", &forge::edits::ProjectEdits::m3_hud_node);
    t.field("m3_lose_node", &forge::edits::ProjectEdits::m3_lose_node);
    t.field("im_ruby", &forge::edits::ProjectEdits::im_ruby);
    t.field("im_chip_rgb", &forge::edits::ProjectEdits::im_chip_rgb);
    t.field("im_px", &forge::edits::ProjectEdits::im_px);
    t.field("im_at", &forge::edits::ProjectEdits::im_at);
    t.field("im_chip", &forge::edits::ProjectEdits::im_chip);
    t.field("im_chip_name", &forge::edits::ProjectEdits::im_chip_name);
    t.field("vs_screen", &forge::edits::ProjectEdits::vs_screen);
    t.field("vs_picture_file", &forge::edits::ProjectEdits::vs_picture_file);
    t.field("vs_picture_svg", &forge::edits::ProjectEdits::vs_picture_svg);
    t.field("vs_frame_file", &forge::edits::ProjectEdits::vs_frame_file);
    t.field("vs_frame_svg", &forge::edits::ProjectEdits::vs_frame_svg);
    t.field("vs_picture", &forge::edits::ProjectEdits::vs_picture);
    t.field("vs_frame", &forge::edits::ProjectEdits::vs_frame);
    t.field("vs_frame_size", &forge::edits::ProjectEdits::vs_frame_size);
    t.field("vs_frame_slice", &forge::edits::ProjectEdits::vs_frame_slice);
    t.field("vs_picture_box", &forge::edits::ProjectEdits::vs_picture_box);
    t.field("vs_frame_box", &forge::edits::ProjectEdits::vs_frame_box);
}

namespace forge::edits {

bool read_edits(const std::filesystem::path& file, ProjectEdits& out, std::string& error) {
    std::vector<forge::u8> bytes;
    if (!forge::read_file(file, bytes)) {
        error = forge::path_to_utf8(file) + " не читается";
        return false;
    }
    forge::data::LoadReport report;
    if (!forge::data::from_json(out, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), report)) {
        error = forge::path_to_utf8(file) + ": " + report.error;
        return false;
    }
    return true;
}

bool write_edits(const std::filesystem::path& file, const ProjectEdits& edits) {
    const std::string text = forge::data::to_json(edits);
    return forge::write_file_atomic(file, std::span(reinterpret_cast<const forge::u8*>(text.data()), text.size()));
}

} // namespace forge::edits
