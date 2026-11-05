-- Logs what the DebugConsole suggests in the current screen (game commands, Lua members and parameter hints),
-- using its suggest.lua directly. Run at the main menu and on an adventure map.
local found
for _, m in ipairs(h7.mods()) do if m.name == "DebugConsole" then found = true end end
assert(found, "DebugConsole is not installed")

local base = io.open("H7ModEngine/mods/DebugConsole/suggest.lua") and "H7ModEngine/mods/DebugConsole/"
    or "../../H7ModEngine/mods/DebugConsole/"
local shim = { name = "DebugConsole" }
local modenv = setmetatable({ mod = shim }, { __index = _G })
function shim.require(n) return assert(loadfile(base .. n .. ".lua", "t", modenv))() end
local S = shim.require("suggest")

local t0 = os.clock()
local list = S.commands()
h7.log(("[consolesuggest] %d commands in %.0f ms"):format(#list, (os.clock() - t0) * 1000))

local env = setmetatable({}, { __index = _G })
local function show(line)
    local r = S.compute(line:match("[^\n]*$"), line, env, line)
    local parts = {}
    if r and r.hint then
        local h = r.hint
        parts[#parts + 1] = ("HINT %s%s [arg %s] %s"):format(h.label,
            h.params and S.format_sig(h.params, h.ret) or "", tostring(h.active), h.doc or "")
    end
    for i = 1, math.min(4, r and r.items and #r.items or 0) do
        local it = r.items[i]
        S.detail(it)
        parts[#parts + 1] = ("%s  %s  %s  %s"):format(it.text, it.kind or "?", it.detail or "", it.owner or "")
    end
    h7.log(("[consolesuggest] %-34s -> %s"):format(line, table.concat(parts, " | ")))
end
show("/st")
show("/stat ")
show("/summon ")
show("h7.fi")
show("h7.find_all(\"H7Creature\", ")
show("h7.first(\"GameViewportClient\").View")
show("h7.first(\"H7CouncilPlayerController\"):Get")
show("h7.first(\"GameViewportClient\"):GetPlayerOwner(")
show("x = h7.first(\"GameViewportClient\"):GetPlayerOwner(0).Ac")
show("h7.first(\"GameViewportClient\"):get_n")
show("print(1, ")
h7.log("[consolesuggest] DONE")
