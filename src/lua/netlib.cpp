// h7.http, h7.json and h7.net: networking for mods.
//
// Blocking work (HTTP, UPnP) runs on a small worker pool. Results are queued and delivered to the mod's callback
// on the game thread from the tick (PumpNet), inside the mod's scope. Unloading a mod drops its pending
// callbacks; the request itself still finishes on the worker and its result is discarded.
#include "runtime.h"
#include "../core/log.h"
#include "../net/http.h"
#include "../net/upnp.h"
#include <lua.h>
#include <lauxlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace h7::lua {

// ---------------------------------------------------------------------------------------
// Worker pool and result delivery
// ---------------------------------------------------------------------------------------

using Pusher = std::function<void(lua_State*)>; // pushes exactly one value (the callback's argument)

struct Pending {
    int mod;
    int ref; // callback, or LUA_NOREF
};

static std::map<int, Pending> g_pending; // game thread only
static int g_nextJob = 1;

static std::mutex g_mx;
static std::condition_variable g_cv;
static std::deque<std::function<void()>> g_tasks;
static std::vector<std::pair<int, Pusher>> g_done;
static int g_workers = 0;
static constexpr int kMaxWorkers = 4;

static void WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(g_mx);
            g_cv.wait(lock, [] { return !g_tasks.empty(); });
            task = std::move(g_tasks.front());
            g_tasks.pop_front();
        }
        try {
            task();
        } catch (...) {
            Log("net: a worker task threw an exception");
        }
    }
}

// Starts `work` on a worker; its returned pusher is handed to the callback registered under the returned id.
static int Submit(lua_State* L, int cbIndex, std::function<Pusher()> work) {
    int id = g_nextJob++;
    int ref = LUA_NOREF;
    if (cbIndex && !lua_isnoneornil(L, cbIndex)) {
        luaL_checktype(L, cbIndex, LUA_TFUNCTION);
        lua_pushvalue(L, cbIndex);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    g_pending[id] = {CurrentModId(), ref};
    {
        std::lock_guard lock(g_mx);
        g_tasks.push_back([id, work = std::move(work)] {
            Pusher p;
            try {
                p = work();
            } catch (const std::exception& e) {
                std::string msg = e.what();
                p = [msg](lua_State* L) {
                    lua_newtable(L);
                    lua_pushboolean(L, 0); lua_setfield(L, -2, "ok");
                    lua_pushstring(L, msg.c_str()); lua_setfield(L, -2, "error");
                };
            }
            std::lock_guard lock(g_mx);
            g_done.emplace_back(id, std::move(p));
        });
        if (g_workers < kMaxWorkers && g_tasks.size() > 0) {
            ++g_workers;
            std::thread(WorkerLoop).detach();
        }
    }
    g_cv.notify_one();
    return id;
}

void PumpNet(lua_State* L) {
    std::vector<std::pair<int, Pusher>> done;
    {
        std::lock_guard lock(g_mx);
        if (g_done.empty()) return;
        done.swap(g_done);
    }
    for (auto& [id, push] : done) {
        auto it = g_pending.find(id);
        if (it == g_pending.end()) continue; // cancelled or its mod unloaded
        Pending p = it->second;
        g_pending.erase(it);
        if (p.ref == LUA_NOREF) continue;
        ModScope scope(p.mod);
        int base = lua_gettop(L);
        lua_pushcfunction(L, Traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, p.ref);
        luaL_unref(L, LUA_REGISTRYINDEX, p.ref);
        push(L);
        if (lua_pcall(L, 1, 0, base + 1) != LUA_OK) Report("network callback (" + ModName(p.mod) + ")", lua_tostring(L, -1));
        lua_settop(L, base);
    }
}

void RemoveModNet(lua_State* L, int modId) {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (it->second.mod != modId) { ++it; continue; }
        if (it->second.ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
        it = g_pending.erase(it);
    }
}

// ---------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------

static const char* kArrayMeta = "h7.json.array";
static int g_nullRef = LUA_NOREF; // h7.json.null sentinel (a unique empty table)

static void PushNull(lua_State* L) { lua_rawgeti(L, LUA_REGISTRYINDEX, g_nullRef); }
static bool IsNull(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return false;
    PushNull(L);
    bool eq = lua_rawequal(L, idx < 0 ? idx - 1 : idx, -1);
    lua_pop(L, 1);
    return eq;
}

static void EncodeString(const char* s, size_t n, std::string& out) {
    out += '"';
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c; // UTF-8 passes through
            }
        }
    }
    out += '"';
}

static void Indent(std::string& out, int pretty, int depth) {
    if (!pretty) return;
    out += '\n';
    out.append((size_t)depth * pretty, ' ');
}

static void Encode(lua_State* L, int idx, std::string& out, int depth, int pretty);

// A table is an array if it is marked with h7.json.array, or if its keys are exactly 1..n (n > 0).
static bool IsArray(lua_State* L, int idx, lua_Integer& n) {
    n = (lua_Integer)lua_rawlen(L, idx);
    if (lua_getmetatable(L, idx)) {
        luaL_getmetatable(L, kArrayMeta);
        bool marked = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
        if (marked) return true;
    }
    if (n == 0) return false;
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1)) { lua_pop(L, 1); return false; }
        ++count;
    }
    return count == n;
}

static void Encode(lua_State* L, int idx, std::string& out, int depth, int pretty) {
    idx = lua_absindex(L, idx);
    if (depth > 100) luaL_error(L, "json.encode: nesting deeper than 100 (cyclic table?)");
    luaL_checkstack(L, 8, "json.encode"); // each level keeps a key, a value and metatables on the stack
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "null"; return;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return;
    case LUA_TNUMBER: {
        char buf[40];
        if (lua_isinteger(L, idx)) {
            snprintf(buf, sizeof buf, "%lld", (long long)lua_tointeger(L, idx));
        } else {
            double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) luaL_error(L, "json.encode: cannot encode %s", std::isnan(d) ? "NaN" : "infinity");
            snprintf(buf, sizeof buf, "%.17g", d);
        }
        out += buf;
        return;
    }
    case LUA_TSTRING: {
        size_t n;
        const char* s = lua_tolstring(L, idx, &n);
        EncodeString(s, n, out);
        return;
    }
    case LUA_TTABLE: break;
    default: luaL_error(L, "json.encode: cannot encode a %s", luaL_typename(L, idx));
    }
    if (IsNull(L, idx)) { out += "null"; return; }

    lua_Integer n;
    if (IsArray(L, idx, n)) {
        out += '[';
        for (lua_Integer i = 1; i <= n; ++i) {
            if (i > 1) out += ',';
            Indent(out, pretty, depth + 1);
            lua_rawgeti(L, idx, i);
            Encode(L, -1, out, depth + 1, pretty);
            lua_pop(L, 1);
        }
        if (n) Indent(out, pretty, depth);
        out += ']';
        return;
    }
    // Object: keys sorted so the output is deterministic (stable hashes, readable diffs).
    std::vector<std::string> keys;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            keys.emplace_back(lua_tostring(L, -1));
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            lua_pushvalue(L, -1);
            keys.emplace_back(lua_tostring(L, -1)); // number keys become strings
            lua_pop(L, 1);
        } else {
            luaL_error(L, "json.encode: object keys must be strings or numbers, got %s", luaL_typename(L, -1));
        }
    }
    std::sort(keys.begin(), keys.end());
    out += '{';
    bool first = true;
    for (auto& k : keys) {
        if (!first) out += ',';
        first = false;
        Indent(out, pretty, depth + 1);
        EncodeString(k.data(), k.size(), out);
        out += pretty ? ": " : ":";
        lua_getfield(L, idx, k.c_str());
        if (lua_isnil(L, -1)) { // key was a number
            lua_pop(L, 1);
            lua_pushstring(L, k.c_str());
            lua_Number num = lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_Integer ki;
            if (lua_numbertointeger(num, &ki) && (lua_Number)ki == num) lua_rawgeti(L, idx, ki);
            else { lua_pushnumber(L, num); lua_rawget(L, idx); }
        }
        Encode(L, -1, out, depth + 1, pretty);
        lua_pop(L, 1);
    }
    if (!keys.empty()) Indent(out, pretty, depth);
    out += '}';
}

struct Decoder {
    lua_State* L;
    const char* s;
    size_t n, i = 0;
    std::string err;

    bool Fail(const char* what) {
        if (err.empty()) {
            char buf[120];
            snprintf(buf, sizeof buf, "%s at position %zu", what, i + 1);
            err = buf;
        }
        return false;
    }
    void Ws() { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool Lit(const char* w) {
        size_t len = strlen(w);
        if (n - i < len || memcmp(s + i, w, len) != 0) return Fail("invalid literal");
        i += len;
        return true;
    }
    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(unsigned& v) {
        if (n - i < 4) return Fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return Fail("invalid \\u escape");
        }
        return true;
    }
    bool String(std::string& out) {
        ++i; // opening quote
        while (i < n) {
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return Fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= n) break;
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && n - i >= 6 && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { Utf8(out, 0xFFFD); cp = lo; }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                Utf8(out, cp);
                break;
            }
            default: return Fail("invalid escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Number() {
        size_t start = i;
        bool isFloat = false;
        if (s[i] == '-') ++i;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isFloat = true;
            ++i;
        }
        std::string text(s + start, i - start);
        char* end = nullptr;
        if (!isFloat) {
            errno = 0;
            long long v = strtoll(text.c_str(), &end, 10);
            if (end && *end == '\0' && errno == 0 && text != "-") { lua_pushinteger(L, v); return true; }
        }
        double d = strtod(text.c_str(), &end);
        if (!end || *end != '\0' || text.empty()) return Fail("invalid number");
        lua_pushnumber(L, d);
        return true;
    }
    bool Value(int depth) {
        if (depth > 200) return Fail("nesting too deep");
        luaL_checkstack(L, 4, "json.decode");
        Ws();
        if (i >= n) return Fail("unexpected end of input");
        char c = s[i];
        if (c == '{') {
            ++i;
            lua_newtable(L);
            Ws();
            if (i < n && s[i] == '}') { ++i; return true; }
            for (;;) {
                Ws();
                if (i >= n || s[i] != '"') return Fail("expected a string key");
                std::string key;
                if (!String(key)) return false;
                Ws();
                if (i >= n || s[i] != ':') return Fail("expected ':'");
                ++i;
                if (!Value(depth + 1)) return false;
                lua_setfield(L, -2, key.c_str());
                Ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            lua_newtable(L);
            luaL_setmetatable(L, kArrayMeta); // so an empty array encodes back as []
            Ws();
            if (i < n && s[i] == ']') { ++i; return true; }
            for (lua_Integer k = 1;; ++k) {
                if (!Value(depth + 1)) return false;
                lua_rawseti(L, -2, k);
                Ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string str;
            if (!String(str)) return false;
            lua_pushlstring(L, str.data(), str.size());
            return true;
        }
        if (c == 't') { if (!Lit("true")) return false; lua_pushboolean(L, 1); return true; }
        if (c == 'f') { if (!Lit("false")) return false; lua_pushboolean(L, 0); return true; }
        if (c == 'n') { if (!Lit("null")) return false; PushNull(L); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return Number();
        return Fail("unexpected character");
    }
};

static std::string EncodeJson(lua_State* L, int idx, int pretty) {
    std::string out;
    Encode(L, idx, out, 0, pretty);
    return out;
}

static int J_encode(lua_State* L) {
    luaL_checkany(L, 1);
    int pretty = lua_toboolean(L, 2) ? 2 : 0;
    std::string out = EncodeJson(L, 1, pretty);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int J_decode(lua_State* L) {
    size_t n;
    const char* s = luaL_checklstring(L, 1, &n);
    int top = lua_gettop(L);
    Decoder d{L, s, n};
    bool ok = d.Value(0);
    if (ok) {
        d.Ws();
        if (d.i != n) ok = d.Fail("trailing characters");
    }
    if (!ok) {
        lua_settop(L, top);
        lua_pushnil(L);
        lua_pushstring(L, d.err.c_str());
        return 2;
    }
    return 1;
}

static int J_array(lua_State* L) {
    if (lua_isnoneornil(L, 1)) lua_newtable(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    luaL_setmetatable(L, kArrayMeta);
    return 1;
}

// ---------------------------------------------------------------------------------------
// h7.http
// ---------------------------------------------------------------------------------------

static std::string UserAgent() { return std::string("H7ModEngine/") + Version(); }

static void PushResponse(lua_State* L, const net::HttpResponse& r) {
    lua_newtable(L);
    lua_pushboolean(L, r.ok); lua_setfield(L, -2, "ok");
    lua_pushinteger(L, r.status); lua_setfield(L, -2, "status");
    lua_pushlstring(L, r.body.data(), r.body.size()); lua_setfield(L, -2, "body");
    lua_newtable(L);
    for (auto& [k, v] : r.headers) {
        // Repeated headers are joined with ", " (RFC 9110), except Set-Cookie which keeps the last value.
        lua_getfield(L, -1, k.c_str());
        if (lua_isstring(L, -1) && k != "set-cookie") {
            std::string joined = std::string(lua_tostring(L, -1)) + ", " + v;
            lua_pop(L, 1);
            lua_pushstring(L, joined.c_str());
        } else {
            lua_pop(L, 1);
            lua_pushstring(L, v.c_str());
        }
        lua_setfield(L, -2, k.c_str());
    }
    lua_setfield(L, -2, "headers");
    if (!r.error.empty()) { lua_pushstring(L, r.error.c_str()); lua_setfield(L, -2, "error"); }
}

static void ReadHeaders(lua_State* L, int idx, net::HttpRequest& req) {
    if (lua_isnoneornil(L, idx)) return;
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1))
            req.headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static bool HasHeader(const net::HttpRequest& req, const char* name) {
    for (auto& [k, v] : req.headers)
        if (_stricmp(k.c_str(), name) == 0) return true;
    return false;
}

// A table body is sent as JSON.
static void ReadBody(lua_State* L, int idx, net::HttpRequest& req) {
    if (lua_isnoneornil(L, idx)) return;
    if (lua_istable(L, idx)) {
        req.body = EncodeJson(L, idx, 0);
        if (!HasHeader(req, "Content-Type")) req.headers.emplace_back("Content-Type", "application/json");
    } else {
        size_t n;
        const char* s = luaL_checklstring(L, idx, &n);
        req.body.assign(s, n);
    }
}

static int StartRequest(lua_State* L, net::HttpRequest req, int cbIndex) {
    if (req.url.rfind("http://", 0) != 0 && req.url.rfind("https://", 0) != 0)
        luaL_error(L, "http: URL must start with http:// or https:// (got '%s')", req.url.c_str());
    std::string ua = UserAgent();
    int id = Submit(L, cbIndex, [req = std::move(req), ua]() -> Pusher {
        auto res = std::make_shared<net::HttpResponse>(net::HttpSend(req, ua));
        return [res](lua_State* L) { PushResponse(L, *res); };
    });
    lua_pushinteger(L, id);
    return 1;
}

// h7.http.request{url=, method=, headers=, body=, timeout=}, fn
static int Http_request(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    net::HttpRequest req;
    lua_getfield(L, 1, "url");
    req.url = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "method");
    if (!lua_isnil(L, -1)) req.method = luaL_checkstring(L, -1);
    for (auto& c : req.method) c = (char)toupper((unsigned char)c);
    lua_getfield(L, 1, "timeout");
    if (!lua_isnil(L, -1)) req.timeoutMs = (int)(luaL_checknumber(L, -1) * 1000);
    lua_getfield(L, 1, "headers");
    ReadHeaders(L, lua_gettop(L), req);
    lua_getfield(L, 1, "body");
    ReadBody(L, lua_gettop(L), req);
    lua_settop(L, 2);
    return StartRequest(L, std::move(req), 2);
}

// h7.http.get(url, fn, headers?)
static int Http_get(lua_State* L) {
    net::HttpRequest req;
    req.url = luaL_checkstring(L, 1);
    ReadHeaders(L, 3, req);
    return StartRequest(L, std::move(req), 2);
}

// h7.http.post(url, body, fn, headers?)
static int Http_post(lua_State* L) {
    net::HttpRequest req;
    req.method = "POST";
    req.url = luaL_checkstring(L, 1);
    ReadHeaders(L, 4, req);
    ReadBody(L, 2, req);
    return StartRequest(L, std::move(req), 3);
}

static int Http_cancel(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    auto it = g_pending.find(id);
    if (it != g_pending.end()) {
        if (it->second.ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
        g_pending.erase(it);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------
// h7.net
// ---------------------------------------------------------------------------------------

static void PushUpnp(lua_State* L, const net::UpnpResult& r) {
    lua_newtable(L);
    lua_pushboolean(L, r.ok); lua_setfield(L, -2, "ok");
    if (!r.externalIp.empty()) { lua_pushstring(L, r.externalIp.c_str()); lua_setfield(L, -2, "external_ip"); }
    if (!r.internalIp.empty()) { lua_pushstring(L, r.internalIp.c_str()); lua_setfield(L, -2, "internal_ip"); }
    if (!r.error.empty()) { lua_pushstring(L, r.error.c_str()); lua_setfield(L, -2, "error"); }
}

static std::string CheckProtocol(lua_State* L, int idx) {
    std::string p = luaL_checkstring(L, idx);
    for (auto& c : p) c = (char)toupper((unsigned char)c);
    if (p != "UDP" && p != "TCP") luaL_error(L, "protocol must be \"UDP\" or \"TCP\" (got '%s')", p.c_str());
    return p;
}

static int CheckPort(lua_State* L, int idx) {
    lua_Integer p = luaL_checkinteger(L, idx);
    if (p < 1 || p > 65535) luaL_error(L, "port must be 1..65535 (got %d)", (int)p);
    return (int)p;
}

// h7.net.upnp_add(port, protocol, fn?, description?)
static int Net_upnp_add(lua_State* L) {
    int port = CheckPort(L, 1);
    std::string proto = CheckProtocol(L, 2);
    std::string desc = luaL_optstring(L, 4, "Heroes VII (H7ModEngine)");
    int id = Submit(L, 3, [=]() -> Pusher {
        auto r = std::make_shared<net::UpnpResult>(net::UpnpAdd(port, port, proto, desc));
        Log("UPnP: map %s %d -> %s: %s", proto.c_str(), port, r->internalIp.c_str(), r->ok ? "ok" : r->error.c_str());
        return [r](lua_State* L) { PushUpnp(L, *r); };
    });
    lua_pushinteger(L, id);
    return 1;
}

// h7.net.upnp_remove(port, protocol, fn?)
static int Net_upnp_remove(lua_State* L) {
    int port = CheckPort(L, 1);
    std::string proto = CheckProtocol(L, 2);
    int id = Submit(L, 3, [=]() -> Pusher {
        auto r = std::make_shared<net::UpnpResult>(net::UpnpRemove(port, proto));
        Log("UPnP: unmap %s %d: %s", proto.c_str(), port, r->ok ? "ok" : r->error.c_str());
        return [r](lua_State* L) { PushUpnp(L, *r); };
    });
    lua_pushinteger(L, id);
    return 1;
}

static int Net_local_ip(lua_State* L) {
    std::string ip = net::LocalIp();
    if (ip.empty()) lua_pushnil(L);
    else lua_pushstring(L, ip.c_str());
    return 1;
}

// ---------------------------------------------------------------------------------------

static void SetSub(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, name);
}

void OpenNet(lua_State* L) {
    luaL_newmetatable(L, kArrayMeta);
    lua_pop(L, 1);
    lua_newtable(L);
    g_nullRef = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_getglobal(L, "h7");
    const luaL_Reg http[] = {
        {"request", Http_request}, {"get", Http_get}, {"post", Http_post}, {"cancel", Http_cancel}, {nullptr, nullptr},
    };
    SetSub(L, "http", http);
    const luaL_Reg json[] = {{"encode", J_encode}, {"decode", J_decode}, {"array", J_array}, {nullptr, nullptr}};
    SetSub(L, "json", json);
    lua_getfield(L, -1, "json");
    PushNull(L);
    lua_setfield(L, -2, "null");
    lua_pop(L, 1);
    const luaL_Reg netf[] = {
        {"upnp_add", Net_upnp_add}, {"upnp_remove", Net_upnp_remove}, {"local_ip", Net_local_ip}, {nullptr, nullptr},
    };
    SetSub(L, "net", netf);
    lua_pop(L, 1);
}

} // namespace h7::lua
