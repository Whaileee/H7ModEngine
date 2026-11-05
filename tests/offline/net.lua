-- Offline test for h7.json / h7.http / h7.net: "build.bat nettest" (or "build.bat nettest online").
-- JSON checks always run; HTTP and UPnP checks only with H7_NETTEST_ONLINE=1 ("build.bat nettest online").
local failures = 0
local function check(name, ok, detail)
    print(("[nettest] %-28s %s  %s"):format(name, ok and "OK  " or "FAIL", tostring(detail or "")))
    if not ok then failures = failures + 1 end
end
local function try(name, fn)
    local ok, v = pcall(fn)
    check(name, ok and v ~= false, ok and v or v)
end

local json = h7.json

-- Encoding
try("encode scalars", function()
    local s = json.encode({ a = 1, b = 1.5, c = "x", d = true, e = false })
    assert(s == '{"a":1,"b":1.5,"c":"x","d":true,"e":false}', s)
    return s
end)
try("encode array", function()
    local s = json.encode({ 1, "two", { 3 } })
    assert(s == '[1,"two",[3]]', s)
    return s
end)
try("encode empty", function()
    assert(json.encode({}) == "{}")
    assert(json.encode(json.array()) == "[]")
    return "{} and []"
end)
try("encode escapes", function()
    local s = json.encode("q\"b\\n\n\1é")
    assert(s == '"q\\"b\\\\n\\n\\u0001é"', s)
    return s
end)
try("encode null", function()
    local s = json.encode({ json.null, 2 })
    assert(s == "[null,2]", s)
    return s
end)
try("encode sorted keys", function()
    local s = json.encode({ z = 1, a = 2, m = 3 })
    assert(s == '{"a":2,"m":3,"z":1}', s)
    return s
end)
try("encode number keys", function()
    local s = json.encode({ [5] = "x", y = 1 })
    assert(s == '{"5":"x","y":1}', s)
    return s
end)
try("encode rejects nan", function()
    local ok = pcall(json.encode, 0 / 0)
    assert(not ok)
    return "error raised"
end)
try("encode rejects cycles", function()
    local t = {}
    t.self = t
    local ok, err = pcall(json.encode, t)
    assert(not ok, "no error")
    return err
end)
try("encode pretty", function()
    local s = json.encode({ a = { 1, 2 } }, true)
    assert(s:find("\n") and json.decode(s).a[2] == 2, s)
    return (s:gsub("\n", "\\n"))
end)

-- Decoding
try("decode object", function()
    local t = json.decode(' { "a" : 1, "b": [true, false, null], "c": {"d": "e"}, "f": -2.5e1 } ')
    assert(t.a == 1 and math.type(t.a) == "integer")
    assert(t.b[1] == true and t.b[2] == false and t.b[3] == json.null)
    assert(t.c.d == "e" and t.f == -25.0)
    return "ok"
end)
try("decode unicode", function()
    local t = json.decode('"\\u00e9\\ud83d\\ude00\\n"')
    assert(t == "é😀\n", t)
    return t
end)
try("decode big int", function()
    local v = json.decode("9007199254740993")
    assert(math.type(v) == "integer" and v == 9007199254740993)
    return v
end)
try("decode errors", function()
    for _, bad in ipairs({ "", "{", "[1,]", '{"a"}', "tru", '"abc', "1 2", "{'a':1}" }) do
        local v, err = json.decode(bad)
        assert(v == nil and type(err) == "string", "accepted: " .. bad)
    end
    return select(2, json.decode("[1,]"))
end)
try("round trip", function()
    local src = { players = { { name = "Ana", slot = 1 }, { name = "Bo", slot = 2 } }, map = "Duel", empty = json.array() }
    local back = json.decode(json.encode(src))
    assert(back.players[2].name == "Bo" and back.map == "Duel" and #back.empty == 0)
    assert(json.encode(back) == json.encode(src))
    return json.encode(back)
end)

-- Network
local online = os.getenv("H7_NETTEST_ONLINE") == "1"
check("local_ip", h7.net.local_ip() ~= nil, h7.net.local_ip())

try("bad url rejected", function()
    local ok, err = pcall(h7.http.get, "ftp://x", function() end)
    assert(not ok)
    return err
end)

-- Checks run one after another (each starts the next from its callback).
local steps = {}
local function nextStep()
    local step = table.remove(steps, 1)
    if step then step() else done(failures) end
end

-- Game content scan (local disk; on CI the folder doesn't exist, which must read as "unmodded").
steps[#steps + 1] = function()
    h7.game_content(function(c)
        local okShape = type(c.packs) == "table" and type(c.modded) == "boolean" and c.modded == (#c.packs > 0)
            and (c.modded == (c.fingerprint ~= ""))
        for _, p in ipairs(c.packs) do
            okShape = okShape and type(p.name) == "string" and p.files > 0 and #p.version == 8
        end
        check("game_content", okShape, ("%s, %d pack(s), fingerprint %s"):format(c.modded and "modded" or "unmodded",
            #c.packs, c.fingerprint ~= "" and c.fingerprint or "-"))
        for i = 1, math.min(#c.packs, 6) do
            print(("[nettest]   %s (%d files) %s"):format(c.packs[i].name, c.packs[i].files, c.packs[i].version))
        end
        -- A second call returns the cached scan.
        h7.game_content(function(c2)
            check("game_content cached", c2.fingerprint == c.fingerprint and #c2.packs == #c.packs)
            nextStep()
        end)
    end)
end

if not online then
    print("[nettest] (online checks skipped; run with 'online' to include HTTP and UPnP)")
    nextStep()
    return
end

steps[#steps + 1] = function()
    h7.http.get("https://postman-echo.com/get?x=1", function(res)
        local body = res.ok and json.decode(res.body)
        check("http get", res.ok and res.status == 200 and body and body.args.x == "1" and body.headers["x-test"] == "yes",
            res.error or (res.status .. " " .. tostring(res.headers["content-type"])))
        nextStep()
    end, { ["X-Test"] = "yes" })
end
steps[#steps + 1] = function()
    h7.http.post("https://postman-echo.com/post", { hello = "world", n = 3 }, function(res)
        local body = res.ok and json.decode(res.body)
        check("http post json", res.ok and res.status == 200 and body and body.json and body.json.hello == "world",
            res.error or res.status)
        nextStep()
    end)
end
steps[#steps + 1] = function()
    h7.http.request({ url = "https://postman-echo.com/status/404", method = "get", timeout = 15 }, function(res)
        check("http status 404", res.ok and res.status == 404, res.error or res.status)
        nextStep()
    end)
end
steps[#steps + 1] = function()
    h7.http.get("https://nonexistent.invalid/", function(res)
        check("http dns failure", not res.ok and res.error ~= nil, res.error)
        nextStep()
    end)
end
steps[#steps + 1] = function()
    -- A callback whose mod unloads (or that is cancelled) before the response arrives must never run.
    h7.http.get("https://postman-echo.com/get?dropped=1", function() check("unloaded callback ran", false) end)
    unload_mod(1)
    local c = h7.http.get("https://postman-echo.com/get?cancelled=1", function() check("cancelled callback ran", false) end)
    h7.http.cancel(c)
    h7.http.get("https://postman-echo.com/get?after=1", function(res)
        check("unload/cancel drop callbacks", res.ok and res.status == 200, res.error or "later request still works")
        nextStep()
    end)
end
steps[#steps + 1] = function()
    h7.net.upnp_add(27999, "udp", function(r)
        -- Without a UPnP router this fails with a clear error; both outcomes pass, the log shows which.
        check("upnp add", r.ok or r.error ~= nil, r.ok and ("external " .. tostring(r.external_ip) .. ", internal " .. tostring(r.internal_ip)) or r.error)
        if not r.ok then return nextStep() end
        h7.net.upnp_remove(27999, "UDP", function(r2)
            check("upnp remove", r2.ok, r2.error)
            nextStep()
        end)
    end, "H7ModEngine test")
end
nextStep()
