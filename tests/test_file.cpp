#include "forge/core/file.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace forge;
namespace fs = std::filesystem;

namespace {
std::vector<u8> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }
} // namespace

TEST_CASE("write_file_atomic replaces a file another thread is reading for a moment") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path file = dir / "held.json";
    REQUIRE(write_file_atomic(file, bytes_of("old")));

    // Opened as «Ресурсы» open a file they hash on their own thread (assets::hash_file): on Windows nobody may replace
    // it until it is closed.
    std::atomic<bool> opened{false};
    std::thread reader([&] {
#if defined(_WIN32)
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") != 0) f = nullptr;
#else
        std::FILE* f = std::fopen(file.c_str(), "rb");
#endif
        opened = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (f) std::fclose(f);
    });
    while (!opened) std::this_thread::yield();
    const bool written = write_file_atomic(file, bytes_of("new"));
    reader.join();

    CHECK(written);
    std::vector<u8> back;
    CHECK(read_file(file, back));
    CHECK(back == bytes_of("new"));
    CHECK_FALSE(fs::exists(dir / "held.json.tmp"));
    fs::remove_all(dir, ec);
}

TEST_CASE("write_file_atomic onto a folder fails at once and leaves no temporary file") {
    const fs::path dir = fs::temp_directory_path() / "forge_tests_file_folder";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "taken" / "inside", ec);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(write_file_atomic(dir / "taken", bytes_of("x")));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK(fs::is_directory(dir / "taken" / "inside"));
    CHECK_FALSE(fs::exists(dir / "taken.tmp"));
    fs::remove_all(dir, ec);
}
