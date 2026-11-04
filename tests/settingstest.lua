-- Mod settings self-test: copy into <Game>\H7ModEngine\exec\ while the game runs; results go to logs\engine.log.
-- Uses the DamageScale and HeroMovement example settings and puts every value back afterwards.
local function check(name, fn)
    local ok, v = pcall(fn)
    h7.log(("[settingstest] %-24s %s  %s"):format(name, ok and "OK  " or "FAIL", tostring(v)))
    return ok and v
end
local function find(mod, key)
    for _, s in ipairs(h7.mod_settings(mod)) do if s.key == key then return s end end
end

local old = find("DamageScale", "multiplier").value
local oldLog = find("DamageScale", "log").value
local oldWho = find("HeroMovement", "who").value

check("listed in h7.mods", function()
    for _, m in ipairs(h7.mods()) do if m.name == "DamageScale" then assert(m.settings == 2, m.settings) return m.settings end end
    error("DamageScale not found")
end)
check("descriptor", function()
    local s = find("DamageScale", "multiplier")
    assert(s.type == "number" and s.min == 0.25 and s.max == 4 and s.step == 0.25 and s.default == 1.25, "fields")
    return ("%s %s [%s..%s] step %s"):format(s.label, tostring(s.value), s.min, s.max, s.step)
end)
check("set number", function() return assert(h7.set_mod_setting("DamageScale", "multiplier", 2) == 2) end)
check("snap to step", function() local v = h7.set_mod_setting("DamageScale", "multiplier", 1.3); assert(v == 1.25, v) return v end)
check("clamp to max", function() local v = h7.set_mod_setting("DamageScale", "multiplier", 99); assert(v == 4, v) return v end)
check("number from text", function() local v = h7.set_mod_setting("DamageScale", "multiplier", "1.5"); assert(v == 1.5, v) return v end)
check("bad number rejected", function()
    local ok = pcall(h7.set_mod_setting, "DamageScale", "multiplier", "abc")
    assert(not ok, "accepted") return "error raised"
end)
check("bool", function() local v = h7.set_mod_setting("DamageScale", "log", false); assert(v == false) return v end)
check("choice", function() local v = h7.set_mod_setting("HeroMovement", "who", "everyone"); assert(v == "Everyone", v) return v end)
check("bad choice rejected", function()
    assert(not pcall(h7.set_mod_setting, "HeroMovement", "who", "Nobody"), "accepted") return "error raised"
end)
check("unknown key rejected", function()
    assert(not pcall(h7.set_mod_setting, "HeroMovement", "nope", 1), "accepted") return "error raised"
end)
local seen = {}
local id = h7.on("setting_changed", function(mod, key, value) seen[#seen + 1] = mod .. "." .. key .. "=" .. tostring(value) end)
check("setting_changed event", function()
    h7.set_mod_setting("DamageScale", "multiplier", 3)
    h7.set_mod_setting("DamageScale", "multiplier", 3) -- unchanged: no event
    assert(#seen == 1, #seen) return seen[1]
end)
h7.off(id)

h7.set_mod_setting("DamageScale", "multiplier", old)
h7.set_mod_setting("DamageScale", "log", oldLog)
h7.set_mod_setting("HeroMovement", "who", oldWho)
check("restored", function() return find("DamageScale", "multiplier").value == old end)
h7.log("[settingstest] DONE")
