#include "factory.h"
#include <windows.h>
#include <cstring>
#include "../core/log.h"
#include "../core/memory.h"
#include "reflection.h"

namespace h7::ue3 {

namespace {

// RVAs in MMH7Game-Win64-Shipping.exe v2.2.1 (found by disassembling UObject::execNew, GNatives[0x11]).
constexpr uintptr_t kStaticConstructObject = 0x26e13b0;
constexpr uintptr_t kFNameCtor = 0x26cb5c0;
constexpr uintptr_t kGetTransientPackage = 0x26bea20;

// First bytes of each function (from the unpacked image) to make sure we call what we think we call.
const uint8_t kSigSCO[] = {0x48, 0x89, 0xe0, 0x4c, 0x89, 0x40, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x70};
const uint8_t kSigFName[] = {0x53, 0x48, 0x83, 0xec, 0x30, 0x45, 0x89, 0xc1, 0x45, 0x31, 0xc0, 0x48, 0x89, 0xcb};
const uint8_t kSigTransient[] = {0x48, 0x8b, 0x05};

// UObject* StaticConstructObject(UClass*, UObject* Outer, FName, EObjectFlags, UObject* Template,
//                                FOutputDevice* Error, UObject* SubobjectRoot, FObjectInstancingGraph*)
using SCOFn = UObject(__fastcall*)(UObject, UObject, FName, uint64_t, UObject, void*, UObject, void*);
// FName::FName(const TCHAR* Name, EFindName FindType, UBOOL)
using FNameFn = FName*(__fastcall*)(FName*, const wchar_t*, int, int);
using TransientFn = UObject(__fastcall*)();

bool g_ok = false;
SCOFn g_sco = nullptr;
FNameFn g_fname = nullptr;
TransientFn g_transient = nullptr;

bool Matches(uintptr_t at, const uint8_t* sig, size_t n) {
    uint8_t buf[32];
    return n <= sizeof(buf) && mem::Read(reinterpret_cast<const void*>(at), buf, n) && memcmp(buf, sig, n) == 0;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// SEH wrappers: no C++ objects with destructors in these frames.
bool SafeName(const wchar_t* s, FName* out) {
    __try {
        g_fname(out, s, 1 /*FNAME_Add*/, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeConstruct(UObject cls, UObject outer, FName name, uint64_t flags, UObject tmpl, UObject* out) {
    __try {
        // Same as execNew: with a template the subobject root is INVALID_OBJECT (components keep pointing at the
        // template's), without one the engine instances them for the new object.
        UObject root = tmpl ? static_cast<UObject>(-1) : 0;
        *out = g_sco(cls, outer, name, flags, tmpl, nullptr, root, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool InitFactory() {
    uintptr_t base = mem::g_game.base;
    bool sco = Matches(base + kStaticConstructObject, kSigSCO, sizeof(kSigSCO));
    bool fname = Matches(base + kFNameCtor, kSigFName, sizeof(kSigFName));
    bool trans = Matches(base + kGetTransientPackage, kSigTransient, sizeof(kSigTransient));
    g_ok = sco && fname && trans;
    if (!g_ok) {
        Log("Object factory disabled: unexpected code at StaticConstructObject=%d FName=%d GetTransientPackage=%d",
            sco, fname, trans);
        return false;
    }
    g_sco = reinterpret_cast<SCOFn>(base + kStaticConstructObject);
    g_fname = reinterpret_cast<FNameFn>(base + kFNameCtor);
    g_transient = reinterpret_cast<TransientFn>(base + kGetTransientPackage);
    Log("Object factory ready (StaticConstructObject at game+%llx)", (unsigned long long)kStaticConstructObject);
    return true;
}

bool FactoryAvailable() { return g_ok; }

FName MakeName(const std::string& name) {
    FName out{-1, 0};
    if (!g_ok) return out;
    std::wstring w = Widen(name);
    if (!SafeName(w.c_str(), &out)) out = {-1, 0};
    return out;
}

UObject TransientPackage() {
    if (!g_ok) return 0;
    __try {
        return g_transient();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

UObject ConstructObject(UObject cls, UObject outer, const std::string& name, uint64_t flags, UObject tmpl,
                        std::string& error) {
    if (!g_ok) return error = "object creation is not available (factory disabled, see engine.log)", 0;
    if (!cls || ObjName(ObjClass(cls)) != "Class") return error = "not a class", 0;
    if (tmpl && !IsA(tmpl, cls)) return error = "template " + ObjFullName(tmpl) + " is not a " + ObjName(cls), 0;
    if (!outer) outer = TransientPackage();
    if (!outer) return error = "no outer (transient package not found)", 0;
    FName fn{0, 0}; // NAME_None: the engine picks a unique name
    if (!name.empty()) {
        fn = MakeName(name);
        if (fn.index < 0) return error = "could not create name '" + name + "'", 0;
    }
    UObject result = 0;
    if (!SafeConstruct(cls, outer, fn, flags, tmpl, &result)) return error = "StaticConstructObject crashed", 0;
    if (!result) return error = "StaticConstructObject returned null", 0;
    if (flags & RF_RootSet) { // AddToRoot: the constructor may not keep it; garbage collection must never free it
        uint64_t f = mem::ReadOr<uint64_t>(result + L.objFlags, 0) | RF_RootSet;
        mem::Write(reinterpret_cast<void*>(result + L.objFlags), &f, sizeof(f));
    }
    InvalidateLookupCache();
    return result;
}

UObject ConstructTopLevel(UObject cls, const std::string& name, uint64_t flags, std::string& error) {
    if (!g_ok) return error = "object creation is not available (factory disabled, see engine.log)", 0;
    FName fn = MakeName(name);
    if (fn.index < 0) return error = "could not create name '" + name + "'", 0;
    UObject result = 0;
    if (!SafeConstruct(cls, 0, fn, flags, 0, &result)) return error = "StaticConstructObject crashed", 0;
    if (!result) return error = "StaticConstructObject returned null", 0;
    InvalidateLookupCache();
    return result;
}

} // namespace h7::ue3
