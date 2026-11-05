#pragma once
#include <string>

namespace h7 {

// <GameRoot>\H7ModEngine — holds H7ModEngine.ini, mods\, logs\, dumps\.
const std::wstring& EngineRoot();
std::wstring EnginePath(const std::wstring& rel);
int ConfigInt(const wchar_t* section, const wchar_t* key, int fallback);
// Writes key=value into the ini only when the key is missing, so options added in newer versions show up in
// existing installs (the default ini is only installed when there is none).
void ConfigDefault(const wchar_t* section, const wchar_t* key, const wchar_t* value);
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

} // namespace h7
