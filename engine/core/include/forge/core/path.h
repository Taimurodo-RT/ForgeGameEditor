#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace forge {

// All engine text is UTF-8. std::filesystem::path built from a plain char
// string uses the system code page on Windows, which garbles non-ASCII names
// (Cyrillic folders, for one). Convert through these two helpers instead.

inline std::filesystem::path utf8_path(std::string_view utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Generic form: '/' separators on every platform.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

} // namespace forge
