#pragma once
#include <string>
#include <cstdint>

struct lua_State;

namespace h7::lua {

// --- runtime.cpp: lifetime, mods, events (game thread only) -------------------------
void OnGameTick(float deltaSeconds, uintptr_t viewport); // driven by the viewport PostRender hook
void EmitEvent(const char* name, int nargs); // pops nargs values pushed on g_L
lua_State* State();
const char* Version();                                   // kVersion, e.g. "0.1.1"

// Ownership tracking so a mod's hooks/handlers/timers are removed when it unloads.
int CurrentModId();
int SetCurrentModId(int id); // returns the previous id
const std::string& ModName(int id);

// Makes `id` the current mod while in scope: logs, hooks, handlers and timers created meanwhile belong to it.
struct ModScope {
    int saved;
    explicit ModScope(int id) : saved(SetCurrentModId(id)) {}
    ~ModScope() { SetCurrentModId(saved); }
};
void Report(const std::string& where, const std::string& error); // logs a Lua error with mod context

// --- bindings.cpp ------------------------------------------------------------------
void OpenBindings(lua_State* L);            // registers the global `h7` table and metatables
void RemoveModHooks(int modId);
void PushObject(lua_State* L, uintptr_t obj);
int Traceback(lua_State* L);                // message handler for lua_pcall

// --- netlib.cpp: h7.http / h7.json / h7.net ------------------------------------------
void OpenNet(lua_State* L);                 // adds h7.http, h7.json, h7.net (after OpenBindings)
int NetSetLanHost(lua_State* L);            // h7.net.set_lan_host, in bindings.cpp (needs struct proxies)
void PumpNet(lua_State* L);                 // delivers finished requests to their callbacks (each tick)
void RemoveModNet(lua_State* L, int modId); // drops a mod's pending callbacks on unload

// --- devlib.cpp: h7.dev (raw memory access for engine development) ---------------------
void OpenDev(lua_State* L);

} // namespace h7::lua
