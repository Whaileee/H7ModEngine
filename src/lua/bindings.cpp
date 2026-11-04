// Lua <-> UnrealScript bindings: object/struct/array proxies, value marshaling, function calls, hooks.
#include "runtime.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../ue3/core.h"
#include "../ue3/factory.h"
#include "../ue3/hooks.h"
#include "../ue3/reflection.h"
#include "../engine/paths.h"
#include <windows.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

namespace h7::lua {

using ue3::PropInfo;
using ue3::PropKind;
using ue3::UObject;

static const char* kObj = "H7Object";
static const char* kStruct = "H7Struct";
static const char* kArray = "H7Array";

struct ObjRef { UObject obj; };
struct StructRef { uintptr_t addr; UObject type; uint64_t frame; uintptr_t outParms = 0; }; // outParms: hook args
struct ArrayRef { uintptr_t arr; UObject inner; uint64_t frame; };

// Proxies into hook parameter storage are only valid while the hook runs.
static std::unordered_set<uint64_t> g_liveFrames;
static uint64_t g_nextFrame = 1;
static bool FrameAlive(uint64_t f) { return f == 0 || g_liveFrames.count(f) != 0; }

using ue3::EngineRealloc;

static std::wstring ToWide(const char* s, size_t len) { return Widen(std::string(s, len)); }

// Writes a UTF-8 string into an FString (TArray<TCHAR>) using the engine allocator.
static bool AssignFString(uintptr_t at, const char* s, size_t len) {
    std::wstring w = ToWide(s, len);
    ue3::TArrayRaw a{};
    if (!mem::Read(at, a)) return false;
    int32_t num = (int32_t)w.size() + 1;
    void* data = EngineRealloc(reinterpret_cast<void*>(a.data), num * sizeof(wchar_t));
    if (!data) return false;
    memcpy(data, w.c_str(), num * sizeof(wchar_t));
    a = {reinterpret_cast<uintptr_t>(data), num, num};
    return mem::Write(reinterpret_cast<void*>(at), &a, sizeof(a));
}

// ---------------------------------------------------------------------------------------
// Field lookup cache: (struct, name) -> field
// ---------------------------------------------------------------------------------------

static std::unordered_map<UObject, std::unordered_map<std::string, UObject>> g_fieldCache;

static UObject LookupField(UObject s, const char* name) {
    auto& m = g_fieldCache[s];
    auto it = m.find(name);
    if (it != m.end()) return it->second;
    UObject f = ue3::FindField(s, name);
    m.emplace(name, f);
    return f;
}

static bool IsFunction(UObject field) { return ue3::ObjName(ue3::ObjClass(field)) == "Function"; }

// ---------------------------------------------------------------------------------------
// Push / write values
// ---------------------------------------------------------------------------------------

void PushObject(lua_State* L, UObject o) {
    if (!o) {
        lua_pushnil(L);
        return;
    }
    auto* r = static_cast<ObjRef*>(lua_newuserdatauv(L, sizeof(ObjRef), 0));
    r->obj = o;
    luaL_setmetatable(L, kObj);
}

static void PushStruct(lua_State* L, uintptr_t addr, UObject type, uint64_t frame) {
    auto* r = static_cast<StructRef*>(lua_newuserdatauv(L, sizeof(StructRef), 0));
    *r = {addr, type, frame};
    luaL_setmetatable(L, kStruct);
}

static void PushArray(lua_State* L, uintptr_t arr, UObject inner, uint64_t frame) {
    auto* r = static_cast<ArrayRef*>(lua_newuserdatauv(L, sizeof(ArrayRef), 0));
    *r = {arr, inner, frame};
    luaL_setmetatable(L, kArray);
}

static UObject ToObject(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    auto* r = static_cast<ObjRef*>(luaL_testudata(L, idx, kObj));
    if (!r) luaL_error(L, "expected an engine object, got %s", luaL_typename(L, idx));
    return r->obj;
}

static UObject CheckValidObject(lua_State* L, int idx) {
    auto* r = static_cast<ObjRef*>(luaL_checkudata(L, idx, kObj));
    if (!ue3::IsValidObject(r->obj)) luaL_error(L, "engine object %p is no longer valid", (void*)r->obj);
    return r->obj;
}

static void PushElem(lua_State* L, uintptr_t addr, UObject prop, PropKind kind, uint64_t frame) {
    switch (kind) {
    case PropKind::Byte: lua_pushinteger(L, mem::ReadOr<uint8_t>(addr, 0)); break;
    case PropKind::Int: lua_pushinteger(L, mem::ReadOr<int32_t>(addr, 0)); break;
    case PropKind::QWord: lua_pushinteger(L, mem::ReadOr<int64_t>(addr, 0)); break;
    case PropKind::Float: lua_pushnumber(L, mem::ReadOr<float>(addr, 0)); break;
    case PropKind::Bool: lua_pushboolean(L, (mem::ReadOr<uint32_t>(addr, 0) & ue3::BoolMask(prop)) != 0); break;
    case PropKind::Object:
    case PropKind::Class:
    case PropKind::Component:
    case PropKind::Interface: PushObject(L, mem::ReadOr<uintptr_t>(addr, 0)); break;
    case PropKind::Name: lua_pushstring(L, ue3::NameToString(mem::ReadOr<ue3::FName>(addr, {0, 0})).c_str()); break;
    case PropKind::Str: lua_pushstring(L, ue3::ReadFString(addr).c_str()); break;
    case PropKind::Struct: PushStruct(L, addr, ue3::PropInner(prop), frame); break;
    case PropKind::Array: PushArray(L, addr, ue3::PropInner(prop), frame); break;
    case PropKind::Delegate:
        lua_createtable(L, 0, 2);
        PushObject(L, mem::ReadOr<uintptr_t>(addr, 0));
        lua_setfield(L, -2, "object");
        lua_pushstring(L, ue3::NameToString(mem::ReadOr<ue3::FName>(addr + 8, {0, 0})).c_str());
        lua_setfield(L, -2, "func");
        break;
    default: lua_pushnil(L); break;
    }
}

static void PushProp(lua_State* L, uintptr_t base, UObject prop, uint64_t frame) {
    PropInfo p = ue3::Describe(prop);
    if (p.arrayDim > 1) {
        lua_createtable(L, p.arrayDim, 0);
        for (int i = 0; i < p.arrayDim; ++i) {
            PushElem(L, base + p.offset + i * p.elementSize, prop, p.kind, frame);
            lua_rawseti(L, -2, i + 1);
        }
        return;
    }
    PushElem(L, base + p.offset, prop, p.kind, frame);
}

static void WriteStructFromTable(lua_State* L, int idx, uintptr_t addr, UObject type);
static int H_to_table(lua_State* L);

// Frees an FString's or a dynamic array's buffer (and the strings in an array of strings) and empties it.
static void FreeOwned(uintptr_t addr, PropKind kind, UObject prop) {
    auto a = mem::ReadOr<ue3::TArrayRaw>(addr, {});
    if (!a.data) return;
    if (kind == PropKind::Array) {
        PropInfo ip = ue3::Describe(ue3::PropInner(prop));
        if (ip.kind == PropKind::Str)
            for (int32_t i = 0; i < a.num; ++i) FreeOwned(a.data + (uintptr_t)i * ip.elementSize, PropKind::Str, 0);
    } else if (kind != PropKind::Str) {
        return;
    }
    EngineRealloc(reinterpret_cast<void*>(a.data), 0);
    a = {};
    mem::Write(reinterpret_cast<void*>(addr), &a, sizeof(a));
}

static void WriteElem(lua_State* L, int idx, uintptr_t addr, UObject prop, PropKind kind) {
    idx = lua_absindex(L, idx);
    switch (kind) {
    case PropKind::Byte: { uint8_t v = (uint8_t)luaL_checkinteger(L, idx); mem::Write((void*)addr, &v, 1); break; }
    case PropKind::Int: { int32_t v = (int32_t)luaL_checkinteger(L, idx); mem::Write((void*)addr, &v, 4); break; }
    case PropKind::QWord: { int64_t v = luaL_checkinteger(L, idx); mem::Write((void*)addr, &v, 8); break; }
    case PropKind::Float: { float v = (float)luaL_checknumber(L, idx); mem::Write((void*)addr, &v, 4); break; }
    case PropKind::Bool: {
        uint32_t mask = ue3::BoolMask(prop), v = mem::ReadOr<uint32_t>(addr, 0);
        v = lua_toboolean(L, idx) ? (v | mask) : (v & ~mask);
        mem::Write((void*)addr, &v, 4);
        break;
    }
    case PropKind::Object:
    case PropKind::Class:
    case PropKind::Component:
    case PropKind::Interface: {
        UObject o = ToObject(L, idx);
        UObject want = ue3::PropInner(prop);
        if (o && want && kind == PropKind::Object && !ue3::IsA(o, want))
            luaL_error(L, "object %s is not a %s", ue3::ObjFullName(o).c_str(), ue3::ObjName(want).c_str());
        mem::Write((void*)addr, &o, 8);
        break;
    }
    case PropKind::Name: {
        const char* s = luaL_checkstring(L, idx);
        int32_t n = ue3::FindNameIndex(s);
        ue3::FName fn{n, 0};
        if (n < 0) fn = ue3::MakeName(s); // adds the name (needs the object factory)
        if (fn.index < 0) luaL_error(L, "name '%s' does not exist in the engine name table", s);
        mem::Write((void*)addr, &fn, 8);
        break;
    }
    case PropKind::Str: {
        size_t len;
        const char* s = luaL_checklstring(L, idx, &len);
        if (!AssignFString(addr, s, len)) luaL_error(L, "could not assign string");
        break;
    }
    case PropKind::Struct: {
        UObject type = ue3::PropInner(prop);
        if (auto* r = static_cast<StructRef*>(luaL_testudata(L, idx, kStruct))) {
            if (r->type != type) luaL_error(L, "struct type mismatch");
            std::vector<uint8_t> tmp(ue3::StructSize(type));
            mem::Read((void*)r->addr, tmp.data(), tmp.size());
            mem::Write((void*)addr, tmp.data(), tmp.size());
        } else {
            luaL_checktype(L, idx, LUA_TTABLE);
            WriteStructFromTable(L, idx, addr, type);
        }
        break;
    }
    case PropKind::Delegate: {
        // {object = obj, func = "FunctionName"} (or nil to clear): an FScriptDelegate {UObject*, FName}.
        struct { UObject object; ue3::FName func; } d{0, {0, 0}};
        if (!lua_isnil(L, idx)) {
            luaL_checktype(L, idx, LUA_TTABLE);
            lua_getfield(L, idx, "object");
            d.object = ToObject(L, -1);
            lua_getfield(L, idx, "func");
            const char* fn = luaL_checkstring(L, -1);
            if (!d.object || !LookupField(ue3::ObjClass(d.object), fn))
                luaL_error(L, "delegate target has no function '%s'", fn);
            d.func = {ue3::FindNameIndex(fn), 0};
            lua_pop(L, 2);
        }
        mem::Write((void*)addr, &d, sizeof(d));
        break;
    }
    case PropKind::Array: {
        // A Lua sequence (or another engine array) replaces the array's contents.
        UObject inner = ue3::PropInner(prop);
        PropInfo ip = ue3::Describe(inner);
        int src = idx;
        if (luaL_testudata(L, idx, kArray)) {
            lua_pushcfunction(L, H_to_table);
            lua_pushvalue(L, idx);
            lua_call(L, 1, 1);
            src = lua_gettop(L);
        } else {
            luaL_checktype(L, idx, LUA_TTABLE);
        }
        int32_t n = (int32_t)lua_rawlen(L, src);
        FreeOwned(addr, PropKind::Array, prop);
        ue3::TArrayRaw a{};
        if (n > 0) {
            void* data = EngineRealloc(nullptr, (size_t)n * ip.elementSize);
            if (!data) luaL_error(L, "allocation failed");
            memset(data, 0, (size_t)n * ip.elementSize);
            a = {reinterpret_cast<uintptr_t>(data), n, n};
        }
        mem::Write((void*)addr, &a, sizeof(a));
        for (int32_t i = 0; i < n; ++i) {
            lua_rawgeti(L, src, i + 1);
            if (!lua_isnil(L, -1)) WriteElem(L, -1, a.data + (uintptr_t)i * ip.elementSize, inner, ip.kind);
            lua_pop(L, 1);
        }
        if (src != idx) lua_pop(L, 1);
        break;
    }
    default: luaL_error(L, "assigning %s properties is not supported", ue3::ObjName(ue3::ObjClass(prop)).c_str());
    }
}

static void WriteProp(lua_State* L, int idx, uintptr_t base, UObject prop) {
    PropInfo p = ue3::Describe(prop);
    if (p.arrayDim > 1) {
        luaL_checktype(L, idx, LUA_TTABLE);
        for (int i = 0; i < p.arrayDim; ++i) {
            lua_rawgeti(L, idx, i + 1);
            if (!lua_isnil(L, -1)) WriteElem(L, -1, base + p.offset + i * p.elementSize, prop, p.kind);
            lua_pop(L, 1);
        }
        return;
    }
    WriteElem(L, idx, base + p.offset, prop, p.kind);
}

static void WriteStructFromTable(lua_State* L, int idx, uintptr_t addr, UObject type) {
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(L, -2);
            UObject f = LookupField(type, key);
            if (!f || IsFunction(f)) luaL_error(L, "struct %s has no field '%s'", ue3::ObjName(type).c_str(), key);
            WriteProp(L, -1, addr, f);
        }
        lua_pop(L, 1);
    }
}

// ---------------------------------------------------------------------------------------
// Calling UFunctions
// ---------------------------------------------------------------------------------------

static int CallUFunction(lua_State* L) {
    UObject fn = reinterpret_cast<UObject>(lua_touserdata(L, lua_upvalueindex(1)));
    UObject self = CheckValidObject(L, 1);
    if (!hooks::HasProcessEvent()) luaL_error(L, "function calls are not available yet (ProcessEvent not discovered)");
    std::vector<PropInfo> params = ue3::FuncParams(fn);
    std::vector<uint8_t> buf(ue3::FuncParmsSize(fn) + 64, 0);
    uintptr_t base = reinterpret_cast<uintptr_t>(buf.data());
    int arg = 2;
    for (auto& p : params) {
        if (p.flags & ue3::CPF_ReturnParm) continue;
        if (!lua_isnoneornil(L, arg)) WriteProp(L, arg, base, p.prop);
        ++arg;
    }
    hooks::ProcessEvent(self, fn, buf.data());
    int nret = 0;
    for (auto& p : params)
        if (p.flags & ue3::CPF_ReturnParm) PushProp(L, base, p.prop, 0), ++nret;
    // Out parameters follow the return value. Struct/array results are copied to tables since the buffer dies here.
    for (auto& p : params) {
        if (!(p.flags & ue3::CPF_OutParm) || (p.flags & ue3::CPF_ReturnParm)) continue;
        PushProp(L, base, p.prop, 0);
        ++nret;
    }
    // Struct/array return values point into `buf`; convert them to plain tables.
    for (int i = lua_gettop(L) - nret + 1; i <= lua_gettop(L); ++i) {
        if (luaL_testudata(L, i, kStruct) || luaL_testudata(L, i, kArray)) {
            lua_getglobal(L, "h7");
            lua_getfield(L, -1, "to_table");
            lua_pushvalue(L, i);
            lua_call(L, 1, 1);
            lua_replace(L, i);
            lua_pop(L, 1);
        }
    }
    // The caller owns the strings and arrays in the parameter buffer (ProcessEvent copies parameters back).
    for (auto& p : params)
        for (int i = 0; i < p.arrayDim; ++i) FreeOwned(base + p.offset + i * p.elementSize, p.kind, p.prop);
    return nret;
}

// ---------------------------------------------------------------------------------------
// H7Object metatable
// ---------------------------------------------------------------------------------------

static int Obj_index(lua_State* L) {
    auto* r = static_cast<ObjRef*>(luaL_checkudata(L, 1, kObj));
    const char* key = luaL_checkstring(L, 2);
    // Helper methods (snake_case, so they never collide with UnrealScript members).
    lua_getfield(L, LUA_REGISTRYINDEX, "H7ObjectMethods");
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);
    if (!ue3::IsValidObject(r->obj)) luaL_error(L, "engine object %p is no longer valid", (void*)r->obj);
    UObject f = LookupField(ue3::ObjClass(r->obj), key);
    if (!f) return 0;
    if (IsFunction(f)) {
        lua_pushlightuserdata(L, reinterpret_cast<void*>(f));
        lua_pushcclosure(L, CallUFunction, 1);
        return 1;
    }
    PushProp(L, r->obj, f, 0);
    return 1;
}

static int Obj_newindex(lua_State* L) {
    UObject o = CheckValidObject(L, 1);
    const char* key = luaL_checkstring(L, 2);
    UObject f = LookupField(ue3::ObjClass(o), key);
    if (!f || IsFunction(f)) luaL_error(L, "%s has no property '%s'", ue3::ObjName(ue3::ObjClass(o)).c_str(), key);
    WriteProp(L, 3, o, f);
    return 0;
}

static int Obj_eq(lua_State* L) {
    auto* a = static_cast<ObjRef*>(luaL_testudata(L, 1, kObj));
    auto* b = static_cast<ObjRef*>(luaL_testudata(L, 2, kObj));
    lua_pushboolean(L, a && b && a->obj == b->obj);
    return 1;
}

static int Obj_tostring(lua_State* L) {
    auto* r = static_cast<ObjRef*>(luaL_checkudata(L, 1, kObj));
    std::string s = ue3::IsValidObject(r->obj) ? ue3::ObjFullName(r->obj) : "<invalid object>";
    lua_pushstring(L, s.c_str());
    return 1;
}

static int M_get_name(lua_State* L) { lua_pushstring(L, ue3::ObjName(CheckValidObject(L, 1)).c_str()); return 1; }
static int M_get_full_name(lua_State* L) { lua_pushstring(L, ue3::ObjFullName(CheckValidObject(L, 1)).c_str()); return 1; }
static int M_get_path_name(lua_State* L) { lua_pushstring(L, ue3::ObjPathName(CheckValidObject(L, 1)).c_str()); return 1; }
static int M_get_class(lua_State* L) { PushObject(L, ue3::ObjClass(CheckValidObject(L, 1))); return 1; }
static int M_get_outer(lua_State* L) { PushObject(L, ue3::ObjOuter(CheckValidObject(L, 1))); return 1; }
static int M_get_address(lua_State* L) { lua_pushinteger(L, (lua_Integer)CheckValidObject(L, 1)); return 1; }
static int M_get_default(lua_State* L) { PushObject(L, ue3::ClassDefaultObject(CheckValidObject(L, 1))); return 1; }
static int M_get_super(lua_State* L) { PushObject(L, ue3::StructSuper(CheckValidObject(L, 1))); return 1; }

static int M_is_valid(lua_State* L) {
    auto* r = static_cast<ObjRef*>(luaL_checkudata(L, 1, kObj));
    lua_pushboolean(L, ue3::IsValidObject(r->obj));
    return 1;
}

static UObject ClassArg(lua_State* L, int idx) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        UObject c = ue3::FindClass(lua_tostring(L, idx));
        if (!c) luaL_error(L, "unknown class '%s'", lua_tostring(L, idx));
        return c;
    }
    return CheckValidObject(L, idx);
}

static int M_is_a(lua_State* L) {
    UObject o = CheckValidObject(L, 1);
    lua_pushboolean(L, ue3::IsA(o, ClassArg(L, 2)));
    return 1;
}

// obj:call("FuncName", ...) — explicit form, useful when a property shadows a function name.
static int M_call(lua_State* L) {
    UObject o = CheckValidObject(L, 1);
    const char* name = luaL_checkstring(L, 2);
    UObject f = LookupField(ue3::ObjClass(o), name);
    if (!f || !IsFunction(f)) luaL_error(L, "%s has no function '%s'", ue3::ObjName(ue3::ObjClass(o)).c_str(), name);
    lua_remove(L, 2);
    lua_pushlightuserdata(L, reinterpret_cast<void*>(f));
    lua_pushcclosure(L, CallUFunction, 1);
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// ---------------------------------------------------------------------------------------
// H7Struct / H7Array metatables
// ---------------------------------------------------------------------------------------

static StructRef* CheckStruct(lua_State* L, int idx) {
    auto* r = static_cast<StructRef*>(luaL_checkudata(L, idx, kStruct));
    if (!FrameAlive(r->frame)) luaL_error(L, "this value referred to hook parameters and is no longer valid");
    return r;
}

// Where field f of r is stored. For hook args, out parameters of script-to-script calls live at the caller.
static uintptr_t FieldBase(const StructRef* r, UObject f) {
    if (!r->outParms) return r->addr;
    PropInfo p = ue3::Describe(f);
    if (!(p.flags & ue3::CPF_OutParm)) return r->addr;
    hooks::CallContext c{};
    c.outParms = reinterpret_cast<uint8_t*>(r->outParms);
    uint8_t* at = hooks::OutParmAddr(c, f);
    return at ? reinterpret_cast<uintptr_t>(at) - p.offset : r->addr;
}

static int Struct_index(lua_State* L) {
    auto* r = CheckStruct(L, 1);
    const char* key = luaL_checkstring(L, 2);
    UObject f = LookupField(r->type, key);
    if (!f || IsFunction(f)) return 0;
    PushProp(L, FieldBase(r, f), f, r->frame);
    return 1;
}

static int Struct_newindex(lua_State* L) {
    auto* r = CheckStruct(L, 1);
    const char* key = luaL_checkstring(L, 2);
    UObject f = LookupField(r->type, key);
    if (!f || IsFunction(f)) luaL_error(L, "%s has no field '%s'", ue3::ObjName(r->type).c_str(), key);
    WriteProp(L, 3, FieldBase(r, f), f);
    return 0;
}

static int Struct_tostring(lua_State* L) {
    auto* r = static_cast<StructRef*>(luaL_checkudata(L, 1, kStruct));
    lua_pushfstring(L, "%s: %p", ue3::ObjName(r->type).c_str(), (void*)r->addr);
    return 1;
}

static ArrayRef* CheckArray(lua_State* L, int idx) {
    auto* r = static_cast<ArrayRef*>(luaL_checkudata(L, idx, kArray));
    if (!FrameAlive(r->frame)) luaL_error(L, "this value referred to hook parameters and is no longer valid");
    return r;
}

static int Array_len(lua_State* L) {
    auto* r = CheckArray(L, 1);
    lua_pushinteger(L, mem::ReadOr<ue3::TArrayRaw>(r->arr, {}).num);
    return 1;
}

static int Array_add(lua_State* L);
static int Array_remove(lua_State* L);

static int Array_index(lua_State* L) {
    auto* r = CheckArray(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* k = lua_tostring(L, 2);
        if (!strcmp(k, "add")) return lua_pushcfunction(L, Array_add), 1;
        if (!strcmp(k, "remove")) return lua_pushcfunction(L, Array_remove), 1;
        return 0;
    }
    lua_Integer i = luaL_checkinteger(L, 2);
    auto a = mem::ReadOr<ue3::TArrayRaw>(r->arr, {});
    if (i < 1 || i > a.num) return 0;
    PropInfo p = ue3::Describe(r->inner);
    PushElem(L, a.data + (i - 1) * p.elementSize, r->inner, p.kind, r->frame);
    return 1;
}

static int Array_newindex(lua_State* L) {
    auto* r = CheckArray(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    auto a = mem::ReadOr<ue3::TArrayRaw>(r->arr, {});
    if (i < 1 || i > a.num) luaL_error(L, "array index %d out of range (1..%d); use :add()", (int)i, a.num);
    PropInfo p = ue3::Describe(r->inner);
    WriteElem(L, 3, a.data + (i - 1) * p.elementSize, r->inner, p.kind);
    return 0;
}

// arr:add(value) — append using the engine allocator.
static int Array_add(lua_State* L) {
    auto* r = CheckArray(L, 1);
    PropInfo p = ue3::Describe(r->inner);
    auto a = mem::ReadOr<ue3::TArrayRaw>(r->arr, {});
    if (a.num + 1 > a.max) {
        int32_t newMax = a.max < 4 ? 4 : a.max * 2;
        void* data = EngineRealloc(reinterpret_cast<void*>(a.data), (size_t)newMax * p.elementSize);
        if (!data) luaL_error(L, "allocation failed");
        a.data = reinterpret_cast<uintptr_t>(data);
        a.max = newMax;
    }
    uintptr_t elem = a.data + (uintptr_t)a.num * p.elementSize;
    memset(reinterpret_cast<void*>(elem), 0, p.elementSize);
    ++a.num;
    mem::Write(reinterpret_cast<void*>(r->arr), &a, sizeof(a));
    if (!lua_isnoneornil(L, 2)) WriteElem(L, 2, elem, r->inner, p.kind);
    lua_pushinteger(L, a.num);
    return 1;
}

// arr:remove(i) — removes element i (1-based), shifting the rest down. Strings in the element are freed.
static int Array_remove(lua_State* L) {
    auto* r = CheckArray(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    PropInfo p = ue3::Describe(r->inner);
    auto a = mem::ReadOr<ue3::TArrayRaw>(r->arr, {});
    if (i < 1 || i > a.num) luaL_error(L, "array index out of range");
    uintptr_t elem = a.data + (uintptr_t)(i - 1) * p.elementSize;
    if (p.kind == PropKind::Str) EngineRealloc(reinterpret_cast<void*>(mem::ReadOr<uintptr_t>(elem, 0)), 0);
    memmove(reinterpret_cast<void*>(elem), reinterpret_cast<void*>(elem + p.elementSize), (size_t)(a.num - i) * p.elementSize);
    --a.num;
    mem::Write(reinterpret_cast<void*>(r->arr), &a, sizeof(a));
    return 0;
}

// ---------------------------------------------------------------------------------------
// h7.* functions
// ---------------------------------------------------------------------------------------

static std::string ArgsToString(lua_State* L, int from) {
    std::string out;
    int n = lua_gettop(L);
    for (int i = from; i <= n; ++i) {
        if (i > from) out += "\t";
        out += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    return out;
}

static int LogAt(lua_State* L, const char* level) {
    int id = CurrentModId();
    std::string prefix = id > 0 ? "[" + ModName(id) + "] " : "[lua] ";
    Log("%s%s%s", level, prefix.c_str(), ArgsToString(L, 1).c_str());
    return 0;
}
static int H_log(lua_State* L) { return LogAt(L, ""); }
static int H_warn(lua_State* L) { return LogAt(L, "WARNING "); }
static int H_error(lua_State* L) { return LogAt(L, "ERROR "); }

static int H_find(lua_State* L) {
    PushObject(L, ue3::FindObject(luaL_checkstring(L, 1)));
    return 1;
}

static int H_find_class(lua_State* L) {
    PushObject(L, ue3::FindClass(luaL_checkstring(L, 1)));
    return 1;
}

// Class defaults (Default__X) and archetypes (templates in content packages, e.g. H7Config.H7AdventureSetup)
// are instances of their class too, but never the object a mod is looking for.
constexpr uint64_t RF_ClassDefaultObject = 0x200, RF_ArchetypeObject = 0x400;
static bool IsDefaultObject(UObject o) {
    uint64_t flags = mem::ReadOr<uint64_t>(o + ue3::L.objFlags, 0);
    return (flags & (RF_ClassDefaultObject | RF_ArchetypeObject)) || ue3::ObjName(o).rfind("Default__", 0) == 0;
}

// h7.find_all(class [, include_defaults]) -> array of live instances of class (or subclasses); with
// include_defaults, class defaults and archetypes are listed too
static int H_find_all(lua_State* L) {
    UObject cls = ClassArg(L, 1);
    bool defaults = lua_toboolean(L, 2);
    lua_newtable(L);
    int n = 0;
    int32_t count = ue3::ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        UObject o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, cls) || (!defaults && IsDefaultObject(o))) continue;
        PushObject(L, o);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int H_first(lua_State* L) {
    UObject cls = ClassArg(L, 1);
    int32_t count = ue3::ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        UObject o = ue3::ObjectAt(i);
        if (o && ue3::IsA(o, cls) && !IsDefaultObject(o)) return PushObject(L, o), 1;
    }
    lua_pushnil(L);
    return 1;
}


// ---------------------------------------------------------------------------------------
// Creating objects
// ---------------------------------------------------------------------------------------

static constexpr uint64_t kKeepFlags = ue3::RF_Public | ue3::RF_Standalone | ue3::RF_ArchetypeObject;

static uint64_t ObjFlags(UObject o) { return mem::ReadOr<uint64_t>(o + ue3::L.objFlags, 0); }

// The object called `name` directly inside `outer` (any class), or 0.
static UObject FindInOuter(UObject outer, const std::string& name) {
    int32_t idx = ue3::FindNameIndex(name);
    if (idx < 0) return 0;
    for (int32_t i = 0, n = ue3::ObjectCount(); i < n; ++i) {
        UObject o = ue3::ObjectAt(i);
        if (o && ue3::ObjOuter(o) == outer && ue3::ObjFName(o).index == idx && ue3::ObjFName(o).number == 0) return o;
    }
    return 0;
}

static UObject ConstructOrError(lua_State* L, UObject cls, UObject outer, const std::string& name, uint64_t flags,
                                UObject tmpl) {
    std::string err;
    UObject o = ue3::ConstructObject(cls, outer, name, flags, tmpl, err);
    if (!o) luaL_error(L, "could not create %s '%s': %s", ue3::ObjName(cls).c_str(), name.c_str(), err.c_str());
    return o;
}

// h7.new_object(class, {name=, outer=, template=, flags=, archetype=, replace=}) -> object
// Fails if `outer` already holds an object with that name, unless replace = true (the engine then reconstructs
// it in place: same address, properties reset).
static int H_new_object(lua_State* L) {
    UObject cls = ClassArg(L, 1);
    std::string name;
    UObject outer = 0, tmpl = 0;
    uint64_t flags = ue3::RF_Public | ue3::RF_Standalone;
    bool replace = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getfield(L, 2, "name") != LUA_TNIL) name = luaL_checkstring(L, -1);
        if (lua_getfield(L, 2, "outer") != LUA_TNIL) outer = CheckValidObject(L, -1);
        if (lua_getfield(L, 2, "template") != LUA_TNIL) tmpl = CheckValidObject(L, -1);
        if (lua_getfield(L, 2, "flags") != LUA_TNIL) flags = (uint64_t)luaL_checkinteger(L, -1);
        if (lua_getfield(L, 2, "archetype") != LUA_TNIL && lua_toboolean(L, -1)) flags |= ue3::RF_ArchetypeObject;
        if (lua_getfield(L, 2, "replace") != LUA_TNIL) replace = lua_toboolean(L, -1);
        lua_pop(L, 6);
    }
    if (!name.empty() && !replace) {
        UObject where = outer ? outer : ue3::TransientPackage();
        if (UObject existing = FindInOuter(where, name))
            luaL_error(L, "%s already exists (pass replace = true to reconstruct it)", ue3::ObjFullName(existing).c_str());
    }
    PushObject(L, ConstructOrError(L, cls, outer, name, flags, tmpl));
    return 1;
}

// h7.clone(source, name [, outer [, extra_flags]]) -> object, created
// A copy of `source` (same class, properties copied from it) called `name` inside `outer` (default: the transient
// package). Archetypes stay archetypes, so the copy can be used wherever the game expects a template. If `outer`
// already holds an object called `name` of the same class it is returned instead (created = false), which makes
// mod code safe to run again after a hot reload.
static int H_clone(lua_State* L) {
    UObject src = CheckValidObject(L, 1);
    std::string name = luaL_checkstring(L, 2);
    UObject outer = lua_isnoneornil(L, 3) ? ue3::TransientPackage() : CheckValidObject(L, 3);
    UObject cls = ue3::ObjClass(src);
    if (UObject existing = FindInOuter(outer, name)) {
        if (ue3::ObjClass(existing) != cls)
            luaL_error(L, "%s already exists with another class", ue3::ObjFullName(existing).c_str());
        PushObject(L, existing);
        lua_pushboolean(L, 0);
        return 2;
    }
    uint64_t flags = (ObjFlags(src) & kKeepFlags) | ue3::RF_Public | ue3::RF_Standalone;
    if (!lua_isnoneornil(L, 4)) flags |= (uint64_t)luaL_checkinteger(L, 4);
    PushObject(L, ConstructOrError(L, cls, outer, name, flags, src));
    lua_pushboolean(L, 1);
    return 2;
}

// h7.package(name) -> the top-level package `name`, created if it doesn't exist. Objects created inside it get
// stable path names ("Name.Object"), which is what save games store.
static int H_package(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    UObject cls = ue3::FindClass("Core.Package");
    if (!cls) luaL_error(L, "Core.Package not found");
    if (UObject p = ue3::FindObject("Package " + name)) {
        if (!ue3::ObjOuter(p)) return PushObject(L, p), 1;
    }
    std::string err;
    UObject p = ue3::ConstructTopLevel(cls, name, ue3::RF_Public | ue3::RF_Standalone, err);
    if (!p) luaL_error(L, "could not create package '%s': %s", name.c_str(), err.c_str());
    PushObject(L, p);
    return 1;
}

// h7.name(str) -> str; adds str to the engine name table (so it can be used as an FName)
static int H_name(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    if (ue3::FindNameIndex(s) < 0 && ue3::MakeName(s).index < 0) luaL_error(L, "could not create name '%s'", s);
    lua_pushstring(L, s);
    return 1;
}

static int H_can_create(lua_State* L) {
    lua_pushboolean(L, ue3::FactoryAvailable());
    return 1;
}

static int H_enum(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    UObject e = ue3::FindObject(std::string("Enum ") + name);
    if (!e) {
        // Allow bare enum names: search by object name.
        int32_t idx = ue3::FindNameIndex(name);
        for (int32_t i = 0, n = ue3::ObjectCount(); i < n && !e && idx >= 0; ++i) {
            UObject o = ue3::ObjectAt(i);
            if (o && ue3::ObjFName(o).index == idx && ue3::ObjName(ue3::ObjClass(o)) == "Enum") e = o;
        }
    }
    if (!e) luaL_error(L, "unknown enum '%s'", name);
    auto names = ue3::EnumNames(e);
    lua_createtable(L, 0, (int)names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, names[i].c_str());
    }
    return 1;
}

static void PushFieldList(lua_State* L, UObject s, bool functions) {
    lua_newtable(L);
    int n = 0;
    for (UObject f : ue3::Fields(s, true)) {
        bool isFn = IsFunction(f);
        if (isFn != functions) continue;
        if (!isFn && ue3::KindOf(f) == PropKind::Unknown) continue;
        lua_createtable(L, 0, 4);
        lua_pushstring(L, ue3::ObjName(f).c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, ue3::ObjName(ue3::ObjClass(f)).c_str());
        lua_setfield(L, -2, "type");
        lua_pushstring(L, ue3::ObjName(ue3::ObjOuter(f)).c_str());
        lua_setfield(L, -2, "owner");
        if (!isFn) {
            PropInfo p = ue3::Describe(f);
            lua_pushinteger(L, p.offset);
            lua_setfield(L, -2, "offset");
        } else {
            lua_pushinteger(L, ue3::FuncFlags(f));
            lua_setfield(L, -2, "flags");
        }
        lua_rawseti(L, -2, ++n);
    }
}

// h7.properties(classOrObjOrStructName) / h7.functions(...)
static UObject StructArg(lua_State* L, int idx) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx);
        UObject o = ue3::FindClass(s);
        if (!o) o = ue3::FindObject(std::string("ScriptStruct ") + s);
        if (!o) luaL_error(L, "unknown class/struct '%s'", s);
        return o;
    }
    if (auto* r = static_cast<StructRef*>(luaL_testudata(L, idx, kStruct))) return r->type;
    UObject o = CheckValidObject(L, idx);
    std::string cls = ue3::ObjName(ue3::ObjClass(o));
    return cls == "Class" || cls == "ScriptStruct" || cls == "Function" ? o : ue3::ObjClass(o);
}
static int H_properties(lua_State* L) { PushFieldList(L, StructArg(L, 1), false); return 1; }
static int H_functions(lua_State* L) { PushFieldList(L, StructArg(L, 1), true); return 1; }

// h7.to_table(struct/array/object) -> plain Lua copy (one level of objects, deep for structs/arrays)
static int H_to_table(lua_State* L) {
    if (auto* r = static_cast<ArrayRef*>(luaL_testudata(L, 1, kArray))) {
        CheckArray(L, 1);
        auto a = mem::ReadOr<ue3::TArrayRaw>(r->arr, {});
        PropInfo p = ue3::Describe(r->inner);
        lua_createtable(L, a.num, 0);
        for (int i = 0; i < a.num; ++i) {
            lua_pushcfunction(L, H_to_table);
            PushElem(L, a.data + i * p.elementSize, r->inner, p.kind, r->frame);
            // objects stay as references (expanding them recursed through object graphs: C stack overflow)
            if (!luaL_testudata(L, -1, kObj)) lua_call(L, 1, 1);
            else lua_remove(L, -2);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    UObject type = 0;
    uintptr_t base = 0;
    uint64_t frame = 0;
    const StructRef* sref = nullptr;
    if (auto* r = static_cast<StructRef*>(luaL_testudata(L, 1, kStruct))) {
        CheckStruct(L, 1);
        type = r->type, base = r->addr, frame = r->frame, sref = r;
    } else if (luaL_testudata(L, 1, kObj)) {
        base = CheckValidObject(L, 1);
        type = ue3::ObjClass(base);
    } else {
        lua_settop(L, 1);
        return 1; // plain values pass through
    }
    lua_newtable(L);
    for (UObject f : ue3::Fields(type, true)) {
        if (IsFunction(f) || ue3::KindOf(f) == PropKind::Unknown) continue;
        lua_pushcfunction(L, H_to_table);
        PushProp(L, sref ? FieldBase(sref, f) : base, f, frame);
        // objects stay as references; structs/arrays are expanded
        if (!luaL_testudata(L, -1, kObj)) lua_call(L, 1, 1);
        else lua_remove(L, -2);
        lua_setfield(L, -2, ue3::ObjName(f).c_str());
    }
    return 1;
}

// h7.exec("console command") -> result string, via the local PlayerController
static int H_exec(lua_State* L) {
    const char* cmd = luaL_checkstring(L, 1);
    UObject pcClass = ue3::FindClass("Engine.PlayerController");
    UObject pc = 0;
    for (int32_t i = 0, n = ue3::ObjectCount(); i < n && !pc; ++i) {
        UObject o = ue3::ObjectAt(i);
        if (o && ue3::IsA(o, pcClass) && !IsDefaultObject(o)) pc = o;
    }
    if (!pc) luaL_error(L, "no PlayerController exists yet");
    PushObject(L, pc);
    lua_getfield(L, -1, "ConsoleCommand");
    lua_insert(L, -2);
    lua_pushstring(L, cmd);
    lua_call(L, 2, 1);
    return 1;
}

// ---------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------

struct LuaHook {
    int mod;
    int ref;
    int listener;
};
static std::unordered_map<int, LuaHook> g_luaHooks; // keyed by listener id

static UObject ResolveFunction(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        UObject f = CheckValidObject(L, idx);
        if (!IsFunction(f)) luaL_error(L, "expected a Function object");
        return f;
    }
    std::string path = lua_tostring(L, idx);
    UObject f = ue3::FindObject(path.rfind("Function ", 0) == 0 ? path : "Function " + path);
    if (!f) luaL_error(L, "function '%s' not found (use Package.Class.Function)", path.c_str());
    return f;
}

static UObject ReturnProp(UObject fn) {
    for (auto& p : ue3::FuncParams(fn))
        if (p.flags & ue3::CPF_ReturnParm) return p.prop;
    return 0;
}

// UnrealScript-style type name of a property, e.g. "int", "H7Creature", "array<H7BaseCreatureStack>".
static std::string TypeName(UObject prop) {
    switch (ue3::KindOf(prop)) {
    case PropKind::Byte: return ue3::PropInner(prop) ? ue3::ObjName(ue3::PropInner(prop)) : "byte";
    case PropKind::Int: return "int";
    case PropKind::Bool: return "bool";
    case PropKind::Float: return "float";
    case PropKind::Name: return "name";
    case PropKind::Str: return "string";
    case PropKind::QWord: return "qword";
    case PropKind::Object: case PropKind::Struct: return ue3::ObjName(ue3::PropInner(prop));
    case PropKind::Class: return "class";
    case PropKind::Array: return "array<" + TypeName(ue3::PropInner(prop)) + ">";
    default: {
        std::string cls = ue3::ObjName(ue3::ObjClass(prop));
        return cls.size() > 8 && cls.ends_with("Property") ? cls.substr(0, cls.size() - 8) : cls;
    }
    }
}

// h7.signature(func) -> "native event int Name(int A, out float B, optional bool C)"
static int H_signature(lua_State* L) {
    UObject fn = ResolveFunction(L, 1);
    uint32_t flags = ue3::FuncFlags(fn);
    std::string s;
    if (flags & ue3::FUNC_Static) s += "static ";
    if (flags & ue3::FUNC_Native) s += "native ";
    s += (flags & ue3::FUNC_Event) ? "event " : "function ";
    std::string args, ret = "void";
    for (auto& p : ue3::FuncParams(fn)) {
        if (p.flags & ue3::CPF_ReturnParm) {
            ret = TypeName(p.prop);
            continue;
        }
        if (!args.empty()) args += ", ";
        if (p.flags & ue3::CPF_OutParm) args += "out ";
        if (p.flags & ue3::CPF_OptionalParm) args += "optional ";
        args += TypeName(p.prop) + " " + p.name;
    }
    s += ret + " " + ue3::ObjPathName(fn) + "(" + args + ")";
    lua_pushstring(L, s.c_str());
    return 1;
}

static int WriteReturnThunk(lua_State* L) {
    auto addr = reinterpret_cast<uintptr_t>(lua_touserdata(L, 2));
    auto prop = reinterpret_cast<UObject>(lua_touserdata(L, 3));
    WriteElem(L, 1, addr, prop, ue3::KindOf(prop));
    return 0;
}

static bool RunLuaHook(int ref, int mod, hooks::CallContext& ctx, bool post) {
    lua_State* L = State();
    if (!L) return false;
    ModScope scope(mod);
    int top = lua_gettop(L);
    lua_pushcfunction(L, Traceback);
    int eh = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    PushObject(L, ctx.self);
    uint64_t frame = g_nextFrame++;
    g_liveFrames.insert(frame);
    if (ctx.locals) {
        PushStruct(L, reinterpret_cast<uintptr_t>(ctx.locals), ctx.function, frame);
        static_cast<StructRef*>(lua_touserdata(L, -1))->outParms = reinterpret_cast<uintptr_t>(ctx.outParms);
    } else {
        lua_pushnil(L);
    }
    int nargs = 2;
    UObject ret = post && ctx.result ? ReturnProp(ctx.function) : 0;
    if (ret) {
        PushElem(L, reinterpret_cast<uintptr_t>(ctx.result), ret, ue3::KindOf(ret), frame);
        ++nargs;
    }
    bool block = false;
    if (lua_pcall(L, nargs, 1, eh) != LUA_OK) {
        Report("hook " + ue3::ObjPathName(ctx.function) + " (" + ModName(mod) + ")", lua_tostring(L, -1));
    } else if (!post) {
        block = lua_isboolean(L, -1) && lua_toboolean(L, -1);
    } else if (ret && !lua_isnil(L, -1)) {
        // Override the return value (protected, so type errors are reported instead of aborting).
        lua_pushcfunction(L, WriteReturnThunk);
        lua_insert(L, -2);
        lua_pushlightuserdata(L, ctx.result);
        lua_pushlightuserdata(L, reinterpret_cast<void*>(ret));
        if (lua_pcall(L, 3, 0, eh) != LUA_OK) Report("hook return " + ue3::ObjPathName(ctx.function), lua_tostring(L, -1));
    }
    g_liveFrames.erase(frame);
    lua_settop(L, top);
    return block;
}

// h7.hook(func, callback [, "pre"|"post"]) -> id
static int AddHook(lua_State* L, bool post) {
    UObject fn = ResolveFunction(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int mod = CurrentModId();
    hooks::PreFn pre;
    hooks::PostFn postFn;
    if (post) postFn = [ref, mod](hooks::CallContext& c) { RunLuaHook(ref, mod, c, true); };
    else pre = [ref, mod](hooks::CallContext& c) { return RunLuaHook(ref, mod, c, false); };
    int id = hooks::Add(fn, pre, postFn);
    if (!id) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        luaL_error(L, "could not hook %s", ue3::ObjPathName(fn).c_str());
    }
    if (!post && (ue3::FuncFlags(fn) & ue3::FUNC_Native))
        Log("note: %s is native; its hook receives no params and cannot block", ue3::ObjPathName(fn).c_str());
    g_luaHooks[id] = {mod, ref, id};
    lua_pushinteger(L, id);
    return 1;
}
static int H_hook(lua_State* L) {
    const char* when = luaL_optstring(L, 3, "pre");
    return AddHook(L, strcmp(when, "post") == 0);
}
static int H_hook_post(lua_State* L) { return AddHook(L, true); }

static void RemoveHook(lua_State* L, int id) {
    auto it = g_luaHooks.find(id);
    if (it == g_luaHooks.end()) return;
    hooks::Remove(id);
    luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
    g_luaHooks.erase(it);
}

static int H_unhook(lua_State* L) {
    RemoveHook(L, (int)luaL_checkinteger(L, 1));
    return 0;
}

void RemoveModHooks(int modId) {
    std::vector<int> ids;
    for (auto& [id, h] : g_luaHooks)
        if (h.mod == modId) ids.push_back(id);
    for (int id : ids) RemoveHook(State(), id);
}

int Traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : luaL_tolstring(L, 1, nullptr), 1);
    return 1;
}

// ---------------------------------------------------------------------------------------

void OpenBindings(lua_State* L) {
    luaL_newmetatable(L, kObj);
    lua_pushcfunction(L, Obj_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Obj_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Obj_eq); lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Obj_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    const luaL_Reg methods[] = {
        {"get_name", M_get_name}, {"get_full_name", M_get_full_name}, {"get_path_name", M_get_path_name},
        {"get_class", M_get_class}, {"get_outer", M_get_outer}, {"get_address", M_get_address},
        {"get_default", M_get_default}, {"get_super", M_get_super}, {"is_valid", M_is_valid},
        {"is_a", M_is_a}, {"call", M_call}, {nullptr, nullptr},
    };
    luaL_newlib(L, methods);
    lua_setfield(L, LUA_REGISTRYINDEX, "H7ObjectMethods");

    luaL_newmetatable(L, kStruct);
    lua_pushcfunction(L, Struct_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Struct_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Struct_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_newmetatable(L, kArray);
    lua_pushcfunction(L, Array_index); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Array_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Array_len); lua_setfield(L, -2, "__len");
    lua_pop(L, 1);

    const luaL_Reg h7lib[] = {
        {"log", H_log}, {"warn", H_warn}, {"error", H_error},
        {"find", H_find}, {"find_class", H_find_class}, {"find_all", H_find_all}, {"first", H_first},
        {"enum", H_enum}, {"properties", H_properties}, {"functions", H_functions}, {"to_table", H_to_table},
        {"exec", H_exec}, {"hook", H_hook}, {"hook_post", H_hook_post}, {"unhook", H_unhook},
        {"signature", H_signature}, {"new_object", H_new_object}, {"clone", H_clone}, {"package", H_package},
        {"name", H_name}, {"can_create", H_can_create}, {nullptr, nullptr},
    };
    lua_getglobal(L, "h7");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    luaL_setfuncs(L, h7lib, 0);
    lua_newtable(L); // h7.flags: object flags for new_object / clone
    const std::pair<const char*, uint64_t> flagList[] = {
        {"Public", ue3::RF_Public}, {"Standalone", ue3::RF_Standalone}, {"Transient", ue3::RF_Transient},
        {"Archetype", ue3::RF_ArchetypeObject}, {"RootSet", ue3::RF_RootSet},
    };
    for (auto& [n, v] : flagList) { lua_pushinteger(L, (lua_Integer)v); lua_setfield(L, -2, n); }
    lua_setfield(L, -2, "flags");
    lua_setglobal(L, "h7");
}

} // namespace h7::lua
