// Small leveled logger writing to stderr. Debug output is compiled in but
// gated at runtime so it can be disabled for high-rate operation (BRG-115).
#pragma once

#include <cstdarg>
#include <string>

namespace ipcrelay {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
bool parse_log_level(const std::string& text, LogLevel& out);
const char* to_string(LogLevel level);

inline bool log_enabled(LogLevel level) { return static_cast<int>(level) <= static_cast<int>(log_level()); }

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void log_write(LogLevel level, const char* fmt, ...);

}  // namespace ipcrelay

#define IPC_LOG(level, ...)                                             \
    do {                                                                \
        if (::ipcrelay::log_enabled(level)) ::ipcrelay::log_write(level, __VA_ARGS__); \
    } while (0)

#define LOG_ERROR(...) IPC_LOG(::ipcrelay::LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...) IPC_LOG(::ipcrelay::LogLevel::Warn, __VA_ARGS__)
#define LOG_INFO(...) IPC_LOG(::ipcrelay::LogLevel::Info, __VA_ARGS__)
#define LOG_DEBUG(...) IPC_LOG(::ipcrelay::LogLevel::Debug, __VA_ARGS__)
