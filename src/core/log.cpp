#include "log.h"
#include <windows.h>
#include <cstdio>
#include <mutex>

namespace h7 {

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void LogInit(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    // Fully shared: Ubisoft Connect starts the exe, which exits and is relaunched, so two processes
    // briefly overlap. The log can also be tailed while the game runs.
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (!g_file) {
        std::wstring alt = path.substr(0, path.size() - 4) + L"_" + std::to_wstring(GetCurrentProcessId()) + L".log";
        g_file = _wfsopen(alt.c_str(), L"w", _SH_DENYNO);
    }
}

void LogRaw(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (g_file) {
        fputs(stamp, g_file);
        fputs(line.c_str(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    OutputDebugStringA(("[H7ModEngine] " + line + "\n").c_str());
}

void Log(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogRaw(buf);
}

std::string HexDump(const void* data, size_t len, uintptr_t displayBase) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    char line[160];
    for (size_t i = 0; i < len; i += 16) {
        int n = snprintf(line, sizeof(line), "  +%04zx %016llx: ", i, (unsigned long long)(displayBase + i));
        for (size_t j = 0; j < 16; ++j)
            n += snprintf(line + n, sizeof(line) - n, i + j < len ? "%02x " : "   ", i + j < len ? p[i + j] : 0);
        n += snprintf(line + n, sizeof(line) - n, " ");
        for (size_t j = 0; j < 16 && i + j < len; ++j)
            line[n++] = (p[i + j] >= 32 && p[i + j] < 127) ? (char)p[i + j] : '.';
        line[n++] = '\n';
        line[n] = 0;
        out += line;
    }
    return out;
}

} // namespace h7
