// Application log: an in-memory ring shown in the Log window, optionally mirrored to a file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace logx {

enum class Level { Info, Warn, Error };

struct Entry {
    Level level;
    double time;  // seconds since the log was first used
    std::string text;
};

void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

// Mirrors all later entries to this file (truncated on open). Empty path disables mirroring.
void setFile(const std::string& path);
// Also echo entries to stdout (used by command-line modes).
void setEcho(bool echo);

// Snapshot of the retained entries (oldest first) and a counter that increases with every entry.
std::vector<Entry> entries();
uint64_t revision();
void clear();

} // namespace logx
