#include "core/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace logx {
namespace {

constexpr size_t kMaxEntries = 2000;

struct State {
    std::mutex mutex;
    std::deque<Entry> entries;
    uint64_t revision = 0;
    FILE* file = nullptr;
    bool echo = false;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
    static State s;
    return s;
}

const char* prefix(Level level) {
    switch (level) {
    case Level::Info: return "info ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "";
}

void add(Level level, const char* fmt, va_list args) {
    char buf[2048];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    s.entries.push_back({level, t, buf});
    if (s.entries.size() > kMaxEntries) s.entries.pop_front();
    ++s.revision;
    if (s.file) {
        std::fprintf(s.file, "[%9.3f] %s %s\n", t, prefix(level), buf);
        std::fflush(s.file);
    }
    if (s.echo) {
        std::fprintf(level == Level::Info ? stdout : stderr, "%s %s\n", prefix(level), buf);
        std::fflush(level == Level::Info ? stdout : stderr);
    }
}

} // namespace

void info(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    add(Level::Info, fmt, a);
    va_end(a);
}

void warn(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    add(Level::Warn, fmt, a);
    va_end(a);
}

void error(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    add(Level::Error, fmt, a);
    va_end(a);
}

void setFile(const std::string& path) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) std::fclose(s.file);
    s.file = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
}

void setEcho(bool echo) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.echo = echo;
}

std::vector<Entry> entries() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return {s.entries.begin(), s.entries.end()};
}

uint64_t revision() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.revision;
}

void clear() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    ++s.revision;
}

} // namespace logx
