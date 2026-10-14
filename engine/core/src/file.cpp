#include "forge/core/file.h"

#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <thread>
#endif

namespace forge {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
// Windows replaces no file while someone has it open without sharing its deletion: «Ресурсы» hash what was just
// written on their own thread (assets::hash_file), an antivirus reads it. Such a reader lets go within milliseconds.
// The rename then fails with a sharing or lock violation, or with "access denied", which a read-only file, a folder
// or a file we may not delete give too. Asking for the right to delete the target and the temporary file, sharing
// everything, tells them apart: a reader still holding one refuses that with a sharing violation.
enum class Hold : u8 {
    Reader, // someone has it open: it lets go soon
    Free,   // nobody holds it now: perhaps the reader just let go
    Never,  // refused for good: a folder, a read-only file, no right, no path
};

bool held(const fs::path& path) {
    const HANDLE h = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return false;
    }
    const DWORD why = GetLastError();
    return why == ERROR_SHARING_VIOLATION || why == ERROR_LOCK_VIOLATION;
}

Hold hold_of(const std::error_code& ec, const fs::path& path, const fs::path& tmp) {
    if (ec.category() != std::system_category()) return Hold::Never;
    const int code = ec.value();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) return Hold::Reader;
    if (code != ERROR_ACCESS_DENIED) return Hold::Never;
    std::error_code folder;
    if (fs::is_directory(path, folder)) return Hold::Never;
    return held(path) || held(tmp) ? Hold::Reader : Hold::Free;
}
#endif

} // namespace

bool read_file(const fs::path& path, std::vector<u8>& out) {
    // A folder is no file (on Linux the stream opens it and reports an endless size).
    std::error_code ec;
    if (fs::is_directory(path, ec)) return false;
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
#if defined(_WIN32)
    // While a reader holds it, again every 10 ms for a second; refused with nobody holding it, once more (the reader
    // may have just let go), then no more. The target stays as it was until a rename goes through.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (int unheld = 0; ec && std::chrono::steady_clock::now() < until;) {
        const Hold h = hold_of(ec, path, tmp);
        if (h == Hold::Never || (h == Hold::Free && ++unheld > 1)) break;
        if (h == Hold::Reader) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ec.clear();
        fs::rename(tmp, path, ec);
    }
#endif
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace forge
