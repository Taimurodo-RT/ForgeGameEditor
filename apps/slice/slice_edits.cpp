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
    t.field("absent", &slice::ProjectEdits::absent);
    t.field("level", &slice::ProjectEdits::level);
    t.field("level_name", &slice::ProjectEdits::level_name);
    t.field("cells", &slice::ProjectEdits::cells);
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
