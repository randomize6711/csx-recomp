#include "logs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace tool {

Severity LineSeverity(const std::string& line) {
    size_t close = line.find("] [");
    if (line.size() > 2 && line[0] == '[' && close != std::string::npos) {
        size_t a = close + 3, b = line.find(']', a);
        std::string lv = b == std::string::npos ? std::string() : line.substr(a, b - a);
        if (lv == "warn") return Severity::Warn;
        if (lv == "error") return Severity::Error;
        if (lv == "debug") return Severity::Debug;
        return Severity::Info;
    }
    if (line.find("error:") != std::string::npos || line.rfind("csx-server: ", 0) == 0) return Severity::Error;
    return Severity::Info;
}

void ConsoleBuffer::Add(const std::string& text, Severity sev, int64_t wall_ms) {
    lines_.push_back({text, sev, wall_ms, next_id_++});
    while (lines_.size() > max_) lines_.pop_front(), dropped_++;
}

std::vector<LogFile> ListLogFiles(const std::string& server_dir) {
    std::vector<LogFile> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(server_dir) / "logs", ec)) {
        std::string n = e.path().filename().string();
        if (!e.is_regular_file(ec)) continue;
        if (!(n.rfind("server", 0) == 0 && e.path().extension() == ".log") && n != "admin.log" && n != "admin.log.1") continue;
        LogFile f;
        f.path = e.path().string();
        f.name = n;
        f.size = e.file_size(ec);
        auto t = e.last_write_time(ec);
        f.modified_unix = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
                              .count();
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const LogFile& a, const LogFile& b) {
        return a.modified_unix != b.modified_unix ? a.modified_unix > b.modified_unix : a.name > b.name;
    });
    return out;
}

std::vector<std::string> ReadLogTail(const std::string& path, size_t max_bytes) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    f.seekg(start);
    std::string line;
    if (start > 0) std::getline(f, line);  // a partial first line
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

bool ContainsNoCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

std::string FormatClock(int64_t wall_ms) {
    std::time_t t = std::time_t(wall_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[16];
    std::strftime(b, sizeof(b), "%H:%M:%S", &tm);
    return b;
}

std::string FormatDuration(double seconds) {
    long long s = (long long)std::max(0.0, seconds);
    char b[48];
    if (s >= 86400) std::snprintf(b, sizeof(b), "%lldd %02lld:%02lld:%02lld", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
    else std::snprintf(b, sizeof(b), "%02lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    return b;
}

std::string FormatBytesPerSecond(double bps) {
    char b[32];
    if (bps >= 1024.0 * 1024) std::snprintf(b, sizeof(b), "%.2f MB/s", bps / (1024.0 * 1024));
    else if (bps >= 1024) std::snprintf(b, sizeof(b), "%.1f KB/s", bps / 1024.0);
    else std::snprintf(b, sizeof(b), "%.0f B/s", bps);
    return b;
}

}  // namespace tool
