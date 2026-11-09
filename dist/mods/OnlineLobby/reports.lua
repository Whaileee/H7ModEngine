-- Opt-in desync reports (setting "report_desyncs", off by default). Design: docs/ONLINE.md "Desync reports".
--
-- While the setting is on and a multiplayer game runs, the lockstep tracer (lib\mpsync.lua) keeps the last
-- MAX_LINES lines in memory; nothing is written to disk or sent. When the game detects that it went out of sync,
-- or a command arrives too late to run (it would be dropped), the newest lines (up to MAX_BYTES) are sent to the
-- lobby's POST /v1/reports a few seconds later, with the versions, map and mod list. Both players' reports of one
-- game carry the lobby game id, so they can be compared with tools\mpsync_diff.py (tools\fetch_reports.py).
--
-- Not sent: player or profile names, addresses, save games, anything outside the trace. The trace names heroes and
-- creatures (game content) and counts resources.

local mpsync = require("mpsync")

local MAX_LINES = 800        -- lines kept in memory
local MAX_BYTES = 96 * 1024  -- log text per report (the newest lines win); the lobby refuses more than 128 KB
local MAX_PER_GAME = 3       -- reports per game session
local MIN_GAP = 60           -- seconds between two reports of one game
local SETTLE = 5             -- wait for the aftermath (the other player's check, the repair step) before sending
local TRIGGERS = { OOS = true, OOSREMOTE = true, STUCK = true, ["DROP?"] = true, LATE = true, ILATE = true }
local TRACE_SETTINGS = { trace_rng = true, gap_seconds = 0.5, snapshots = true }

local R = {}
local ctx            -- from init(): api, fields(), game_id(), log
local tracer
local buf, head, count = {}, 0, 0 -- ring buffer of lines
local session        -- per game: { id, info, sent, last, pending }

local function push(line)
    head = head % MAX_LINES + 1
    buf[head] = line
    if count < MAX_LINES then count = count + 1 end
end

-- The newest lines that fit in MAX_BYTES, oldest first.
local function recent_text()
    local out, bytes = {}, 0
    for i = 0, count - 1 do
        local line = buf[(head - 1 - i) % MAX_LINES + 1]
        bytes = bytes + #line + 1
        if bytes > MAX_BYTES then break end
        out[#out + 1] = line
    end
    local text = {}
    for i = #out, 1, -1 do text[#text + 1] = out[i] end
    return table.concat(text, "\n") .. "\n"
end

local function send()
    local s = session
    if not s or not s.pending then return end
    local p = s.pending
    s.pending = nil
    tracer.snapshot(true, "report") -- the state now, even while a command runs
    local body = ctx.fields()
    body.session = s.id
    body.game = ctx.game_id()
    body.role = s.info.role
    body.player = s.info.player
    body.map = s.info.map
    body.simturns = s.info.simturns
    body.trigger = table.concat(p.tags, ","):sub(1, 64)
    body.uac = p.uac
    body.log = s.info.header .. "\n" .. recent_text()
    s.sent = s.sent + 1
    s.last = os.clock()
    ctx.api("POST", "/v1/reports", body, nil, function(res, data)
        if res.status == 201 and data and data.id then
            ctx.log("Online: desync report %s sent (%s, UAC %s)", tostring(data.id), body.trigger, tostring(body.uac))
        else
            ctx.log("Online: desync report not sent: %s", tostring(data and data.error or res.error or res.status))
        end
    end)
end

local function trigger(tag)
    local s = session
    if not s then return end
    if s.pending then
        local tags = s.pending.tags
        if not tags[tag] then tags[tag] = true; tags[#tags + 1] = tag end
        return
    end
    if s.sent >= MAX_PER_GAME or (s.last and os.clock() - s.last < MIN_GAP) then return end
    s.pending = { tags = { tag, [tag] = true }, uac = (tracer.counters()) }
    h7.after(SETTLE, send)
end

local function start_tracer()
    if tracer then return end
    tracer = mpsync.new({
        settings = TRACE_SETTINGS,
        on_start = function(info)
            buf, head, count = {}, 0, 0
            session = { id = h7.crypto.random_hex(16), info = info, sent = 0 }
            ctx.log("Online: desync reports are on for this game (session %s)", session.id)
        end,
        on_line = function(line, tag)
            if not session then return end
            push(line)
            if TRIGGERS[tag] then trigger(tag) end
        end,
        on_stop = function()
            session = nil
            buf, head, count = {}, 0, 0
        end,
    })
    if #tracer.missing > 0 then
        ctx.log("Online: desync reports could not hook %d function(s): %s", #tracer.missing,
            table.concat(tracer.missing, "; "))
    end
end

local function stop_tracer()
    if not tracer then return end
    tracer.destroy()
    tracer = nil
end

-- c = { api = function(method, path, body, token, fn), fields = function() -> table, game_id = function() -> id,
--       log = function(fmt, ...) }
function R.init(c)
    ctx = c
    if mod.settings.report_desyncs then start_tracer() end
    h7.on("setting_changed", function(modName, key, value)
        if modName ~= mod.name or key ~= "report_desyncs" then return end
        if value then start_tracer() else stop_tracer() end
        ctx.log("Online: desync reports %s", value and "on" or "off")
    end)
end

function R.shutdown()
    stop_tracer()
end

return R
