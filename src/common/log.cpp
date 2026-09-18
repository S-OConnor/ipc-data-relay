#include "ipcrelay/log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace ipcrelay {

static std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level)); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load()); }

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Info: return "INFO";
        case LogLevel::Debug: return "DEBUG";
    }
    return "?";
}

bool parse_log_level(const std::string& text, LogLevel& out) {
    std::string t;
    for (char c : text) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "error") { out = LogLevel::Error; return true; }
    if (t == "warn" || t == "warning") { out = LogLevel::Warn; return true; }
    if (t == "info") { out = LogLevel::Info; return true; }
    if (t == "debug") { out = LogLevel::Debug; return true; }
    return false;
}

void log_write(LogLevel level, const char* fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::fprintf(stderr, "%s.%03ld [%s] %s\n", ts, static_cast<long>(tv.tv_usec / 1000), to_string(level), msg);
    std::fflush(stderr);
}

}  // namespace ipcrelay
