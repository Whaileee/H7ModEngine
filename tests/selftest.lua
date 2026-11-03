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
check("mods", function() return #h7.mods() .. " mods" end)

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
return "selftest dispatched"
