#pragma once

// JSON numbers and strings as the files of this library write and read them (motion_file.cpp, scene_file.cpp).

#include "forge/core/types.h"

#include <yyjson.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace forge::vector::json {

// The shortest form that reads back as the same number; 0 for one that is not finite.
inline void put_number(std::string& out, f64 v) {
    if (!std::isfinite(v)) v = 0;
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, ec == std::errc() ? end : buf);
}

inline void put_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += ch;
    }
    out += '"';
}

inline bool number(yyjson_val* v, f64& out) {
    if (!yyjson_is_num(v)) return false;
    out = yyjson_get_num(v);
    return std::isfinite(out);
}

inline bool text(yyjson_val* v, std::string& out) {
    if (!yyjson_is_str(v)) return false;
    out.assign(yyjson_get_str(v), yyjson_get_len(v));
    return true;
}

} // namespace forge::vector::json
