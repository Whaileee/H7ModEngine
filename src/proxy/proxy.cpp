// dinput8.dll proxy: the game imports DirectInput8Create, so Windows loads this DLL from the
// game folder. Every export forwards to the real system dinput8.dll.
#include <windows.h>

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, void* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(Real("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick() {
    auto fn = reinterpret_cast<LPCVOID(WINAPI*)()>(Real("GetdfDIJoystick"));
    return fn ? fn() : nullptr;
}

} // extern "C"
