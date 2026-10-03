#include "forge/core/log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace forge {

namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

const char* file_name(const char* path) {
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* last = slash > backslash ? slash : backslash;
    return last ? last + 1 : path;
}
} // namespace

void log_set_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < g_level.load(std::memory_order_relaxed)) return;

    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::lock_guard lock(g_mutex);
    std::fprintf(out, "[%s] %s (%s:%d)\n", level_name(level), message, file_name(file), line);
}

} // namespace forge
