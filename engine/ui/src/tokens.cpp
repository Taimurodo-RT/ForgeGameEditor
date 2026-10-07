#include "forge/ui/tokens.h"

#include "forge/core/file.h"
#include "forge/core/log.h"

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace forge::ui {

namespace {

std::string str(yyjson_val* v) {
    if (yyjson_is_str(v)) return std::string(yyjson_get_str(v), yyjson_get_len(v));
    if (yyjson_is_int(v)) return std::to_string(yyjson_get_int(v));
    if (yyjson_is_real(v)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", yyjson_get_real(v));
        return buf;
    }
    return {};
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// "\"Exo 2\", \"Segoe UI\", system-ui" -> "Exo 2": RmlUi takes one family.
std::string first_family(std::string_view list) {
    const usize comma = list.find(',');
    std::string first = trim(list.substr(0, comma));
    if (first.size() >= 2 && (first.front() == '"' || first.front() == '\'')) first = first.substr(1, first.size() - 2);
    return first;
}

// RmlUi reads durations in seconds only.
std::string to_seconds(const std::string& value) {
    if (value.size() > 2 && value.compare(value.size() - 2, 2, "ms") == 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%gs", std::atof(value.c_str()) / 1000.0);
        return buf;
    }
    return value;
}

struct Doc {
    yyjson_doc* doc = nullptr;
    ~Doc() {
        if (doc) yyjson_doc_free(doc);
    }
};

} // namespace

bool Tokens::load(const std::filesystem::path& tokens_json, std::string* error) {
    std::vector<u8> tokens, overrides;
    if (!read_file(tokens_json, tokens)) {
        if (error) *error = "cannot read " + tokens_json.generic_string();
        return false;
    }
    std::filesystem::path extra = tokens_json;
    extra.replace_filename("tokens.rml.json");
    read_file(extra, overrides); // optional
    return load_from_text({reinterpret_cast<const char*>(tokens.data()), tokens.size()},
                          {reinterpret_cast<const char*>(overrides.data()), overrides.size()}, error);
}

void Tokens::set(const std::string& name, const std::string& theme, std::string value) {
    raw_[name][theme] = std::move(value);
}

bool Tokens::load_from_text(std::string_view tokens_json, std::string_view overrides_json, std::string* error) {
    Doc doc;
    doc.doc = yyjson_read(tokens_json.data(), tokens_json.size(), 0);
    yyjson_val* root = yyjson_doc_get_root(doc.doc);
    if (!yyjson_is_obj(root)) {
        if (error) *error = "tokens.json is not a JSON object";
        return false;
    }
    raw_.clear();
    themes_.clear();

    // Colours: one value per theme.
    yyjson_val* color = yyjson_obj_get(root, "color");
    yyjson_val* theme_list = yyjson_obj_get(color, "themes");
    size_t i, n;
    yyjson_val* item;
    yyjson_arr_foreach(theme_list, i, n, item) {
        const std::string id = str(yyjson_obj_get(item, "id"));
        if (!id.empty()) themes_.push_back(id);
    }
    if (themes_.empty()) themes_.push_back("dark");

    // Tokens whose value is either a string or {theme: string}.
    for (const char* group : {"color", "spacing", "radius", "shadow", "border", "opacity", "duration", "easing",
                              "zIndex", "size"}) {
        yyjson_val* list = yyjson_obj_get(yyjson_obj_get(root, group), "tokens");
        yyjson_arr_foreach(list, i, n, item) {
            const std::string name = str(yyjson_obj_get(item, "name"));
            yyjson_val* value = yyjson_obj_get(item, "value");
            if (name.empty() || !value) continue;
            const bool is_duration = std::string_view(group) == "duration";
            if (yyjson_is_obj(value)) {
                size_t j, m;
                yyjson_val *key, *v;
                yyjson_obj_foreach(value, j, m, key, v) set(name, str(key), str(v));
            } else {
                set(name, "", is_duration ? to_seconds(str(value)) : str(value));
            }
        }
    }

    // Typography: families and type styles.
    yyjson_val* type = yyjson_obj_get(root, "type");
    yyjson_val* families = yyjson_obj_get(type, "families");
    {
        size_t j, m;
        yyjson_val *key, *v;
        yyjson_obj_foreach(families, j, m, key, v) set("font-" + str(key), "", first_family(str(v)));
    }
    yyjson_arr_foreach(yyjson_obj_get(type, "groups"), i, n, item) {
        const std::string group_family = str(yyjson_obj_get(item, "family"));
        size_t j, m;
        yyjson_val* style;
        yyjson_arr_foreach(yyjson_obj_get(item, "styles"), j, m, style) {
            const std::string name = str(yyjson_obj_get(style, "name"));
            if (name.empty()) continue;
            std::string family = str(yyjson_obj_get(style, "family"));
            if (family.empty()) family = group_family;
            // Through the family token, so a theme that swaps fonts swaps these too.
            set(name + "-family", "", "var(--font-" + family + ")");
            set(name + "-size", "", str(yyjson_obj_get(style, "fontSize")));
            set(name + "-line-height", "", str(yyjson_obj_get(style, "lineHeight")));
            set(name + "-weight", "", str(yyjson_obj_get(style, "fontWeight")));
            std::string spacing = str(yyjson_obj_get(style, "letterSpacing"));
            set(name + "-letter-spacing", "", spacing.empty() ? "0px" : spacing);
        }
    }

    // RmlUi-specific overrides.
    if (!overrides_json.empty()) {
        Doc extra;
        extra.doc = yyjson_read(overrides_json.data(), overrides_json.size(), 0);
        yyjson_val* xroot = yyjson_doc_get_root(extra.doc);
        if (!yyjson_is_obj(xroot)) {
            if (error) *error = "tokens.rml.json is not a JSON object";
        } else {
            size_t j, m;
            yyjson_val *key, *v;
            yyjson_obj_foreach(yyjson_obj_get(xroot, "tokens"), j, m, key, v) raw_[str(key)] = {{"", str(v)}};
            yyjson_obj_foreach(yyjson_obj_get(xroot, "themes"), j, m, key, v) {
                const std::string theme = str(key);
                size_t k, o;
                yyjson_val *tk, *tv;
                yyjson_obj_foreach(v, k, o, tk, tv) set(str(tk), theme, str(tv));
            }
        }
    }

    if (theme_.empty() || std::find(themes_.begin(), themes_.end(), theme_) == themes_.end()) theme_ = themes_.front();
    resolve();
    return true;
}

bool Tokens::set_theme(std::string_view theme) {
    for (const std::string& t : themes_) {
        if (t == theme) {
            theme_ = t;
            resolve();
            return true;
        }
    }
    return false;
}

void Tokens::resolve() {
    active_.clear();
    for (const auto& [name, values] : raw_) {
        auto it = values.find(theme_);
        if (it == values.end()) it = values.find("");
        if (it == values.end()) it = values.find(themes_.front());
        if (it != values.end()) active_[name] = it->second;
    }
    // References: "{surface-container-lowest}" and var(--x) inside values.
    for (int pass = 0; pass < 4; ++pass) {
        bool changed = false;
        for (auto& [name, value] : active_) {
            if (value.size() > 2 && value.front() == '{' && value.back() == '}') {
                auto ref = active_.find(value.substr(1, value.size() - 2));
                if (ref != active_.end() && ref->first != name) {
                    value = ref->second;
                    changed = true;
                }
            } else if (value.find("var(") != std::string::npos) {
                value = substitute(value);
                changed = true;
            }
        }
        if (!changed) break;
    }
    // Colour functions: mix(a, b, t) and alpha(a, t), for state layers and
    // tints that CSS would do with color-mix().
    auto is_function = [](const std::string& v) { return v.rfind("mix(", 0) == 0 || v.rfind("alpha(", 0) == 0; };
    for (int pass = 0; pass < 4; ++pass) {
        const bool last = pass == 3;
        bool pending = false;
        for (auto& [name, value] : active_) {
            if (!is_function(value)) continue;
            std::string result = eval_color(value, last);
            if (result.empty()) pending = true; // depends on a colour not computed yet
            else value = std::move(result);
        }
        if (!pending) break;
    }
}

bool Tokens::parse_color(std::string_view text, float out[4]) const {
    std::string s = trim(text);
    if (const std::string* v = find(s)) s = *v;
    if (s.empty() || s[0] != '#') return false;
    const std::string hex = s.substr(1);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int digits[8];
    for (usize i = 0; i < hex.size() && i < 8; ++i)
        if ((digits[i] = nibble(hex[i])) < 0) return false;
    if (hex.size() == 3 || hex.size() == 4) {
        for (usize i = 0; i < 4; ++i)
            out[i] = i < hex.size() ? static_cast<float>(digits[i] * 17) / 255.0f : 1.0f;
        return true;
    }
    if (hex.size() == 6 || hex.size() == 8) {
        for (usize i = 0; i < 4; ++i)
            out[i] = i * 2 < hex.size() ? static_cast<float>(digits[i * 2] * 16 + digits[i * 2 + 1]) / 255.0f : 1.0f;
        return true;
    }
    return false;
}

std::string Tokens::eval_color(const std::string& expression, bool report) const {
    const usize open = expression.find('(');
    const usize close = expression.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return expression;
    const std::string fn = expression.substr(0, open);
    std::vector<std::string> args;
    std::string_view inner(expression.data() + open + 1, close - open - 1);
    usize start = 0;
    for (usize i = 0; i <= inner.size(); ++i) {
        if (i == inner.size() || inner[i] == ',') {
            args.push_back(trim(inner.substr(start, i - start)));
            start = i + 1;
        }
    }
    float a[4], b[4], c[4];
    if (fn == "mix" && args.size() == 3 && parse_color(args[0], a) && parse_color(args[1], b)) {
        const float t = static_cast<float>(std::atof(args[2].c_str()));
        for (int i = 0; i < 4; ++i) c[i] = a[i] + (b[i] - a[i]) * t;
    } else if (fn == "alpha" && args.size() == 2 && parse_color(args[0], a)) {
        for (int i = 0; i < 3; ++i) c[i] = a[i];
        c[3] = a[3] * static_cast<float>(std::atof(args[1].c_str()));
    } else {
        if (!report) return {};
        FORGE_WARN("ui: cannot evaluate token value %s", expression.c_str());
        return "#ff00ff";
    }
    char buf[16];
    auto byte = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
    return buf;
}

const std::string* Tokens::find(std::string_view name) const {
    auto it = active_.find(std::string(name));
    return it != active_.end() ? &it->second : nullptr;
}

usize Tokens::size() const { return active_.size(); }

std::string Tokens::substitute(std::string_view text, std::vector<std::string>* missing) const {
    std::string out;
    out.reserve(text.size());
    usize pos = 0;
    while (true) {
        const usize start = text.find("var(", pos);
        if (start == std::string_view::npos) break;
        // Must be a whole word: "var(" not preceded by an identifier character.
        if (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '-' ||
                          text[start - 1] == '_')) {
            out.append(text.substr(pos, start + 4 - pos));
            pos = start + 4;
            continue;
        }
        // Matching parenthesis (fallbacks may contain calls).
        usize depth = 1, end = start + 4;
        while (end < text.size() && depth > 0) {
            if (text[end] == '(') ++depth;
            else if (text[end] == ')') --depth;
            ++end;
        }
        if (depth != 0) break;
        out.append(text.substr(pos, start - pos));
        std::string_view inner = text.substr(start + 4, end - 1 - (start + 4));
        const usize comma = inner.find(',');
        std::string name = trim(inner.substr(0, comma));
        if (name.rfind("--", 0) == 0) name = name.substr(2);
        if (const std::string* value = find(name)) {
            out += *value;
        } else {
            // Not a token: the style's own custom property (or a typo). RmlUi
            // resolves it, with its fallback, when the style is computed.
            out.append(text.substr(start, end - start));
            if (missing) missing->push_back(name);
        }
        pos = end;
    }
    out.append(text.substr(pos));
    return out;
}

} // namespace forge::ui
