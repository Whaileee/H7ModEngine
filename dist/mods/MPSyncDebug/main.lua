-- MPSyncDebug: writes the multiplayer lockstep trace (lib\mpsync.lua) to a file, so two players' logs can be
-- compared with tools\mpsync_diff.py (docs/DESYNC.md, Phase A).
--
-- One file per multiplayer game: <Game>\H7ModEngine\logs\mpsync-<host|client>-<time>.log. The line format and
-- tags are described at the top of lib\mpsync.lua; the important ones are also written to engine.log.

local mpsync = require("mpsync")

local f, path

local function logs_dir()
    return (mod.dir:gsub("[\\/]mods[\\/][^\\/]+[\\/]?$", "")) .. "\\logs"
end

local function close(reason)
    if not f then return end
    f:write(("# closed (%s) at %s\n"):format(reason, os.date("%Y-%m-%d %H:%M:%S")))
    f:close()
    f = nil
    h7.log("MPSync: closed " .. tostring(path) .. " (" .. reason .. ")")
end

local tracer = mpsync.new({
    settings = mod.settings,
    on_start = function(info)
        path = ("%s\\mpsync-%s-%s.log"):format(logs_dir(), info.role, os.date("%Y%m%d-%H%M%S"))
        local err
        f, err = io.open(path, "w")
        if not f then
            h7.warn("MPSync: can't write " .. path .. ": " .. tostring(err))
            return
        end
        f:setvbuf("full", 65536)
        f:write(("# MPSyncDebug %s (engine %s)\n"):format(mod.version, tostring(h7.version)), info.header, "\n")
        h7.log("MPSync: tracing to " .. path)
    end,
    on_line = function(line, _, important)
        if not f then return end
        f:write(line, "\n")
        if important then
            f:flush()
            h7.warn("MPSync: " .. line)
        end
    end,
    on_stop = close,
})

if #tracer.missing > 0 then
    h7.warn("MPSync: could not hook " .. #tracer.missing .. " function(s): " .. table.concat(tracer.missing, "; "))
end

-- Flush once a second, so a crash or a killed game loses at most a second of lines.
h7.every(1, function()
    if f then f:flush() end
end)

h7.log("MPSync: ready; tracing starts in the next multiplayer game")

function on_unload()
    tracer.destroy()
    close("mod unloaded")
end
