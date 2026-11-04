#include "reflection.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <windows.h>
#include <map>
#include <unordered_map>

namespace h7::ue3 {

static bool g_calibrated = false;
static int g_classDefaultObject = -1;
static int g_enumNames = -1;

bool IsCalibrated() { return g_calibrated; }

template <class T>
static T Rd(uintptr_t a, int off, T fallback = T{}) {
    return off < 0 ? fallback : mem::ReadOr<T>(a + off, fallback);
}

UObject StructSuper(UObject s) { return Rd<uintptr_t>(s, L.structSuper); }
UObject StructChildren(UObject s) { return Rd<uintptr_t>(s, L.structChildren); }
UObject FieldNext(UObject f) { return Rd<uintptr_t>(f, L.fieldNext); }
int32_t StructSize(UObject s) { return Rd<int32_t>(s, L.structPropertySize); }

uint32_t FuncFlags(UObject fn) { return Rd<uint32_t>(fn, L.funcFlags); }
uint16_t FuncNativeIndex(UObject fn) { return Rd<uint16_t>(fn, L.funcFlags >= 0 ? L.funcFlags + 4 : -1); }
uint16_t FuncParmsSize(UObject fn) { return Rd<uint16_t>(fn, L.funcParmsSize); }
uint16_t FuncReturnOffset(UObject fn) { return Rd<uint16_t>(fn, L.funcReturnOffset); }
uintptr_t* FuncNativePtr(UObject fn) { return L.funcNative < 0 ? nullptr : reinterpret_cast<uintptr_t*>(fn + L.funcNative); }

UObject ClassDefaultObject(UObject cls) { return Rd<uintptr_t>(cls, g_classDefaultObject); }

void* EngineRealloc(void* p, size_t n) {
    using Fn = void*(__cdecl*)(void*, size_t);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"tbbmalloc.dll"), "scalable_realloc"));
    return fn ? fn(p, n) : nullptr;
}

static std::unordered_map<UObject, PropKind> g_kindCache;

PropKind KindOf(UObject prop) {
    UObject cls = ObjClass(prop);
    auto it = g_kindCache.find(cls);
    if (it != g_kindCache.end()) return it->second;
    static const std::map<std::string, PropKind> kinds = {
        {"ByteProperty", PropKind::Byte},         {"IntProperty", PropKind::Int},
        {"BoolProperty", PropKind::Bool},         {"FloatProperty", PropKind::Float},
        {"ObjectProperty", PropKind::Object},     {"ClassProperty", PropKind::Class},
        {"NameProperty", PropKind::Name},         {"StrProperty", PropKind::Str},
        {"StructProperty", PropKind::Struct},     {"ArrayProperty", PropKind::Array},
        {"DelegateProperty", PropKind::Delegate}, {"InterfaceProperty", PropKind::Interface},
        {"MapProperty", PropKind::Map},           {"QWordProperty", PropKind::QWord},
        {"ComponentProperty", PropKind::Component},
        // Heroes VII's lazily loaded object reference (Core.DynLoadObjectProperty, e.g. H7Faction.mStartTown): an
        // ObjectProperty subclass whose value is the object pointer (none until the game's getter loads it).
        {"DynLoadObjectProperty", PropKind::Object},
    };
    auto k = kinds.find(ObjName(cls));
    PropKind kind = k == kinds.end() ? PropKind::Unknown : k->second;
    g_kindCache[cls] = kind;
    return kind;
}

static int g_propClassSize = -1; // sizeof(UProperty): start of subclass fields

UObject PropInner(UObject prop) { return Rd<uintptr_t>(prop, g_propClassSize); }
uint32_t BoolMask(UObject prop) { return Rd<uint32_t>(prop, L.boolBitMask); }

PropInfo Describe(UObject prop) {
    PropInfo p;
    p.prop = prop;
    p.kind = KindOf(prop);
    p.name = ObjName(prop);
    p.offset = Rd<int32_t>(prop, L.propOffset);
    p.arrayDim = Rd<int32_t>(prop, L.propArrayDim, 1);
    p.elementSize = Rd<int32_t>(prop, L.propElementSize);
    p.flags = Rd<uint64_t>(prop, L.propFlags);
    return p;
}

std::vector<UObject> Fields(UObject s, bool includeSupers) {
    std::vector<UObject> out;
    int guard = 0;
    for (UObject cur = s; cur && guard < 256; cur = includeSupers ? StructSuper(cur) : 0, ++guard) {
        int n = 0;
        for (UObject f = StructChildren(cur); f && n < 20000; f = FieldNext(f), ++n) out.push_back(f);
    }
    return out;
}

UObject FindField(UObject s, const std::string& name) {
    int32_t idx = FindNameIndex(name);
    if (idx < 0) return 0;
    int guard = 0;
    for (UObject cur = s; cur && guard < 256; cur = StructSuper(cur), ++guard) {
        int n = 0;
        for (UObject f = StructChildren(cur); f && n < 20000; f = FieldNext(f), ++n)
            if (ObjFName(f).index == idx) return f;
    }
    return 0;
}

std::vector<PropInfo> FuncParams(UObject fn) {
    std::vector<PropInfo> out;
    for (UObject f : Fields(fn, false)) {
        if (KindOf(f) == PropKind::Unknown) continue;
        PropInfo p = Describe(f);
        if (p.flags & CPF_Parm) out.push_back(p);
    }
    return out;
}

std::vector<std::string> EnumNames(UObject e) {
    std::vector<std::string> out;
    TArrayRaw a{};
    if (g_enumNames < 0 || !mem::Read(e + g_enumNames, a) || a.num < 0 || a.num > 4096) return out;
    for (int i = 0; i < a.num; ++i) out.push_back(NameToString(mem::ReadOr<FName>(a.data + i * 8, FName{0, 0})));
    return out;
}

std::string ReadFString(uintptr_t at) {
    TArrayRaw a{};
    if (!mem::Read(at, a) || !a.data || a.num <= 0 || a.num > 1 << 20) return {};
    std::wstring w(a.num, L'\0');
    if (!mem::Read((void*)a.data, w.data(), a.num * sizeof(wchar_t))) return {};
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    std::string s;
    // UTF-16 -> UTF-8
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
        else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

#define REQUIRE(cond, what)                                   \
    if (!(cond)) {                                            \
        Log("Calibration failed: %s", what);                  \
        return false;                                         \
    }

template <class T, class Pred>
static int FindOffset(int from, int to, int step, Pred pred) {
    for (int off = from; off + (int)sizeof(T) <= to; off += step)
        if (pred(off)) return off;
    return -1;
}

static UObject ChildNamed(UObject s, const char* name) {
    int32_t idx = FindNameIndex(name);
    for (UObject f = StructChildren(s); f; f = FieldNext(f))
        if (ObjFName(f).index == idx) return f;
    return 0;
}

bool Calibrate() {
    if (g_calibrated) return true;
    UObject cObject = FindObject("Class Core.Object");
    UObject cField = FindObject("Class Core.Field");
    UObject cStruct = FindObject("Class Core.Struct");
    UObject cProperty = FindObject("Class Core.Property");
    UObject cActor = FindObject("Class Engine.Actor");
    REQUIRE(cObject && cField && cStruct && cProperty && cActor, "core classes not found");

    // UStruct::SuperField — Field's super is Object.
    L.structSuper = FindOffset<uintptr_t>(L.objSize, 0xd0, 8, [&](int o) {
        return Rd<uintptr_t>(cField, o) == cObject && Rd<uintptr_t>(cActor, o) == cObject;
    });
    REQUIRE(L.structSuper >= 0, "UStruct::SuperField");

    // UStruct::PropertySize — sizeof(UObject) and sizeof(UField).
    L.structPropertySize = FindOffset<int32_t>(L.objSize, 0xd0, 4, [&](int o) {
        return Rd<int32_t>(cObject, o) == L.objSize && Rd<int32_t>(cField, o) == L.objSize + 8;
    });
    REQUIRE(L.structPropertySize >= 0, "UStruct::PropertySize");
    g_propClassSize = StructSize(cProperty);

    // UStruct::Children — first field whose Outer is the struct itself.
    L.structChildren = FindOffset<uintptr_t>(L.objSize, 0xd0, 8, [&](int o) {
        if (o == L.structSuper) return false;
        UObject a = Rd<uintptr_t>(cActor, o), b = Rd<uintptr_t>(cObject, o);
        return IsValidObject(a) && ObjOuter(a) == cActor && IsValidObject(b) && ObjOuter(b) == cObject;
    });
    REQUIRE(L.structChildren >= 0, "UStruct::Children");

    // UProperty: Object.Outer/Name/Class live at 0x40/0x48/0x50 and are 8 bytes each.
    UObject pOuter = ChildNamed(cObject, "Outer"), pName = ChildNamed(cObject, "Name"), pClass = ChildNamed(cObject, "Class");
    REQUIRE(pOuter && pName && pClass, "Object.Outer/Name/Class properties");
    int lo = L.fieldNext + 8, hi = g_propClassSize;
    L.propOffset = FindOffset<int32_t>(lo, hi, 4, [&](int o) {
        return Rd<int32_t>(pOuter, o) == L.objOuter && Rd<int32_t>(pName, o) == L.objName && Rd<int32_t>(pClass, o) == L.objClass;
    });
    L.propArrayDim = FindOffset<int32_t>(lo, hi, 4, [&](int o) {
        return Rd<int32_t>(pOuter, o) == 1 && Rd<int32_t>(pName, o) == 1 && Rd<int32_t>(pClass, o) == 1;
    });
    L.propElementSize = FindOffset<int32_t>(lo, hi, 4, [&](int o) {
        return o != L.propOffset && Rd<int32_t>(pOuter, o) == 8 && Rd<int32_t>(pName, o) == 8 && Rd<int32_t>(pClass, o) == 8;
    });
    REQUIRE(L.propOffset >= 0 && L.propArrayDim >= 0 && L.propElementSize >= 0, "UProperty Offset/ArrayDim/ElementSize");

    // Functions with known signatures from Actor.uc:
    //   event Touch(Actor Other, PrimitiveComponent OtherComp, vector HitLocation, vector HitNormal)  -> 4 parms, 0x28 bytes
    //   native(267) final function bool SetLocation(vector NewLocation)                                -> 2 parms, 0x10 bytes, ret @0xc
    UObject fTouch = ChildNamed(cActor, "Touch"), fSetLoc = ChildNamed(cActor, "SetLocation");
    REQUIRE(fTouch && fSetLoc, "Actor.Touch / Actor.SetLocation");

    // PropertyFlags: every Touch child is a parm (CPF_Parm), Object.Outer is not.
    L.propFlags = FindOffset<uint64_t>(lo, hi, 8, [&](int o) {
        if (Rd<uint64_t>(pOuter, o) & CPF_Parm) return false;
        int n = 0;
        for (UObject f = StructChildren(fTouch); f; f = FieldNext(f), ++n)
            if (!(Rd<uint64_t>(f, o) & CPF_Parm)) return false;
        return n == 4;
    });
    REQUIRE(L.propFlags >= 0, "UProperty::PropertyFlags");

    int fStart = StructSize(cStruct), fEnd = StructSize(FindObject("Class Core.Function"));
    L.funcFlags = FindOffset<uint32_t>(fStart, fEnd, 4, [&](int o) {
        uint32_t t = Rd<uint32_t>(fTouch, o), s = Rd<uint32_t>(fSetLoc, o);
        return (t & FUNC_Event) && !(t & FUNC_Native) && (s & (FUNC_Native | FUNC_Final)) == (FUNC_Native | FUNC_Final);
    });
    L.funcParmsSize = FindOffset<uint16_t>(fStart, fEnd, 2, [&](int o) {
        return Rd<uint16_t>(fTouch, o) == 0x28 && Rd<uint16_t>(fSetLoc, o) == 0x10;
    });
    L.funcReturnOffset = FindOffset<uint16_t>(fStart, fEnd, 2, [&](int o) {
        return o != L.funcParmsSize && Rd<uint16_t>(fSetLoc, o) == 0xc;
    });
    REQUIRE(L.funcFlags >= 0 && L.funcParmsSize >= 0 && L.funcReturnOffset >= 0, "UFunction flags/parms");
    if (Rd<uint16_t>(fSetLoc, L.funcFlags + 4) != 267) Log("warning: UFunction::iNative not at FunctionFlags+4");

    // UFunction::Func is the last member (8-byte pointer-to-member). It is bound lazily while packages
    // link, so its value is checked later in CalibrateFunctionPointers().
    L.funcNative = fEnd - 8;
    UObject fSin = FindObject("Function Core.Object.Sin");
    REQUIRE(fSin && mem::g_game.Contains(Rd<uintptr_t>(fSin, L.funcNative)), "UFunction::Func (Object.Sin not bound)");

    L.funcSize = fEnd;

    // UStruct::Script — bytecode TArray<BYTE>. Touch has an empty body: EX_Return EX_Nothing EX_EndOfScript.
    L.structScript = FindOffset<uintptr_t>(L.structPropertySize + 4, fStart, 4, [&](int o) {
        TArrayRaw a{};
        return mem::Read(fTouch + o, a) && a.num == 3 && mem::ReadOr<uint32_t>(a.data, 0) << 8 == 0x530b0400;
    });
    if (L.structScript < 0) Log("warning: UStruct::Script not found; script-to-script calls can't be hooked");

    // Subclass fields start right after UProperty.
    L.boolBitMask = L.objPropClass = L.structPropStruct = L.arrayPropInner = L.bytePropEnum = g_propClassSize;
    UObject bHidden = ChildNamed(cActor, "bHidden");
    uint32_t mask = bHidden ? BoolMask(bHidden) : 0;
    if (!mask || (mask & (mask - 1))) Log("warning: BoolProperty bitmask looks wrong (%08x)", mask);

    // UClass::ClassDefaultObject — pointer to Default__<ClassName>. Engine structs are packed to 4 bytes,
    // so pointers are not necessarily 8-aligned.
    UObject cdoActor = FindObject("Actor Engine.Default__Actor");
    g_classDefaultObject = FindOffset<uintptr_t>(fEnd, StructSize(FindObject("Class Core.Class")), 4,
                                                 [&](int o) { return cdoActor && Rd<uintptr_t>(cActor, o) == cdoActor; });
    if (g_classDefaultObject < 0) Log("warning: UClass::ClassDefaultObject not found");

    // UEnum::Names — TArray<FName> right after UField.
    if (UObject ePhys = FindObject("Enum Engine.Actor.EPhysics")) {
        for (int o = L.fieldNext + 8; o < 0x90 && g_enumNames < 0; o += 8) {
            TArrayRaw a{};
            if (mem::Read(ePhys + o, a) && a.num > 0 && a.num < 64 &&
                NameToString(mem::ReadOr<FName>(a.data, FName{0, 0})) == "PHYS_None")
                g_enumNames = o;
        }
    }

    Log("Calibrated: Struct{Super=%x Children=%x Size=%x} Prop{ArrayDim=%x ElemSize=%x Flags=%x Offset=%x sub=%x} "
        "Func{Flags=%x Parms=%x Ret=%x Func=%x Script=%x} Class{CDO=%x} Enum{Names=%x}",
        L.structSuper, L.structChildren, L.structPropertySize, L.propArrayDim, L.propElementSize, L.propFlags,
        L.propOffset, g_propClassSize, L.funcFlags, L.funcParmsSize, L.funcReturnOffset, L.funcNative,
        L.structScript, g_classDefaultObject, g_enumNames);
    g_calibrated = true;
    return true;
}

static uintptr_t g_processInternal = 0;
uintptr_t ProcessInternal() { return g_processInternal; }

bool CalibrateFunctionPointers() {
    // Script (non-native) functions all bind Func to UObject::ProcessInternal: take the most common value.
    std::map<uintptr_t, int> hist;
    int script = 0, unbound = 0;
    UObject cFunction = FindObject("Class Core.Function");
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject o = ObjectAt(i);
        if (!o || ObjClass(o) != cFunction || (FuncFlags(o) & FUNC_Native)) continue;
        ++script;
        uintptr_t v = Rd<uintptr_t>(o, L.funcNative);
        if (mem::g_game.Contains(v)) hist[v]++;
        else ++unbound;
    }
    int best = 0;
    for (auto& [v, c] : hist)
        if (c > best) best = c, g_processInternal = v;
    Log("Script functions: %d, bound to ProcessInternal(game+%llx): %d, unbound: %d, other: %d", script,
        (unsigned long long)(g_processInternal - mem::g_game.base), best, unbound, script - best - unbound);
    return g_processInternal != 0;
}

} // namespace h7::ue3
