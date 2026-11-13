#include "Logger.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <mutex>
#include <atomic>

namespace {
    FILE* g_file = nullptr;
    std::filesystem::path g_path;
    std::mutex g_mutex;
    std::deque<Logger::Entry> g_entries;
    std::atomic<size_t> g_version{ 0 };
    constexpr size_t kMaxEntries = 1000;
}

void Logger::Init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    g_path = file;
    _wfopen_s(&g_file, file.c_str(), L"w");
}

void Logger::Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void Logger::Write(const char* level, const char* fmt, ...) {
    char msg[1024];
    va_list args; va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t; GetLocalTime(&t);
    char time[16];
    snprintf(time, sizeof(time), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    char line[1200];
    snprintf(line, sizeof(line), "[%s] [%s] %s\n", time, level, msg);

    OutputDebugStringA(line); // visible avec DebugView
    std::lock_guard lock(g_mutex);
    if (g_file) { fputs(line, g_file); fflush(g_file); }

    const Level lv = strcmp(level, "ERREUR") == 0 ? Level::Error : strcmp(level, "WARN") == 0 ? Level::Warn : Level::Info;
    g_entries.push_back({ time, lv, msg });
    if (g_entries.size() > kMaxEntries) g_entries.pop_front();
    ++g_version;
}

void Logger::Snapshot(std::vector<Entry>& out) {
    std::lock_guard lock(g_mutex);
    out.assign(g_entries.begin(), g_entries.end());
}

size_t Logger::Version() { return g_version.load(); }

void Logger::ClearView() {
    std::lock_guard lock(g_mutex);
    g_entries.clear();
    ++g_version;
}

const std::filesystem::path& Logger::FilePath() { return g_path; }
