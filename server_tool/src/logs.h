// The server's output as the tool shows it: the live console (bounded, so a chatty server cannot use up memory) and
// the log files of previous sessions (the server keeps its last 30 runs: logs/server_NNN.log).
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace tool {

enum class Severity { Debug, Info, Warn, Error, Tool };

// "[   12.345] [warn] [t1234] text": the level; a tool's own line is Tool
Severity LineSeverity(const std::string& line);

struct ConsoleLine {
    std::string text;
    Severity sev = Severity::Info;
    int64_t wall_ms = 0;  // when the tool received it (Unix ms): the server's own stamp is seconds since its start
    uint64_t id = 0;      // increasing
};

class ConsoleBuffer {
public:
    explicit ConsoleBuffer(size_t max_lines = 50000) : max_(max_lines) {}
    void Add(const std::string& text, Severity sev, int64_t wall_ms);
    void Clear() { lines_.clear(); }
    const std::deque<ConsoleLine>& Lines() const { return lines_; }
    uint64_t LastId() const { return next_id_ - 1; }
    size_t Dropped() const { return dropped_; }

private:
    size_t max_;
    std::deque<ConsoleLine> lines_;
    uint64_t next_id_ = 1;
    size_t dropped_ = 0;
};

struct LogFile {
    std::string path, name;
    uintmax_t size = 0;
    int64_t modified_unix = 0;
};
// newest first: server_NNN.log and admin.log(.1) of a server folder's logs/
std::vector<LogFile> ListLogFiles(const std::string& server_dir);
// a log's lines, at most max_bytes from its end
std::vector<std::string> ReadLogTail(const std::string& path, size_t max_bytes = 8u << 20);

// case-insensitive substring search
bool ContainsNoCase(const std::string& text, const std::string& needle);

std::string FormatClock(int64_t wall_ms);  // local HH:MM:SS
std::string FormatDuration(double seconds);  // 1d 02:03:04 / 02:03:04
std::string FormatBytesPerSecond(double bps);

}  // namespace tool
