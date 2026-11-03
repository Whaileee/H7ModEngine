#pragma once
#include <string>
#include <cstdarg>

namespace h7 {

void LogInit(const std::wstring& path);
void Log(const char* fmt, ...);
void LogRaw(const std::string& line);
std::string HexDump(const void* data, size_t len, uintptr_t displayBase);

} // namespace h7
