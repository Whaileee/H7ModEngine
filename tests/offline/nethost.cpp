// Offline test host for src/lua/netlib.cpp + src/net/*: a bare Lua state with h7.http / h7.json / h7.net /
// h7.crypto and stubs for the runtime functions netlib uses. Runs a Lua script, pumping network results until it
// calls done(). The challenge responder listens on 127.0.0.1 only here (no firewall prompt).
//   build.bat nettest [online]   (builds build/nettest/nethost.exe and runs tests/offline/net.lua)
#include "../../src/lua/runtime.h"
#include "../../src/net/responder.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <winsock2.h>
#include <ws2tcpip.h>
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

// test.challenge(port, line, half_close?) -> reply (possibly ""), elapsed ms | nil, error. A blocking loopback TCP client like
// the lobby's: connects, sends `line`, optionally ends its side, and returns everything received until the
// responder closes (at most 3 s).
static int Challenge(lua_State* L) {
    int port = (int)luaL_checkinteger(L, 1);
    size_t n;
    const char* line = luaL_checklstring(L, 2, &n);
    bool halfClose = lua_toboolean(L, 3);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        lua_pushnil(L);
        lua_pushfstring(L, "connect failed (error %d)", e);
        return 2;
    }
    if (n) send(s, line, (int)n, 0);
    if (halfClose) shutdown(s, SD_SEND);
    std::string reply;
    ULONGLONG start = GetTickCount64(), deadline = start + 3000;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        int ms = (int)(deadline - now);
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        if (select(0, &r, nullptr, nullptr, &tv) != 1) break;
        char buf[256];
        int got = recv(s, buf, sizeof buf, 0);
        if (got <= 0) break;
        reply.append(buf, got);
    }
    closesocket(s);
    lua_pushlstring(L, reply.data(), reply.size());
    lua_pushinteger(L, (lua_Integer)(GetTickCount64() - start));
    return 2;
}

// test.occupy(port) -> ok: holds a loopback TCP listener on `port` until exit (a "busy port" for the responder).
static int Occupy(lua_State* L) {
    int port = (int)luaL_checkinteger(L, 1);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(s, (sockaddr*)&a, sizeof a) == 0 && listen(s, 1) == 0;
    if (!ok) closesocket(s);
    lua_pushboolean(L, ok);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: nethost <script.lua>\n"); return 2; }
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    lua_setglobal(L, "h7");
    h7::lua::OpenNet(L);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    h7::net::ResponderLoopbackOnly(true);
    const luaL_Reg test[] = {{"challenge", Challenge}, {"occupy", Occupy}, {nullptr, nullptr}};
    luaL_newlib(L, test);
    lua_setglobal(L, "test");
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
