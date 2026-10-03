#pragma once

// Text form of game data: what the editor saves into the project and what goes
// into git. Readable, diff-friendly, tolerant of change:
//   - missing fields keep their default values,
//   - unknown fields are skipped with a warning,
//   - older objects are migrated (renamed fields automatically, anything
//     else through the type's MigrateFn) before they are read.
//
//   {"$type": "Player", "$v": 2, "name": "Борин", "health": {"current": 80, "max": 100}}

#include "forge/data/reflect.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge::data {

struct LoadReport {
    std::string error;                 // set when nothing usable was loaded
    std::vector<std::string> warnings; // the object loaded, but something was off
    bool ok() const { return error.empty(); }
};

// A saved JSON object as seen by a migration. Changes apply to the object that
// is about to be read.
class JsonObjectEdit {
public:
    JsonObjectEdit(void* doc, void* object) : doc_(doc), obj_(object) {}

    bool valid() const { return obj_ != nullptr; }
    bool has(std::string_view key) const;
    bool rename(std::string_view from, std::string_view to);
    bool remove(std::string_view key);

    std::optional<f64> get_number(std::string_view key) const;
    std::optional<std::string> get_string(std::string_view key) const;
    std::optional<bool> get_bool(std::string_view key) const;

    void set_number(std::string_view key, f64 value);
    void set_int(std::string_view key, i64 value);
    void set_string(std::string_view key, std::string_view value);
    void set_bool(std::string_view key, bool value);

    // Nested object, e.g. edit.child("stats").rename("hp", "health").
    JsonObjectEdit child(std::string_view key) const;

private:
    void* doc_; // yyjson_mut_doc*
    void* obj_; // yyjson_mut_val* (an object) or nullptr
};

std::string to_json(const reflect::TypeInfo* type, const void* object, bool pretty = true);
bool from_json(const reflect::TypeInfo* type, void* object, std::string_view json, LoadReport& report);

template <typename T>
std::string to_json(const T& object, bool pretty = true) {
    return to_json(reflect::type_of<T>(), &object, pretty);
}

template <typename T>
bool from_json(T& object, std::string_view json, LoadReport& report) {
    return from_json(reflect::type_of<T>(), &object, json, report);
}

} // namespace forge::data
