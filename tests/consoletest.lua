-- DebugConsole check: turns the mod on the way the Mods menu does, then waits for typed input.
-- Drive it from outside with tests/keys.ps1 (see the steps logged below); assert on the log lines.
local function log(s) h7.log("[consoletest] " .. s) end
local function info()
    for _, m in ipairs(h7.mods()) do
        if m.name == "DebugConsole" then return m end
    end
end
local m = info()
log(m and ("listed: category=%s hidden=%s enabled=%s settings=%d"):format(m.category, tostring(m.hidden),
    tostring(m.enabled), m.settings) or "NOT LISTED")
log("key from ini: " .. tostring(h7.config("Console", "Key")))
h7.set_mod_enabled("DebugConsole", true)
h7.after(1, function()
    m = info()
    log("loaded=" .. tostring(m and m.loaded))
    log("now: keys.ps1 TILDE h 7 PERIOD v e r s i o n RETURN 2 ADD 3 MULTIPLY 4 RETURN")
    log("DONE")
end)
