#pragma once

// Design tokens of Forge UI, read from the design system's tokens.json.
//
// Styles write var(--primary) or var(--radius-md, 12px); the engine replaces
// them with the value for the active theme while it reads the file (RmlUi
// itself has no CSS variables). A theme switch re-reads the styles.
//
// Names available to styles:
//   colours and shadows     --surface, --primary, --elevation-2, ... (per theme)
//   spacing, radius, ...    --space-4, --radius-md, --border-strong, --icon-m, ...
//   durations               --duration-short (converted to seconds for RmlUi)
//   easing                  --ease-standard (an RmlUi tween, see tokens.rml.json)
//   font families           --font-display, --font-sans, --font-mono, --font-icons, ...
//   type styles             --title-medium-size, -line-height, -weight, -letter-spacing, -family
//
// tokens.rml.json next to tokens.json adds what has no direct RmlUi form:
// overrides for single tokens, per-theme changes (fantasy fonts) and
// component tokens. Values there may refer to other tokens as {name} and
// compute colours: mix(primary, on-primary, 0.08), alpha(scrim, 0.5).

#include "forge/core/types.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace forge::ui {

class Tokens {
public:
    // Reads tokens.json and, when present, tokens.rml.json next to it.
    // error explains the first problem; tokens that are malformed are skipped.
    bool load(const std::filesystem::path& tokens_json, std::string* error = nullptr);
    bool load_from_text(std::string_view tokens_json, std::string_view overrides_json, std::string* error = nullptr);

    const std::vector<std::string>& themes() const { return themes_; }
    const std::string& theme() const { return theme_; }
    bool set_theme(std::string_view theme);

    // Value for the active theme, or nullptr.
    const std::string* find(std::string_view name) const;
    usize size() const;

    // Replaces every var(--name) and var(--name, fallback) in text. Names
    // without a value keep the fallback (or become empty) and are listed in
    // missing.
    std::string substitute(std::string_view text, std::vector<std::string>* missing = nullptr) const;

private:
    void set(const std::string& name, const std::string& theme, std::string value);
    void resolve();
    bool parse_color(std::string_view text, float out[4]) const; // a token name or #hex
    // Empty when an argument is not a colour yet (unless report).
    std::string eval_color(const std::string& expression, bool report) const;

    std::vector<std::string> themes_;
    std::string theme_;
    // name -> value per theme ("" = every theme)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> raw_;
    std::unordered_map<std::string, std::string> active_; // resolved for theme_
};

} // namespace forge::ui
