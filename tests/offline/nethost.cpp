// Offline test host for src/lua/netlib.cpp + src/net/*: a bare Lua state with h7.http / h7.json / h7.net and
// stubs for the runtime functions netlib uses. Runs a Lua script, pumping network results until it calls done().
//   build.bat nettest [online]   (builds build/nettest/nethost.exe and runs tests/offline/net.lua)
#include "../../src/lua/runtime.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace h7 {
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
} // namespace h7

namespace h7::lua {
static int g_mod = 1;
static std::string g_name = "test";
int CurrentModId() { return g_mod; }
int SetCurrentModId(int id) { int old = g_mod; g_mod = id; return old; }
const std::string& ModName(int) { return g_name; }
const char* Version() { return "test"; }
void Report(const std::string& where, const std::string& error) { printf("ERROR in %s: %s\n", where.c_str(), error.c_str()); }
int NetSetLanHost(lua_State* L) { return luaL_error(L, "set_lan_host needs the game"); }
int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}
} // namespace h7::lua

static bool g_done = false;
static int g_failures = 0;
static int Done(lua_State* L) { g_done = true; g_failures = (int)luaL_optinteger(L, 1, 0); return 0; }
static int Unload(lua_State* L) { h7::lua::RemoveModNet(L, (int)luaL_checkinteger(L, 1)); return 0; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: nethost <script.lua>\n"); return 2; }
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    lua_setglobal(L, "h7");
    h7::lua::OpenNet(L);
    lua_register(L, "done", Done);
    lua_register(L, "unload_mod", Unload); // simulates a mod unload: drops the mod's pending callbacks
    if (luaL_dofile(L, argv[1]) != LUA_OK) { printf("ERROR: %s\n", lua_tostring(L, -1)); return 1; }
    for (int ms = 0; !g_done && ms < 60000; ms += 10) {
        Sleep(10);
        h7::lua::PumpNet(L);
    }
    if (!g_done) { printf("TIMEOUT\n"); return 1; }
    printf(g_failures ? "FAILED (%d)\n" : "ALL OK\n", g_failures);
    return g_failures ? 1 : 0;
}
