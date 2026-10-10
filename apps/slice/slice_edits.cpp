#include "slice_edits.h"

#include "forge/core/file.h"
#include "forge/core/path.h"
#include "forge/data/json.h"

#include <span>

FORGE_REFLECT(slice::ProjectEdits, 1) {
    t.field("object", &slice::ProjectEdits::object);
    t.field("object_name", &slice::ProjectEdits::object_name);
    t.field("x", &slice::ProjectEdits::x);
    t.field("y", &slice::ProjectEdits::y);
    t.field("color", &slice::ProjectEdits::color);
    t.field("var", &slice::ProjectEdits::var);
    t.field("value", &slice::ProjectEdits::value);
    t.field("screen", &slice::ProjectEdits::screen);
    t.field("text", &slice::ProjectEdits::text);
    t.field("text_node", &slice::ProjectEdits::text_node);
    t.field("picture_node", &slice::ProjectEdits::picture_node);
    t.field("button_node", &slice::ProjectEdits::button_node);
    t.field("sound", &slice::ProjectEdits::sound);
    t.field("sound_frames", &slice::ProjectEdits::sound_frames);
    t.field("motion_scale", &slice::ProjectEdits::motion_scale);
    t.field("motion_seconds", &slice::ProjectEdits::motion_seconds);
    t.field("absent", &slice::ProjectEdits::absent);
    t.field("level", &slice::ProjectEdits::level);
    t.field("level_name", &slice::ProjectEdits::level_name);
    t.field("cells", &slice::ProjectEdits::cells);
    t.field("go_continue", &slice::ProjectEdits::go_continue);
    t.field("go_start", &slice::ProjectEdits::go_start);
    t.field("go_through", &slice::ProjectEdits::go_through);
    t.field("go_level", &slice::ProjectEdits::go_level);
    t.field("go_arrive", &slice::ProjectEdits::go_arrive);
    t.field("go_mark_put", &slice::ProjectEdits::go_mark_put);
    t.field("go_mark_find", &slice::ProjectEdits::go_mark_find);
    t.field("go_save", &slice::ProjectEdits::go_save);
    t.field("pl_enemy", &slice::ProjectEdits::pl_enemy);
    t.field("pl_coin", &slice::ProjectEdits::pl_coin);
    t.field("pl_trap", &slice::ProjectEdits::pl_trap);
    t.field("pl_goal", &slice::ProjectEdits::pl_goal);
    t.field("pl_at", &slice::ProjectEdits::pl_at);
    t.field("pl_damage", &slice::ProjectEdits::pl_damage);
    t.field("pl_enemy_score", &slice::ProjectEdits::pl_enemy_score);
    t.field("pl_coin_score", &slice::ProjectEdits::pl_coin_score);
    t.field("pl_trap_damage", &slice::ProjectEdits::pl_trap_damage);
    t.field("pl_trap_half_w", &slice::ProjectEdits::pl_trap_half_w);
    t.field("pl_pit", &slice::ProjectEdits::pl_pit);
    t.field("pl_hud", &slice::ProjectEdits::pl_hud);
    t.field("pl_lose", &slice::ProjectEdits::pl_lose);
    t.field("pl_win", &slice::ProjectEdits::pl_win);
    t.field("pl_hud_text", &slice::ProjectEdits::pl_hud_text);
    t.field("pl_lose_text", &slice::ProjectEdits::pl_lose_text);
    t.field("pl_lose_again", &slice::ProjectEdits::pl_lose_again);
    t.field("pl_win_text", &slice::ProjectEdits::pl_win_text);
    t.field("pl_goal_color", &slice::ProjectEdits::pl_goal_color);
    t.field("tp_hero", &slice::ProjectEdits::tp_hero);
    t.field("tp_coin", &slice::ProjectEdits::tp_coin);
    t.field("tp_coin_at", &slice::ProjectEdits::tp_coin_at);
    t.field("tp_coin_score", &slice::ProjectEdits::tp_coin_score);
    t.field("tp_enemy", &slice::ProjectEdits::tp_enemy);
    t.field("tp_enemy_speed", &slice::ProjectEdits::tp_enemy_speed);
    t.field("tp_enemy_damage", &slice::ProjectEdits::tp_enemy_damage);
    t.field("tp_bridge", &slice::ProjectEdits::tp_bridge);
    t.field("tp_exit", &slice::ProjectEdits::tp_exit);
    t.field("tp_exit_level", &slice::ProjectEdits::tp_exit_level);
    t.field("tp_exit_arrive", &slice::ProjectEdits::tp_exit_arrive);
    t.field("tp_pit", &slice::ProjectEdits::tp_pit);
    t.field("tp_goal", &slice::ProjectEdits::tp_goal);
    t.field("tp_hud", &slice::ProjectEdits::tp_hud);
    t.field("tp_lose", &slice::ProjectEdits::tp_lose);
    t.field("tp_win", &slice::ProjectEdits::tp_win);
    t.field("tp_hud_score", &slice::ProjectEdits::tp_hud_score);
    t.field("tp_again", &slice::ProjectEdits::tp_again);
    t.field("tp_win_title", &slice::ProjectEdits::tp_win_title);
    t.field("tp_win_text", &slice::ProjectEdits::tp_win_text);
    t.field("tp_win_again", &slice::ProjectEdits::tp_win_again);
    t.field("tp_win_again_label", &slice::ProjectEdits::tp_win_again_label);
    t.field("tp_win_again_text", &slice::ProjectEdits::tp_win_again_text);
    t.field("tp_twin", &slice::ProjectEdits::tp_twin);
}

namespace slice {

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

} // namespace slice
