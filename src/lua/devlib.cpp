// h7.dev: raw memory access for engine development (probing native layouts from Lua before they get a proper
// API). Not for mods: addresses are build-specific and a wrong write crashes the game. Reads and writes are
// SEH-guarded, so a bad address returns nil/false instead of crashing.
#include "runtime.h"
#include "../core/memory.h"
#include "../ue3/reflection.h"
#include <lua.h>
#include <lauxlib.h>
#include <cstring>
#include <string>

namespace h7::lua {

static uintptr_t CheckAddr(lua_State* L, int idx) {
    lua_Integer a = luaL_checkinteger(L, idx);
    if (a == 0) luaL_error(L, "address is 0");
    return (uintptr_t)a;
}

// h7.dev.read(address, size) -> string (raw bytes) or nil if unreadable
static int D_read(lua_State* L) {
    uintptr_t a = CheckAddr(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n < 0 || n > 65536) luaL_error(L, "size must be 0..65536");
    std::string buf((size_t)n, '\0');
    if (!mem::Read((const void*)a, buf.data(), (size_t)n)) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf.data(), buf.size());
    return 1;
}

// h7.dev.write(address, bytes) -> bool
static int D_write(lua_State* L) {
    uintptr_t a = CheckAddr(L, 1);
    size_t n;
    const char* s = luaL_checklstring(L, 2, &n);
    lua_pushboolean(L, mem::Write((void*)a, s, n));
    return 1;
}

// h7.dev.u64(address) / h7.dev.u32(address) -> integer or nil
static int D_u64(lua_State* L) {
    uint64_t v;
    if (!mem::Read(CheckAddr(L, 1), v)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
static int D_u32(lua_State* L) {
    uint32_t v;
    if (!mem::Read(CheckAddr(L, 1), v)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

// h7.dev.alloc(size) -> address of zeroed memory from the engine's allocator (the engine may free it)
static int D_alloc(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0 || n > 65536) luaL_error(L, "size must be 1..65536");
    void* p = ue3::EngineRealloc(nullptr, (size_t)n);
    if (!p) luaL_error(L, "allocation failed");
    memset(p, 0, (size_t)n);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)p);
    return 1;
}

// h7.dev.game_base() -> base address of the game executable (for RVAs)
static int D_game_base(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)mem::g_game.base);
    return 1;
}

void OpenDev(lua_State* L) {
    lua_getglobal(L, "h7");
    lua_newtable(L);
    const luaL_Reg fns[] = {
        {"read", D_read}, {"write", D_write}, {"u64", D_u64}, {"u32", D_u32}, {"alloc", D_alloc},
        {"game_base", D_game_base}, {nullptr, nullptr},
    };
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "dev");
    lua_pop(L, 1);
}

} // namespace h7::lua
