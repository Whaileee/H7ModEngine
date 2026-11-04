#include <windows.h>
#include "paths.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../lua/runtime.h"
#include "../ue3/core.h"
#include "../ue3/dumper.h"
#include "../ue3/hooks.h"
#include "../ue3/reflection.h"
#include "../ue3/factory.h"

using namespace h7;

static bool WaitFor(const char* what, int timeoutMs, bool (*probe)()) {
    for (int waited = 0; waited < timeoutMs; waited += 250) {
        if (probe()) return true;
        Sleep(250);
    }
    Log("Timed out waiting for %s", what);
    return false;
}

static ue3::UObject g_viewport = 0;

static bool FindViewportClient() {
    ue3::UObject cls = ue3::FindClass("Engine.GameViewportClient");
    if (!cls || !ue3::IsCalibrated()) return false;
    for (int32_t i = 0, n = ue3::ObjectCount(); i < n; ++i) {
        ue3::UObject o = ue3::ObjectAt(i);
        if (o && ue3::IsA(o, cls) && ue3::ObjName(o).rfind("Default__", 0) != 0) {
            g_viewport = o;
            return true;
        }
    }
    return false;
}

// The engine heartbeat: GameViewportClient.PostRender(Canvas) runs every drawn frame on the game thread.
// (GameViewportClient.Tick can't be used: it is an empty probe event, so ProcessEvent never calls it.)
static bool InstallTickHook() {
    ue3::UObject fn = ue3::FindField(ue3::ObjClass(g_viewport), "PostRender");
    if (!fn) return false;
    int id = hooks::Add(fn, nullptr, [](hooks::CallContext& c) {
        static LARGE_INTEGER freq{}, last{};
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        float dt = last.QuadPart ? float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart)) : 1.0f / 60.0f;
        last = now;
        if (dt < 0 || dt > 1.0f) dt = 1.0f / 60.0f; // after loading screens
        hooks::Maintain();
        lua::OnGameTick(dt, c.self);
    }, /*viaFunc*/ true); // installed from this thread; the engine calls PostRender from C++ anyway
    Log("Tick hook on %s: %s", ue3::ObjFullName(fn).c_str(), id ? "ok" : "FAILED");
    return id != 0;
}

static DWORD WINAPI InitThread(LPVOID) {
    for (auto dir : {L"", L"logs", L"dumps", L"mods", L"lib", L"exec", L"data"})
        CreateDirectoryW((*dir ? EnginePath(dir) : EngineRoot()).c_str(), nullptr);
    LogInit(EnginePath(L"logs\\engine.log"));
    mem::InitGameModule();
    Log("H7ModEngine starting; game image %p-%p", (void*)mem::g_game.base, (void*)mem::g_game.end);

    // The executable is packed: wait until the engine has unpacked and built its name/object tables.
    if (!WaitFor("GNames", 300000, [] { return ue3::FindGNames(); })) return 0;
    if (!WaitFor("GObjects", 300000, [] { return ue3::FindGObjects(); })) return 0;
    // Natively registered classes appear first; wait for script packages (functions/properties) to load.
    if (!WaitFor("script packages", 300000, [] {
            ue3::InvalidateLookupCache();
            return ue3::FindObject("Function Engine.Actor.Touch") != 0 && ue3::FindObject("Class MMH7Game.H7Creature") != 0;
        }))
        return 0;
    Log("Script loaded: %d names, %d objects", ue3::NameCount(), ue3::ObjectCount());
    if (!ue3::Calibrate()) {
        ue3::DumpProbe(EnginePath(L"dumps"));
        Log("Engine layout calibration failed; mods disabled. See dumps\\probe.txt");
        return 0;
    }
    if (ConfigInt(L"Dev", L"DumpProbe", 0)) ue3::DumpProbe(EnginePath(L"dumps"));
    if (ConfigInt(L"Dev", L"DumpImage", 0)) ue3::DumpImage(EnginePath(L"dumps\\MMH7Game_unpacked.exe"));

    if (!WaitFor("GameViewportClient", 300000, FindViewportClient)) return 0;
    Sleep(2000); // let the remaining startup packages finish linking
    if (!ue3::CalibrateFunctionPointers()) {
        Log("Could not identify UObject::ProcessInternal; mods disabled");
        return 0;
    }
    ue3::InitFactory(); // optional: h7.new_object / h7.clone stay unavailable if this fails
    if (!InstallTickHook()) return 0;
    if (ConfigInt(L"Dev", L"DumpSDK", 0)) ue3::DumpNamesAndObjects(EnginePath(L"dumps"));
    Log("Init complete; Lua starts on the next frame");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
