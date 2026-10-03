#pragma once

// Whole-file reads and crash-safe writes.

#include "forge/core/types.h"

#include <filesystem>
#include <span>
#include <vector>

namespace forge {

bool read_file(const std::filesystem::path& path, std::vector<u8>& out);

// Writes to a temporary file next to the target, then renames it over the
// target: a crash or a concurrent reader never sees half a file. Creates
// missing folders.
bool write_file_atomic(const std::filesystem::path& path, std::span<const u8> bytes);

} // namespace forge
