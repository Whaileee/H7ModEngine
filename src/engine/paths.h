#pragma once
#include <string>

namespace h7 {

// <GameRoot>\H7ModEngine — holds H7ModEngine.ini, mods\, logs\, dumps\.
const std::wstring& EngineRoot();
std::wstring EnginePath(const std::wstring& rel);
int ConfigInt(const wchar_t* section, const wchar_t* key, int fallback);
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

} // namespace h7
