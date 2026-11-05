-- Engine self-test: copy into <Game>\H7ModEngine\exec\ while the game runs; results go to logs\engine.log.
local function check(name, fn)
    local ok, v = pcall(fn)
    h7.log(("[selftest] %-22s %s  %s"):format(name, ok and "OK  " or "FAIL", tostring(v)))
    return ok and v
end

local Actor = check("find_class", function() return h7.find_class("Engine.Actor") end)
check("class default", function() return assert(Actor:get_default(), "no CDO") end)
check("super chain", function() return Actor:get_super() end)
check("enum", function() return h7.enum("EPhysics").PHYS_Walking end)
check("properties", function() return #h7.properties("Engine.Actor") .. " props" end)
check("property type_name", function()
    for _, p in ipairs(h7.properties("Engine.Actor")) do
        if p.name == "Location" then assert(p.type_name == "Vector", tostring(p.type_name)) return p.type_name end
    end
    error("Location not found")
end)
check("functions", function() return #h7.functions("Engine.Actor") .. " functions" end)
local vp = check("viewport", function() return h7.first("GameViewportClient") end)
local pc = check("player controller", function() return h7.first("PlayerController") end)
check("read float", function() return pc.Location.X end)
check("read bool", function() return pc.bHidden end)
check("read struct table", function() local t = h7.to_table(pc.Location); return t.X .. "," .. t.Y .. "," .. t.Z end)
check("read object", function() return pc.WorldInfo end)
check("read string", function() return pc.PlayerReplicationInfo and pc.PlayerReplicationInfo.PlayerName end)
check("read array", function() return #pc.Attached end)
check("is_a", function() return pc:is_a("Actor") and not pc:is_a("Pawn") end)
check("write bool", function() local old = pc.bHidden; pc.bHidden = old; return pc.bHidden == old end)
check("find_all", function() return #h7.find_all("Engine.Actor") .. " actors" end)

-- Writing arrays from Lua tables (main menu only: uses the main menu's movie controller).
local cntl = h7.first("H7MainMenuCntl")
if cntl then
    check("write array", function()
        local old = h7.to_table(cntl.mLocaSection)
        cntl.mLocaSection = { "A", "B" }
        local got = #cntl.mLocaSection .. ":" .. cntl.mLocaSection[2]
        cntl.mLocaSection = old
        assert(got == "2:B", got)
        assert(#cntl.mLocaSection == #old, "not restored")
        return got
    end)
    check("array argument", function()
        local AS = h7.enum("ASType")
        local arr = cntl:CreateArray()
        arr:Invoke("push", { { Type = AS.AS_String, s = "first" }, { Type = AS.AS_Number, n = 2 } })
        local got = arr:GetElementString(0) .. "," .. arr:GetElementFloat(1)
        assert(got == "first,2.0", got)
        return got
    end)
end
check("mods", function() return #h7.mods() .. " mods" end)
check("log_lines", function()
    local _, newest = h7.log_lines()
    h7.log("selftest marker")
    local new, after = h7.log_lines(newest)
    assert(after == newest + 1 and new[1] == "[lua] selftest marker", tostring(new[1]))
    return after .. " lines"
end)
check("config", function()
    assert(h7.config("NoSuchSection", "NoSuchKey") == nil, "missing key not nil")
    assert(h7.config("NoSuchSection", "NoSuchKey", "x") == "x", "default ignored")
    return "Console.Key=" .. tostring(h7.config("Console", "Key"))
end)
check("clipboard", function()
    local old = h7.clipboard()
    assert(h7.clipboard("selftest zażółć"), "could not set")
    local got = h7.clipboard()
    h7.clipboard(old) -- put the player's clipboard back
    assert(got == "selftest zażółć", got)
    return "round trip ok"
end)

-- Multiplexed hook on the function that drives the engine heartbeat.
local ticks = 0
local id = check("hook_post", function()
    return h7.hook_post("Engine.GameViewportClient.PostRender", function(self, args) ticks = ticks + 1 end)
end)
h7.after(2, function()
    h7.log(("[selftest] hook fired %d times in 2s"):format(ticks))
    h7.unhook(id)
end)

-- Calling a UnrealScript function through ProcessEvent.
check("call function", function() return pc:GetHumanReadableName() end)

-- Networking (full offline coverage: build.bat nettest online). Here: the API exists in game and callbacks
-- come back on the game thread.
check("json round trip", function()
    local s = h7.json.encode({ a = 1, b = { true, h7.json.null }, c = h7.json.array() })
    assert(s == '{"a":1,"b":[true,null],"c":[]}', s)
    return h7.json.decode(s).b[2] == h7.json.null
end)
check("local_ip", function() return h7.net.local_ip() or "offline" end)
check("crypto", function()
    assert(h7.crypto.sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    assert(#h7.crypto.random_hex(16) == 32)
    return h7.crypto.hmac_sha256("Jefe", "what do ya want for nothing?"):sub(1, 8) == "5bdcc146"
end)
check("net drivers no downloads", function() -- OnlineLobby turns AllowDownloads off at load
    local d = h7.find_class("IpDrv.TcpNetDriver"):get_default()
    return d.AllowDownloads == false
end)
check("http get dispatched", function()
    return h7.http.get("https://example.com/", function(res)
        h7.log(("[selftest] http get           %s  %s"):format(res.ok and res.status == 200 and "OK  " or "FAIL",
            res.error or (res.status .. ", " .. #res.body .. " bytes")))
    end)
end)
return "selftest dispatched"
