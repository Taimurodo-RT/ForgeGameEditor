#pragma once

#include <cstdarg>

namespace forge {

enum class LogLevel { Trace, Info, Warn, Error };

void log_set_level(LogLevel level);
void log_write(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

} // namespace forge

#define FORGE_TRACE(...) ::forge::log_write(::forge::LogLevel::Trace, __FILE__, __LINE__, __VA_ARGS__)
#define FORGE_INFO(...) ::forge::log_write(::forge::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define FORGE_WARN(...) ::forge::log_write(::forge::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)
#define FORGE_ERROR(...) ::forge::log_write(::forge::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
