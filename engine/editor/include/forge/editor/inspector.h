#pragma once

// The inspector's view of a component, built from reflection: one row per
// field (nested structs and arrays become indented rows), with the value as
// text. Edits come back as text for one field path. The UI decides how a row
// looks (number field, switch, dropdown) from its kind.
//
// Paths: "speed", "stats.max", "items.2.name" (array element 2).

#include "forge/core/types.h"
#include "forge/data/reflect.h"

#include <string>
#include <string_view>
#include <vector>

namespace forge::editor {

struct FieldRow {
    std::string path;
    std::string label; // the field's label, or its name
    std::string tooltip;
    reflect::Kind kind = reflect::Kind::Struct;
    std::string value; // text form; empty for structs and arrays
    u32 depth = 0;
    bool read_only = false;
    bool has_range = false;
    f64 min = 0, max = 0;
    std::vector<std::string> options; // enum value names
};

void describe_fields(const reflect::TypeInfo* type, const void* object, std::vector<FieldRow>& out);

// Text form of one value ("1.5", "true", "0.5, 2", "#ff8800ff", enum name).
std::string value_text(const reflect::TypeInfo* type, const void* value);

// Parses text into the field at path. Numbers outside the field's range are
// clamped. False (with error) when the path or the text is invalid.
bool set_field_text(const reflect::TypeInfo* type, void* object, std::string_view path, std::string_view text,
                    std::string* error = nullptr);

} // namespace forge::editor
