#include "forge/core/file.h"

#include <fstream>

namespace forge {

namespace fs = std::filesystem;

bool read_file(const fs::path& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<usize>(size));
    in.seekg(0);
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

bool write_file_atomic(const fs::path& path, std::span<const u8> bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace forge
