// Lua runtime: state lifetime, mod discovery/loading/hot-reload, events, timers, dev exec folder.
// Everything here runs on the game thread (driven by the viewport Tick hook).
#include "runtime.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../engine/paths.h"
#include "../ue3/core.h"
#include "../ue3/reflection.h"
#include <windows.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace h7::lua {

static lua_State* g_L = nullptr;
lua_State* State() { return g_L; }

static const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------------------
// Mods
// ---------------------------------------------------------------------------------------

struct Mod {
    int id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string category;       // manifest `category`: groups mods in the Mods menu
    std::vector<std::string> loadAfter;
    fs::path dir;
    bool defaultEnabled = true; // manifest `enabled`
    bool enabled = true;        // after the ini's [Mods] Enabled/Disabled lists
    bool hidden = false;        // manifest `hidden`: left out of mod lists shown to players
    int envRef = LUA_NOREF;
    bool loaded = false;
    long long stamp = 0; // newest .lua write time, for hot reload
};

static std::vector<Mod> g_mods;
static int g_currentMod = 0;
static std::string g_noMod = "engine";

int CurrentModId() { return g_currentMod; }
const std::string& ModName(int id) {
    for (auto& m : g_mods)
        if (m.id == id) return m.name;
    return g_noMod;
}

struct ModScope {
    int saved;
    explicit ModScope(int id) : saved(g_currentMod) { g_currentMod = id; }
    ~ModScope() { g_currentMod = saved; }
};

void Report(const std::string& where, const std::string& error) { Log("ERROR in %s: %s", where.c_str(), error.c_str()); }

// ---------------------------------------------------------------------------------------
// Events and timers
// ---------------------------------------------------------------------------------------

struct Handler {
    int id;
    int mod;
    int ref;
};
static std::map<std::string, std::vector<Handler>> g_events;

struct Timer {
    int id;
    int mod;
    int ref;
    double due;
    double interval; // 0 = one-shot
};
static std::vector<Timer> g_timers;
static int g_nextId = 1;
static double g_clock = 0;

static int H_on(lua_State* L) {
    const char* ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int id = g_nextId++;
    g_events[ev].push_back({id, g_currentMod, luaL_ref(L, LUA_REGISTRYINDEX)});
    lua_pushinteger(L, id);
    return 1;
}

static int H_off(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    for (auto& [name, list] : g_events) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id != id) continue;
            luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
            list.erase(it);
            return 0;
        }
    }
    return 0;
}

void EmitEvent(const char* name, int nargs) {
    lua_State* L = g_L;
    int base = lua_gettop(L) - nargs; // args occupy base+1..top
    auto it = g_events.find(name);
    if (it != g_events.end()) {
        std::vector<Handler> list = it->second; // handlers may unsubscribe while running
        for (auto& h : list) {
            ModScope scope(h.mod);
            lua_pushcfunction(L, Traceback);
            lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
            for (int i = 1; i <= nargs; ++i) lua_pushvalue(L, base + i);
            if (lua_pcall(L, nargs, 0, base + nargs + 1) != LUA_OK)
                Report(std::string("event '") + name + "' (" + ModName(h.mod) + ")", lua_tostring(L, -1));
            lua_settop(L, base + nargs);
        }
    }
    lua_settop(L, base);
}

static int AddTimer(lua_State* L, bool repeat) {
    double secs = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int id = g_nextId++;
    g_timers.push_back({id, g_currentMod, luaL_ref(L, LUA_REGISTRYINDEX), g_clock + secs, repeat ? std::max(secs, 0.0) : 0});
    lua_pushinteger(L, id);
    return 1;
}
static int H_after(lua_State* L) { return AddTimer(L, false); }
static int H_every(lua_State* L) { return AddTimer(L, true); }

static int H_cancel(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it) {
        if (it->id != id) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, it->ref);
        g_timers.erase(it);
        break;
    }
    return 0;
}

static void RunTimers() {
    lua_State* L = g_L;
    std::vector<Timer> due;
    for (auto it = g_timers.begin(); it != g_timers.end();) {
        if (it->due > g_clock) { ++it; continue; }
        due.push_back(*it);
        if (it->interval > 0) {
            it->due = g_clock + it->interval;
            ++it;
        } else {
            it = g_timers.erase(it);
        }
    }
    for (auto& t : due) {
        ModScope scope(t.mod);
        lua_pushcfunction(L, Traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, t.ref);
        if (lua_pcall(L, 0, 0, -2) != LUA_OK) Report("timer (" + ModName(t.mod) + ")", lua_tostring(L, -1));
        lua_settop(L, 0);
        if (t.interval == 0) luaL_unref(L, LUA_REGISTRYINDEX, t.ref);
    }
}

// ---------------------------------------------------------------------------------------
// Mod loading
// ---------------------------------------------------------------------------------------

static long long NewestLuaStamp(const fs::path& dir) {
    long long newest = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.path().extension() != ".lua") continue;
        newest = std::max<long long>(newest, e.last_write_time(ec).time_since_epoch().count());
    }
    return newest;
}

// [Mods] Enabled= / Disabled= in H7ModEngine.ini: comma-separated mod names that override the manifest's `enabled`.
static std::vector<std::string> ModList(const wchar_t* key) {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"Mods", key, L"", buf, 2048, EnginePath(L"H7ModEngine.ini").c_str());
    std::vector<std::string> out;
    std::string s = Narrow(buf), cur;
    for (char c : s + ",") {
        if (c == ',') {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != ' ' || !cur.empty()) {
            cur += c;
        }
    }
    return out;
}

static void WriteModList(const wchar_t* key, const std::vector<std::string>& names) {
    std::string s;
    for (auto& n : names) s += (s.empty() ? "" : ", ") + n;
    WritePrivateProfileStringW(L"Mods", key, Widen(s).c_str(), EnginePath(L"H7ModEngine.ini").c_str());
}

static bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

static bool EffectiveEnabled(const Mod& m, const std::vector<std::string>& enabled,
                             const std::vector<std::string>& disabled) {
    if (Contains(disabled, m.name)) return false;
    if (Contains(enabled, m.name)) return true;
    return m.defaultEnabled;
}

static std::string TableString(lua_State* L, int idx, const char* key, const std::string& fallback) {
    lua_getfield(L, idx, key);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}

// Reads mods/<dir>/mod.lua (optional manifest returning a table). False if the manifest fails to run.
static bool ReadManifest(Mod& m) {
    fs::path manifest = m.dir / "mod.lua";
    m.name = m.dir.filename().string();
    if (!fs::exists(manifest)) return true;
    lua_State* L = g_L;
    if (luaL_dofile(L, manifest.string().c_str()) != LUA_OK) {
        Report("manifest " + manifest.string(), lua_tostring(L, -1));
        lua_settop(L, 0);
        return false;
    }
    if (lua_istable(L, -1)) {
        m.name = TableString(L, -1, "name", m.name);
        m.version = TableString(L, -1, "version", "");
        m.author = TableString(L, -1, "author", "");
        m.description = TableString(L, -1, "description", "");
        m.category = TableString(L, -1, "category", "");
        lua_getfield(L, -1, "load_after");
        if (lua_istable(L, -1)) {
            for (int i = 1; lua_rawgeti(L, -1, i) == LUA_TSTRING; ++i) {
                m.loadAfter.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "enabled");
        m.defaultEnabled = lua_isnil(L, -1) || lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "hidden");
        m.hidden = lua_toboolean(L, -1);
        lua_settop(L, 0);
        return true;
    }
    lua_settop(L, 0);
    return true;
}

// mod.require("file") — loads <moddir>/file.lua in the mod's environment (cached per mod).
static int Mod_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int envIdx = lua_upvalueindex(1);
    lua_getfield(L, envIdx, "__modules");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    lua_getfield(L, envIdx, "mod");
    lua_getfield(L, -1, "dir");
    std::string path = std::string(lua_tostring(L, -1)) + "\\" + name + ".lua";
    lua_pop(L, 2);
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) return lua_error(L);
    lua_pushvalue(L, envIdx);
    lua_setupvalue(L, -2, 1); // _ENV
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

// Persistent per-mod data: mod.save(tbl) / mod.load() -> tbl, stored as a Lua chunk in data\<mod>.lua.
static void Serialize(lua_State* L, int idx, std::string& out, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) out += std::to_string(lua_tointeger(L, idx));
        else { char b[64]; snprintf(b, sizeof(b), "%.17g", lua_tonumber(L, idx)); out += b; }
        break;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
    case LUA_TSTRING: {
        size_t n;
        const char* s = lua_tolstring(L, idx, &n);
        out += '"';
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = s[i];
            if (c == '"' || c == '\\') out += '\\', out += c;
            else if (c == '\n') out += "\\n";
            else if (c < 32) { char b[8]; snprintf(b, sizeof(b), "\\%03d", c); out += b; }
            else out += c;
        }
        out += '"';
        break;
    }
    case LUA_TTABLE:
        if (depth > 32) luaL_error(L, "table too deep to save");
        out += "{";
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            out += "[";
            Serialize(L, -2, out, depth + 1);
            out += "]=";
            Serialize(L, -1, out, depth + 1);
            out += ",";
            lua_pop(L, 1);
        }
        out += "}";
        break;
    default: out += "nil"; break;
    }
}

static fs::path DataFile(lua_State* L) {
    lua_getfield(L, lua_upvalueindex(1), "name");
    fs::path p = fs::path(EnginePath(L"data")) / (std::string(lua_tostring(L, -1)) + ".lua");
    lua_pop(L, 1);
    return p;
}

static int Mod_save(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string out = "return ";
    Serialize(L, 1, out, 0);
    fs::create_directories(EnginePath(L"data"));
    FILE* f = nullptr;
    if (_wfopen_s(&f, DataFile(L).wstring().c_str(), L"wb") || !f) return luaL_error(L, "could not write save data");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}

static int Mod_load(lua_State* L) {
    fs::path p = DataFile(L);
    if (!fs::exists(p)) return lua_newtable(L), 1;
    if (luaL_loadfile(L, p.string().c_str()) != LUA_OK) return lua_error(L);
    lua_newtable(L); // empty _ENV: data files can't call anything
    lua_setupvalue(L, -2, 1);
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) lua_newtable(L);
    return 1;
}

static void UnloadMod(Mod& m) {
    if (!m.loaded) return;
    lua_State* L = g_L;
    {
        // Give the mod a chance to clean up: global function on_unload() in its environment.
        ModScope scope(m.id);
        lua_pushcfunction(L, Traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, m.envRef);
        lua_getfield(L, -1, "on_unload");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 1) != LUA_OK) Report(m.name + ".on_unload", lua_tostring(L, -1));
        }
        lua_settop(L, 0);
    }
    RemoveModHooks(m.id);
    for (auto& [name, list] : g_events) {
        for (auto& h : list)
            if (h.mod == m.id) luaL_unref(L, LUA_REGISTRYINDEX, h.ref);
        list.erase(std::remove_if(list.begin(), list.end(), [&](auto& h) { return h.mod == m.id; }), list.end());
    }
    for (auto& t : g_timers)
        if (t.mod == m.id) luaL_unref(L, LUA_REGISTRYINDEX, t.ref);
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(), [&](auto& t) { return t.mod == m.id; }), g_timers.end());
    luaL_unref(L, LUA_REGISTRYINDEX, m.envRef);
    m.envRef = LUA_NOREF;
    m.loaded = false;
    Log("Unloaded mod %s", m.name.c_str());
}

static void LoadMod(Mod& m) {
    lua_State* L = g_L;
    ModScope scope(m.id);
    fs::path entry = m.dir / "main.lua";
    if (!fs::exists(entry)) {
        Log("Mod %s has no main.lua; skipped", m.name.c_str());
        return;
    }
    // Environment: own globals, falling back to the shared _G.
    lua_newtable(L);
    int env = lua_gettop(L);
    lua_newtable(L);
    lua_getglobal(L, "_G");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);
    lua_newtable(L);
    lua_setfield(L, env, "__modules");

    lua_createtable(L, 0, 8);
    lua_pushstring(L, m.name.c_str()); lua_setfield(L, -2, "name");
    lua_pushstring(L, m.version.c_str()); lua_setfield(L, -2, "version");
    lua_pushstring(L, m.author.c_str()); lua_setfield(L, -2, "author");
    lua_pushstring(L, m.dir.string().c_str()); lua_setfield(L, -2, "dir");
    lua_pushvalue(L, env); lua_pushcclosure(L, Mod_require, 1); lua_setfield(L, -2, "require");
    lua_pushvalue(L, -1); lua_pushcclosure(L, Mod_save, 1); lua_setfield(L, -2, "save");
    lua_pushvalue(L, -1); lua_pushcclosure(L, Mod_load, 1); lua_setfield(L, -2, "load");
    lua_setfield(L, env, "mod");

    lua_pushvalue(L, env);
    m.envRef = luaL_ref(L, LUA_REGISTRYINDEX);
    m.loaded = true;
    m.stamp = NewestLuaStamp(m.dir);

    lua_pushcfunction(L, Traceback);
    int eh = lua_gettop(L);
    if (luaL_loadfile(L, entry.string().c_str()) != LUA_OK) {
        Report(m.name + "/main.lua", lua_tostring(L, -1));
    } else {
        lua_pushvalue(L, env);
        lua_setupvalue(L, -2, 1);
        if (lua_pcall(L, 0, 0, eh) != LUA_OK) Report(m.name + "/main.lua", lua_tostring(L, -1));
        else Log("Loaded mod %s %s%s", m.name.c_str(), m.version.c_str(), m.author.empty() ? "" : (" by " + m.author).c_str());
    }
    lua_settop(L, 0);
}

// Every mod folder is listed (h7.mods), but only enabled mods load.
static void DiscoverAndLoadMods() {
    g_mods.clear();
    std::vector<std::string> enabled = ModList(L"Enabled"), disabled = ModList(L"Disabled");
    std::error_code ec;
    int nextId = 1;
    std::vector<Mod> found;
    for (auto& e : fs::directory_iterator(EnginePath(L"mods"), ec)) {
        if (!e.is_directory()) continue;
        Mod m;
        m.id = nextId++;
        m.dir = e.path();
        if (!ReadManifest(m)) continue;
        m.enabled = EffectiveEnabled(m, enabled, disabled);
        if (!m.enabled) Log("Mod %s is disabled", m.name.c_str());
        found.push_back(std::move(m));
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    // Order: alphabetical, then move each mod after the mods it lists in load_after.
    std::vector<Mod> ordered;
    while (!found.empty()) {
        bool progressed = false;
        for (auto it = found.begin(); it != found.end(); ++it) {
            bool ready = std::all_of(it->loadAfter.begin(), it->loadAfter.end(), [&](auto& dep) {
                return std::none_of(found.begin(), found.end(), [&](auto& f) { return f.name == dep; });
            });
            if (!ready) continue;
            ordered.push_back(std::move(*it));
            found.erase(it);
            progressed = true;
            break;
        }
        if (!progressed) { // dependency cycle: load the rest alphabetically
            Log("warning: load_after cycle among remaining mods");
            for (auto& m : found) ordered.push_back(std::move(m));
            break;
        }
    }
    g_mods = std::move(ordered);
    int loaded = 0;
    for (auto& m : g_mods)
        if (m.enabled) LoadMod(m), ++loaded;
    lua_pushinteger(g_L, loaded);
    EmitEvent("mods_loaded", 1);
}

// h7.set_mod_enabled(name, enabled): remembered in the ini; the mod loads or unloads on the next frame
// (never inside the caller, which may be the mod itself).
static bool g_toggled = false;
static int H_set_mod_enabled(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool on = lua_toboolean(L, 2);
    auto it = std::find_if(g_mods.begin(), g_mods.end(), [&](auto& m) { return m.name == name; });
    if (it == g_mods.end()) return luaL_error(L, "unknown mod '%s'", name);
    std::vector<std::string> enabled = ModList(L"Enabled"), disabled = ModList(L"Disabled");
    std::erase(enabled, it->name);
    std::erase(disabled, it->name);
    if (on != it->defaultEnabled) (on ? enabled : disabled).push_back(it->name);
    WriteModList(L"Enabled", enabled);
    WriteModList(L"Disabled", disabled);
    it->enabled = on;
    g_toggled = true;
    return 0;
}

static void ApplyToggles() {
    for (auto& m : g_mods) {
        if (m.enabled == m.loaded) continue;
        if (m.enabled) {
            LoadMod(m);
        } else {
            UnloadMod(m);
        }
        lua_pushstring(g_L, m.name.c_str());
        lua_pushboolean(g_L, m.enabled);
        EmitEvent("mod_toggled", 2);
    }
}

static bool g_reloadAll = false;
static int H_reload(lua_State* L) {
    g_reloadAll = true;
    return 0;
}

static int H_mods(lua_State* L) {
    lua_newtable(L);
    int i = 0;
    for (auto& m : g_mods) {
        lua_createtable(L, 0, 7);
        lua_pushstring(L, m.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, m.version.c_str()); lua_setfield(L, -2, "version");
        lua_pushstring(L, m.author.c_str()); lua_setfield(L, -2, "author");
        lua_pushstring(L, m.description.c_str()); lua_setfield(L, -2, "description");
        lua_pushstring(L, m.category.c_str()); lua_setfield(L, -2, "category");
        lua_pushboolean(L, m.enabled); lua_setfield(L, -2, "enabled");
        lua_pushboolean(L, m.loaded); lua_setfield(L, -2, "loaded");
        lua_pushboolean(L, m.hidden); lua_setfield(L, -2, "hidden");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------
// Dev exec folder: drop a .lua file into H7ModEngine\exec\ and it runs on the next tick.
// Output (print/h7.log, return values and errors) goes to logs\engine.log; the file is renamed to .done.
// ---------------------------------------------------------------------------------------

static void RunExecFolder() {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(EnginePath(L"exec"), ec)) {
        if (e.path().extension() != ".lua") continue;
        fs::path file = e.path();
        lua_State* L = g_L;
        Log("exec: running %s", file.filename().string().c_str());
        lua_pushcfunction(L, Traceback);
        if (luaL_loadfile(L, file.string().c_str()) != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 1) != LUA_OK) {
            Log("exec: ERROR %s", lua_tostring(L, -1));
        } else {
            for (int i = 2; i <= lua_gettop(L); ++i) Log("exec: => %s", luaL_tolstring(L, i, nullptr)), lua_pop(L, 1);
        }
        lua_settop(L, 0);
        fs::path done = file;
        done.replace_extension(".done");
        fs::remove(done, ec);
        fs::rename(file, done, ec);
    }
}

// ---------------------------------------------------------------------------------------
// State lifetime and per-tick work
// ---------------------------------------------------------------------------------------

static int H_print(lua_State* L) {
    int n = lua_gettop(L);
    std::string s;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) s += "\t";
        s += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    int id = CurrentModId();
    Log("%s%s", id > 0 ? ("[" + ModName(id) + "] ").c_str() : "[lua] ", s.c_str());
    return 0;
}

static void CreateState() {
    g_L = luaL_newstate();
    luaL_openlibs(g_L);
    lua_newtable(g_L);
    lua_pushstring(g_L, kVersion);
    lua_setfield(g_L, -2, "version");
    const luaL_Reg fns[] = {
        {"on", H_on}, {"off", H_off}, {"after", H_after}, {"every", H_every}, {"cancel", H_cancel},
        {"reload", H_reload}, {"mods", H_mods}, {"set_mod_enabled", H_set_mod_enabled}, {nullptr, nullptr},
    };
    luaL_setfuncs(g_L, fns, 0);
    lua_setglobal(g_L, "h7");
    lua_register(g_L, "print", H_print);
    OpenBindings(g_L);
    // Shared Lua libraries for all mods: H7ModEngine\lib\?.lua
    std::string libPath = Narrow(EnginePath(L"lib")) + "\\?.lua";
    lua_getglobal(g_L, "package");
    lua_pushstring(g_L, libPath.c_str());
    lua_setfield(g_L, -2, "path");
    lua_pop(g_L, 1);
    Log("Lua %s.%s ready (H7ModEngine %s)", LUA_VERSION_MAJOR, LUA_VERSION_MINOR, kVersion);
}

// Reads an object-typed property (or element 0 of an array property) by name; 0 if missing.
static ue3::UObject ObjectProp(ue3::UObject obj, const char* name) {
    if (!obj || !ue3::IsValidObject(obj)) return 0;
    ue3::UObject prop = ue3::FindField(ue3::ObjClass(obj), name);
    if (!prop) return 0;
    uintptr_t at = obj + ue3::Describe(prop).offset;
    if (ue3::KindOf(prop) == ue3::PropKind::Array) {
        if (mem::ReadOr<int32_t>(at + 8, 0) <= 0) return 0;
        at = mem::ReadOr<uintptr_t>(at, 0);
    }
    return at ? mem::ReadOr<uintptr_t>(at, 0) : 0;
}

static std::string CurrentMapName(ue3::UObject viewport) {
    // The map is the outermost package of the local player's WorldInfo
    // (scanning for WorldInfo objects finds stale ones from loaded-but-inactive packages).
    // GamePlayers lives on the GameEngine, which is the viewport client's outer.
    ue3::UObject pc = ObjectProp(ObjectProp(ue3::ObjOuter(viewport), "GamePlayers"), "Actor");
    ue3::UObject top = ObjectProp(pc, "WorldInfo");
    while (ue3::ObjOuter(top)) top = ue3::ObjOuter(top);
    return top ? ue3::ObjName(top) : "";
}

void OnGameTick(float dt, uintptr_t viewport) {
    if (!g_L) {
        CreateState();
        DiscoverAndLoadMods();
    }
    g_clock += dt;

    static double nextSlowCheck = 0;
    if (g_clock >= nextSlowCheck) {
        nextSlowCheck = g_clock + 1.0;
        // Hot reload: reload a mod when any of its .lua files changed.
        for (auto& m : g_mods) {
            if (!m.loaded) continue;
            long long s = NewestLuaStamp(m.dir);
            if (s != m.stamp) {
                Log("Reloading mod %s (files changed)", m.name.c_str());
                UnloadMod(m);
                LoadMod(m);
            }
        }
        static std::string lastMap;
        std::string map = CurrentMapName(viewport);
        if (!map.empty() && map != lastMap) {
            lastMap = map;
            Log("Map: %s", map.c_str());
            lua_pushstring(g_L, map.c_str());
            EmitEvent("map_loaded", 1);
        }
        RunExecFolder();
    }

    if (g_toggled) {
        g_toggled = false;
        ApplyToggles();
    }

    if (g_reloadAll) {
        g_reloadAll = false;
        Log("Reloading all mods");
        for (auto& m : g_mods) UnloadMod(m);
        DiscoverAndLoadMods();
    }

    RunTimers();
    lua_pushnumber(g_L, dt);
    EmitEvent("tick", 1);
}

} // namespace h7::lua
