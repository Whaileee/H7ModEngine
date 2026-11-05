-- Finds which toggle commands of the console's list (Stat ..., Show ...) this build recognizes: each is run twice
-- in the same frame (on, then off again), so nothing changes on screen. Writes dumps\probed_commands.txt.
local vp = h7.first("GameViewportClient")
local list = h7.to_table(vp.ViewportConsole.ManualAutoCompleteList)
local ok, missing = {}, {}
for _, e in ipairs(list) do
    local cmd = e.Command:match("^%s*(.-)%s*$")
    local first = cmd:lower():match("^(%S+)")
    if first == "stat" or first == "show" then
        local res = tostring(h7.exec(cmd) or "")
        if res:find("Command not recognized", 1, true) then
            missing[#missing + 1] = cmd
        else
            h7.exec(cmd) -- toggle back
            ok[#ok + 1] = cmd .. (res:match("%S") and ("  => " .. res:gsub("%s+", " ")) or "")
        end
    end
end
local f = io.open("H7ModEngine/dumps/probed_commands.txt", "w") or io.open("../../H7ModEngine/dumps/probed_commands.txt", "w")
f:write("RECOGNIZED\n", table.concat(ok, "\n"), "\n\nNOT RECOGNIZED\n", table.concat(missing, "\n"), "\n")
f:close()
return #ok .. " recognized, " .. #missing .. " not"
