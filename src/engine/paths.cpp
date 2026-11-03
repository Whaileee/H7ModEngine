#include "paths.h"
#include <windows.h>

namespace h7 {

const std::wstring& EngineRoot() {
    static std::wstring root = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe;
        // ...\Binaries\Win64\MMH7Game-Win64-Shipping.exe -> game root is three levels up.
        for (int i = 0; i < 3; ++i) p = p.substr(0, p.find_last_of(L"\\/"));
        return p + L"\\H7ModEngine";
    }();
    return root;
}

std::wstring EnginePath(const std::wstring& rel) { return EngineRoot() + L"\\" + rel; }

int ConfigInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return (int)GetPrivateProfileIntW(section, key, fallback, EnginePath(L"H7ModEngine.ini").c_str());
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

} // namespace h7
