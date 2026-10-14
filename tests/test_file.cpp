#include "forge/core/file.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace forge;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::vector<u8> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

std::string text_of(const fs::path& file) {
    std::vector<u8> back;
    if (!read_file(file, back)) return "(unread)";
    return {back.begin(), back.end()};
}

// Opened as «Ресурсы» open a file they hash on their own thread (assets::hash_file): on Windows nobody may replace it
// until it is closed. The reader opens it, says so, holds it until the writer has begun and `hold` after that, and
// notes the time just before it lets go: no rename onto the file goes through before that time.
struct Reader {
    fs::path file;
    std::chrono::milliseconds hold;
    std::atomic<int> state{0}; // 1: open (or failed to), 2: the writer began
    bool opened = false;
    Clock::time_point letting_go;
    std::thread thread;

    Reader(fs::path f, std::chrono::milliseconds h) : file(std::move(f)), hold(h) {
        thread = std::thread([this] {
#if defined(_WIN32)
            std::FILE* fp = nullptr;
            if (_wfopen_s(&fp, file.c_str(), L"rb") != 0) fp = nullptr;
#else
            std::FILE* fp = std::fopen(file.c_str(), "rb");
#endif
            opened = fp != nullptr;
            state = 1;
            while (state.load() < 2) std::this_thread::yield();
            std::this_thread::sleep_for(hold);
            letting_go = Clock::now();
            if (fp) std::fclose(fp);
        });
        while (state.load() < 1) std::this_thread::yield();
    }
    void begin() { state = 2; }
    void join() {
        state = 2;
        if (thread.joinable()) thread.join();
    }
    ~Reader() { join(); }
};

#if defined(_WIN32)
// The hold is real: a plain rename onto the file is refused while it lasts.
bool plain_rename_refused(const fs::path& dir, const fs::path& file) {
    const fs::path other = dir / "other.json";
    { std::ofstream(other, std::ios::binary) << "other"; }
    std::error_code ec;
    fs::rename(other, file, ec);
    std::error_code gone;
    fs::remove(other, gone);
    return static_cast<bool>(ec);
}
#endif

} // namespace

TEST_CASE("write_file_atomic replaces a file another thread is reading for a moment") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "held.json";
    REQUIRE(write_file_atomic(file, bytes_of("old")));

    Reader reader(file, std::chrono::milliseconds(200));
    CHECK(reader.opened);
#if defined(_WIN32)
    CHECK(plain_rename_refused(dir, file));
    CHECK(text_of(file) == "old");
#endif
    reader.begin();
    const bool written = write_file_atomic(file, bytes_of("new"));
    const Clock::time_point done = Clock::now();
    reader.join();

    CHECK(written);
    CHECK(text_of(file) == "new");
    CHECK_FALSE(fs::exists(dir / "held.json.tmp"));
#if defined(_WIN32)
    // It went through only once the reader let go: it waited for it, and was not let through while it held.
    CHECK(done > reader.letting_go);
#else
    (void)done;
#endif
    fs::remove_all(dir, ec);
}

TEST_CASE("write_file_atomic gives up on a file held past a second and leaves it as it was") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file_held";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "held.json";
    REQUIRE(write_file_atomic(file, bytes_of("old")));

#if defined(_WIN32)
    Reader reader(file, std::chrono::milliseconds(3000));
#else
    Reader reader(file, std::chrono::milliseconds(200));
#endif
    CHECK(reader.opened);
    reader.begin();
    const Clock::time_point t0 = Clock::now();
    const bool written = write_file_atomic(file, bytes_of("new"));
    const auto took = Clock::now() - t0;
    reader.join();

#if defined(_WIN32)
    CHECK_FALSE(written);
    CHECK(took >= std::chrono::seconds(1));
    CHECK(took < std::chrono::milliseconds(2500));
    CHECK(text_of(file) == "old");
#else
    // Nothing holds a file here: the rename goes through at once.
    CHECK(written);
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(text_of(file) == "new");
#endif
    CHECK_FALSE(fs::exists(dir / "held.json.tmp"));
    fs::remove_all(dir, ec);
}

TEST_CASE("write_file_atomic onto a folder fails at once and leaves no temporary file") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file_folder";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "taken" / "inside", ec);
    const auto t0 = Clock::now();
    CHECK_FALSE(write_file_atomic(dir / "taken", bytes_of("x")));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK(fs::is_directory(dir / "taken" / "inside"));
    CHECK_FALSE(fs::exists(dir / "taken.tmp"));
    fs::remove_all(dir, ec);
}

TEST_CASE("write_file_atomic onto a read-only file: refused at once on Windows, the file as it was") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file_readonly";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "locked.json";
    REQUIRE(write_file_atomic(file, bytes_of("old")));
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    REQUIRE_FALSE(ec);

    const auto t0 = Clock::now();
    const bool written = write_file_atomic(file, bytes_of("new"));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(500));
#if defined(_WIN32)
    // "Access denied" with nobody holding it: no reader to wait for.
    CHECK_FALSE(written);
    CHECK(text_of(file) == "old");
#else
    // A rename replaces the name whatever the file's own mode.
    CHECK(written);
    CHECK(text_of(file) == "new");
#endif
    CHECK_FALSE(fs::exists(dir / "locked.json.tmp"));
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(dir, ec);
}
