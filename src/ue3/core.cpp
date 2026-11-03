#include "core.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <windows.h>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace h7::ue3 {

Layout L;

static uintptr_t g_names = 0;   // address of TArray<FNameEntry*>
static uintptr_t g_objects = 0; // address of TArray<UObject*>
static bool g_namesWideOnly = false;

uintptr_t GNamesAddress() { return g_names; }
uintptr_t GObjectsAddress() { return g_objects; }

static bool ReadArray(uintptr_t at, TArrayRaw& a) { return at && mem::Read(at, a); }

// ---------------------------------------------------------------------------
// GNames discovery
// ---------------------------------------------------------------------------

static int FindAnsi(const uint8_t* buf, size_t n, const char* s) {
    size_t len = strlen(s) + 1; // include terminator
    for (size_t i = 0; i + len <= n; ++i)
        if (memcmp(buf + i, s, len) == 0) return (int)i;
    return -1;
}

static int FindWide(const uint8_t* buf, size_t n, const char* s) {
    uint8_t w[128] = {};
    size_t len = strlen(s);
    for (size_t i = 0; i < len; ++i) w[i * 2] = (uint8_t)s[i];
    size_t wl = (len + 1) * 2;
    for (size_t i = 0; i + wl <= n; i += 2)
        if (memcmp(buf + i, w, wl) == 0) return (int)i;
    return -1;
}

static bool TryNamesCandidate(uintptr_t at, const TArrayRaw& a) {
    if (a.num < 1000 || a.num > 4'000'000 || a.max < a.num || a.max > 8'000'000) return false;
    if (a.data < 0x10000 || (a.data & 7) || mem::g_game.Contains(a.data)) return false;
    uintptr_t e[3];
    if (!mem::Read(reinterpret_cast<void*>(a.data), e, sizeof(e))) return false;
    uint8_t b0[0x48], b1[0x48], b2[0x48];
    if (!mem::Read((void*)e[0], b0, sizeof(b0)) || !mem::Read((void*)e[1], b1, sizeof(b1)) ||
        !mem::Read((void*)e[2], b2, sizeof(b2)))
        return false;
    int k = FindAnsi(b0, sizeof(b0), "None");
    bool wide = false;
    if (k < 0 || FindAnsi(b1, sizeof(b1), "ByteProperty") != k || FindAnsi(b2, sizeof(b2), "IntProperty") != k) {
        k = FindWide(b0, sizeof(b0), "None");
        if (k < 0 || FindWide(b1, sizeof(b1), "ByteProperty") != k) return false;
        wide = true;
    }
    g_names = at;
    L.nameEntryString = k;
    g_namesWideOnly = wide;

    // Find the Index field: (i << 1 | unicode) for the first entries.
    for (int off = 0; off + 4 <= k; off += 4) {
        bool ok = true;
        for (int i = 0; i < 32 && ok; ++i) {
            uintptr_t ent = 0;
            int32_t v = 0;
            ok = mem::Read(a.data + i * 8, ent) && ent && mem::Read(ent + off, v) && (v >> 1) == i;
        }
        if (ok) { L.nameEntryIndex = off; break; }
    }
    Log("GNames @ %p (data %p, num %d, max %d) nameOffset=0x%x indexField=0x%x wide=%d", (void*)at, (void*)a.data,
        a.num, a.max, k, L.nameEntryIndex, (int)wide);
    Log("FNameEntry[0..2]:\n%s%s%s", HexDump(b0, sizeof(b0), e[0]).c_str(), HexDump(b1, sizeof(b1), e[1]).c_str(),
        HexDump(b2, sizeof(b2), e[2]).c_str());
    return true;
}

static uintptr_t ScanModuleForArray(bool (*pred)(uintptr_t, const TArrayRaw&)) {
    uintptr_t found = 0;
    std::vector<uint8_t> buf;
    mem::ForEachRegion(mem::g_game.base, mem::g_game.end, true, [&](uintptr_t base, size_t size) {
        if (found) return;
        buf.resize(size);
        if (!mem::Read((void*)base, buf.data(), size)) return;
        for (size_t o = 0; o + sizeof(TArrayRaw) <= size; o += 8) {
            TArrayRaw a;
            memcpy(&a, buf.data() + o, sizeof(a));
            if (pred(base + o, a)) { found = base + o; return; }
        }
    });
    return found;
}

bool FindGNames() {
    if (g_names) return true;
    return ScanModuleForArray(TryNamesCandidate) != 0;
}

int32_t NameCount() {
    TArrayRaw a{};
    return ReadArray(g_names, a) ? a.num : 0;
}

std::string NameToString(int32_t index) {
    TArrayRaw a{};
    if (!ReadArray(g_names, a) || index < 0 || index >= a.num) return {};
    uintptr_t ent = 0;
    if (!mem::Read(a.data + (uintptr_t)index * 8, ent) || !ent) return {};
    bool wide = g_namesWideOnly;
    if (L.nameEntryIndex >= 0) {
        int32_t v = 0;
        if (mem::Read(ent + L.nameEntryIndex, v)) wide = (v & 1) != 0;
    }
    std::string out;
    uintptr_t p = ent + L.nameEntryString;
    if (wide) {
        wchar_t ch[256];
        if (!mem::Read((void*)p, ch, sizeof(ch))) return {};
        for (int i = 0; i < 256 && ch[i]; ++i) out.push_back(ch[i] < 128 ? (char)ch[i] : '?');
    } else {
        char ch[256];
        if (!mem::Read((void*)p, ch, sizeof(ch))) return {};
        for (int i = 0; i < 256 && ch[i]; ++i) out.push_back(ch[i]);
    }
    return out;
}

std::string NameToString(const FName& n) {
    std::string s = NameToString(n.index);
    if (n.number > 0) s += "_" + std::to_string(n.number - 1);
    return s;
}

static std::mutex g_cacheMutex;
static std::unordered_map<std::string, int32_t> g_nameCache;
static int32_t g_nameCacheCount = 0;
static std::unordered_map<std::string, UObject> g_objectCache;

void InvalidateLookupCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_objectCache.clear();
}

int32_t FindNameIndex(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    int32_t count = NameCount();
    if (count != g_nameCacheCount) {
        for (int32_t i = g_nameCacheCount; i < count; ++i) {
            std::string s = NameToString(i);
            if (!s.empty()) g_nameCache.emplace(s, i);
        }
        g_nameCacheCount = count;
    }
    auto it = g_nameCache.find(name);
    if (it != g_nameCache.end()) return it->second;
    // Names are case-insensitive in UE3; fall back to a slow insensitive compare.
    for (auto& [k, v] : g_nameCache)
        if (_stricmp(k.c_str(), name.c_str()) == 0) return v;
    return -1;
}

// ---------------------------------------------------------------------------
// GObjects discovery
// ---------------------------------------------------------------------------

static bool TryObjectsCandidate(uintptr_t at, const TArrayRaw& a) {
    if (a.num < 1000 || a.num > 50'000'000 || a.max < a.num || a.max > 100'000'000) return false;
    if (a.data < 0x10000 || (a.data & 7) || mem::g_game.Contains(a.data)) return false;
    uintptr_t objs[256];
    if (!mem::Read((void*)a.data, objs, sizeof(objs))) return false;
    int32_t names = NameCount();
    int good = 0;
    for (int i = 0; i < 256; ++i) {
        if (!objs[i]) continue;
        uintptr_t vt = 0;
        int32_t idx = -1;
        FName n{};
        if (!mem::Read(objs[i] + L.objVTable, vt) || !mem::g_game.Contains(vt)) return false;
        if (!mem::Read(objs[i] + L.objInternalIndex, idx) || idx != i) return false;
        if (!mem::Read(objs[i] + L.objName, n) || n.index < 0 || n.index >= names) return false;
        ++good;
    }
    if (good < 128) return false;
    Log("GObjects @ %p (data %p, num %d, max %d)", (void*)at, (void*)a.data, a.num, a.max);
    return true;
}

bool FindGObjects() {
    if (g_objects) return true;
    g_objects = ScanModuleForArray(TryObjectsCandidate);
    return g_objects != 0;
}

int32_t ObjectCount() {
    TArrayRaw a{};
    return ReadArray(g_objects, a) ? a.num : 0;
}

UObject ObjectAt(int32_t index) {
    TArrayRaw a{};
    if (!ReadArray(g_objects, a) || index < 0 || index >= a.num) return 0;
    return mem::ReadOr<uintptr_t>(a.data + (uintptr_t)index * 8, 0);
}

bool IsValidObject(UObject o) {
    if (!o) return false;
    int32_t idx = -1;
    return mem::Read(o + L.objInternalIndex, idx) && ObjectAt(idx) == o;
}

UObject ObjClass(UObject o) { return o ? mem::ReadOr<uintptr_t>(o + L.objClass, 0) : 0; }
UObject ObjOuter(UObject o) { return o ? mem::ReadOr<uintptr_t>(o + L.objOuter, 0) : 0; }
FName ObjFName(UObject o) { return o ? mem::ReadOr<FName>(o + L.objName, FName{-1, 0}) : FName{-1, 0}; }
std::string ObjName(UObject o) { return o ? NameToString(ObjFName(o)) : "None"; }

std::string ObjPathName(UObject o) {
    if (!o) return "None";
    std::string s = ObjName(o);
    int guard = 0;
    for (UObject outer = ObjOuter(o); outer && guard < 64; outer = ObjOuter(outer), ++guard)
        s = ObjName(outer) + "." + s;
    return s;
}

std::string ObjFullName(UObject o) {
    if (!o) return "None";
    return ObjName(ObjClass(o)) + " " + ObjPathName(o);
}

bool IsA(UObject o, UObject cls) {
    if (!o || !cls || L.structSuper < 0) return false;
    int guard = 0;
    for (UObject c = ObjClass(o); c && guard < 128; c = mem::ReadOr<uintptr_t>(c + L.structSuper, 0), ++guard)
        if (c == cls) return true;
    return false;
}

static bool MatchesPath(UObject o, const std::string& path) {
    // Compare from the innermost name outwards to avoid building strings.
    size_t end = path.size();
    UObject cur = o;
    while (cur) {
        size_t dot = path.rfind('.', end - 1);
        size_t start = dot == std::string::npos ? 0 : dot + 1;
        std::string part = path.substr(start, end - start);
        if (_stricmp(ObjName(cur).c_str(), part.c_str()) != 0) return false;
        if (dot == std::string::npos) return ObjOuter(cur) == 0;
        end = dot;
        cur = ObjOuter(cur);
    }
    return false;
}

UObject FindObject(const std::string& query) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_objectCache.find(query);
        if (it != g_objectCache.end() && IsValidObject(it->second)) return it->second;
    }
    std::string className, path = query;
    size_t sp = query.find(' ');
    if (sp != std::string::npos) {
        className = query.substr(0, sp);
        path = query.substr(sp + 1);
    }
    size_t lastDot = path.rfind('.');
    std::string leaf = lastDot == std::string::npos ? path : path.substr(lastDot + 1);
    // Leaf may carry a _N number suffix; match on the base name index, then verify the full path.
    int32_t leafIdx = FindNameIndex(leaf);
    std::string leafBase = leaf;
    if (leafIdx < 0) {
        size_t us = leaf.rfind('_');
        if (us != std::string::npos) leafIdx = FindNameIndex(leafBase = leaf.substr(0, us));
        if (leafIdx < 0) return 0;
    }
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        UObject o = ObjectAt(i);
        if (!o || ObjFName(o).index != leafIdx) continue;
        if (!className.empty() && _stricmp(ObjName(ObjClass(o)).c_str(), className.c_str()) != 0) continue;
        if (!MatchesPath(o, path)) continue;
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_objectCache[query] = o;
        return o;
    }
    return 0;
}

UObject FindClass(const std::string& path) {
    if (path.find('.') != std::string::npos) return FindObject("Class " + path);
    int32_t idx = FindNameIndex(path);
    if (idx < 0) return 0;
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        UObject o = ObjectAt(i);
        if (o && ObjFName(o).index == idx && ObjName(ObjClass(o)) == "Class") return o;
    }
    return 0;
}

} // namespace h7::ue3
