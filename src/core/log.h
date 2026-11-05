#pragma once
#include <string>
#include <cstdarg>
#include <cstdint>
#include <vector>

namespace h7 {

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);
void LogRaw(const std::string& line);
// The most recent log lines (without timestamps), numbered from 1, for in-game viewers such as the debug console.
// Appends to `out` every kept line numbered above `after`; returns the number of the newest line (0 if none).
uint64_t LogHistory(uint64_t after, std::vector<std::string>& out);
std::string HexDump(const void* data, size_t len, uintptr_t displayBase);

} // namespace h7
