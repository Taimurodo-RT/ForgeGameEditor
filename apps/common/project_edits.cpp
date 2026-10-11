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
