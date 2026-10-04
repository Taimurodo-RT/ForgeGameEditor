#include "file_types.h"

#include "forge/core/file.h"
#include "forge/core/log.h"
#include "forge/core/path.h"
#include "forge/data/json.h"
#include "forge/data/reflect.h"

FORGE_REFLECT_DECLARE(forge::editor_app::FileType)
FORGE_REFLECT_DECLARE(forge::editor_app::FileTypeList)

FORGE_REFLECT(forge::editor_app::FileType, 1) {
    t.field("id", &forge::editor_app::FileType::id);
    t.field("name", &forge::editor_app::FileType::name);
    t.field("icon", &forge::editor_app::FileType::icon);
    t.field("show_extension", &forge::editor_app::FileType::show_extension);
    t.field("extensions", &forge::editor_app::FileType::extensions);
}

FORGE_REFLECT(forge::editor_app::FileTypeList, 1) {
    t.field("types", &forge::editor_app::FileTypeList::types);
}

namespace forge::editor_app {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

bool FileTypes::load(const std::filesystem::path& file) {
    list_ = {};
    std::vector<u8> bytes;
    bool ok = read_file(file, bytes);
    if (ok) {
        data::LoadReport report;
        ok = data::from_json(list_, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), report);
    }
    if (!ok) FORGE_WARN("Типы файлов не прочитались (%s): все файлы будут просто «Файл»", path_to_utf8(file).c_str());
    for (FileType& t : list_.types) {
        for (std::string& e : t.extensions) {
            e = lower(e);
            if (!e.empty() && e[0] != '.') e.insert(e.begin(), '.');
        }
        if (t.id == "folder") folder_ = t;
        if (t.id == "other") other_ = t;
    }
    index();
    return ok;
}

void FileTypes::index() {
    by_ext_.clear();
    for (usize i = 0; i < list_.types.size(); ++i)
        for (const std::string& e : list_.types[i].extensions) by_ext_.emplace(e, i); // the first one named wins
}

const FileType& FileTypes::of(const std::string& extension) const {
    auto it = by_ext_.find(lower(extension));
    return it == by_ext_.end() ? other_ : list_.types[it->second];
}

std::string FileTypes::type_name(const FileType& type, const std::string& extension) const {
    if (!type.show_extension || extension.size() < 2) return type.name;
    std::string upper = extension.substr(1);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return type.name + " " + upper;
}

} // namespace forge::editor_app
