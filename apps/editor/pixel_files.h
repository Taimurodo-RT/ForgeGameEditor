#pragma once

// What pixel_editor.cpp and pixel_motion.cpp share: the step of the history that writes a character's files, and the
// words and file helpers of «Векторная».

#include "pixel_editor.h"

#include "asset_library.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/editor/undo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::editor_app::pixel_files {

// New contents of the character's files (none: the file is not there), written at once; Ctrl+Z writes them back. A
// folder the files made (`made`; outer ones first) goes on Ctrl+Z when nothing else is in it.
class PixelFiles final : public editor::Command {
public:
    struct File {
        std::filesystem::path path;
        std::optional<std::vector<u8>> before, after;
    };
    PixelFiles(PixelEditor& editor, AssetLibrary& assets, std::vector<File> files, std::string label, std::filesystem::path made = {})
        : PixelFiles(editor, assets, std::move(files), std::move(label), made.empty() ? std::vector<std::filesystem::path>{} : std::vector{made}) {}
    PixelFiles(PixelEditor& editor, AssetLibrary& assets, std::vector<File> files, std::string label, std::vector<std::filesystem::path> made)
        : editor_(editor), assets_(assets), files_(std::move(files)), label_(std::move(label)), made_(std::move(made)) {}
    void apply(editor::Document&) override { put(true); }
    void revert(editor::Document&) override { put(false); }
    std::string label() const override { return label_; }

private:
    void put(bool forward) {
        for (const File& f : files_) {
            const std::optional<std::vector<u8>>& bytes = forward ? f.after : f.before;
            std::error_code ec;
            if (!bytes) std::filesystem::remove(f.path, ec);
            else if (!write_file_atomic(f.path, *bytes)) FORGE_ERROR("Векторная: %s не записывается", path_to_utf8(f.path).c_str());
        }
        if (!forward)
            for (auto it = made_.rbegin(); it != made_.rend(); ++it) {
                std::error_code ec;
                if (std::filesystem::is_empty(*it, ec)) std::filesystem::remove(*it, ec);
            }
        editor_.reload();
        assets_.refresh(); // «Ресурсы» and the game's copies see the files as they are now
    }
    PixelEditor& editor_;
    AssetLibrary& assets_;
    std::vector<File> files_;
    std::string label_;
    std::vector<std::filesystem::path> made_;
};

// 0.25 → "0,25".
inline std::string number_words(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

// Seconds as the tab writes them: to thousandths, "0,25".
inline std::string seconds_words(f64 s) { return number_words(std::round(s * 1000) / 1000); }

inline std::string count_words(u32 n, const char* one, const char* few, const char* many) {
    const u32 d = n % 10, h = n % 100;
    return std::to_string(n) + " " + (d == 1 && h != 11 ? one : d >= 2 && d <= 4 && (h < 12 || h > 14) ? few : many);
}

// The vector view, and the box the pixel frame is shown big in (logical px).
inline constexpr u32 kViewPx = 288;

// When a file was last written and how long it is; 0 when it is not there.
inline i64 stamp_of(const std::filesystem::path& file) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec) return 0;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) return 0;
    return static_cast<i64>(t.time_since_epoch().count()) * 31 + static_cast<i64>(size) + 1;
}

inline bool read_text(const std::filesystem::path& file, std::string& out) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

inline std::optional<std::vector<u8>> bytes_of(const std::filesystem::path& file) {
    std::vector<u8> bytes;
    if (!read_file(file, bytes)) return std::nullopt;
    return bytes;
}

inline std::vector<u8> text_bytes(const std::string& text) { return {text.begin(), text.end()}; }

// The words of a call's text: on one line, not longer than a line of the inspector.
inline std::string call_words(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    if (out.size() > 96) {
        usize cut = 93;
        while (cut > 0 && (static_cast<u8>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut) + "…";
    }
    return out;
}

} // namespace forge::editor_app::pixel_files
