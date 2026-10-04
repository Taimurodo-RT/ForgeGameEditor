#pragma once

// Kinds of files in the «Ресурсы» tab: the name shown in the «Тип» column,
// the icon and its colour. Read from ui/editor/file_types.json, so the list
// is edited like the rest of the UI; the colours are classes in assets.rcss
// ("ft-<id>").

#include "forge/core/types.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge::editor_app {

struct FileType {
    std::string id;   // "image"; the icon's class is "ft-image"
    std::string name; // "Картинка"
    std::string icon; // a Material Symbols name
    bool show_extension = true; // «Картинка PNG» rather than «Картинка»
    std::vector<std::string> extensions; // ".png", lower case
};

struct FileTypeList {
    std::vector<FileType> types;
};

class FileTypes {
public:
    // False when the file is missing or broken; the built-in folder and
    // "other" kinds are there anyway.
    bool load(const std::filesystem::path& file);

    // By the extension (".png", any case); "other" when unknown.
    const FileType& of(const std::string& extension) const;
    const FileType& folder() const { return folder_; }
    // «Картинка PNG», «Файл XYZ», «Папка».
    std::string type_name(const FileType& type, const std::string& extension) const;
    usize size() const { return list_.types.size(); }

private:
    void index();

    FileTypeList list_;
    std::unordered_map<std::string, usize> by_ext_;
    FileType folder_{"folder", "Папка", "folder", false, {}};
    FileType other_{"other", "Файл", "draft", true, {}};
};

} // namespace forge::editor_app
