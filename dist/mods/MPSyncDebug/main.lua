-- MPSyncDebug: traces the multiplayer lockstep so two players' logs can be compared (docs/DESYNC.md, Phase A).
--
-- While a multiplayer game runs, every line goes to <Game>\H7ModEngine\logs\mpsync-<host|client>-<time>.log:
--   <seconds since start> u=<action counter> r=<synced RNG counter> <TAG> <details>
-- Tags (the important ones are also written to engine.log as warnings):
--   STEP     the action counter (UAC) went up        SEND / ISEND   this PC sent a command / instant command
--   RELAY    the host forwarded a player's command   RECV / IRECV   a command arrived (LATE: behind our counter)
--   QUEUE    a command waits for its counter         CMD / IEXEC    a command was started here
--   DROP?    a queued command is behind the counter and will be thrown away (the game's "illegal uac")
--   STUCK    a queued command can never run (its counter is already past); WAIT: queued for over 15 s
--   RNG      a draw from the synced generator        CHECK / RCHECK our / the other side's out-of-sync check
--   OOS      the game detected an out-of-sync state  SYNCH*         the game's repair step
--   GAP      no frame ran for a while (the window was in the background); BURST: many commands in one frame
--   HASH / STATE   the game state while nothing runs (STATE lists it when it changed)
-- tools\mpsync_diff.py compares two of these logs.

local statelib = mod.require("state")

local NM_LISTEN, NM_CLIENT = 2, 3 -- ENetMode

local S = {
    f = nil, path = nil, t0 = 0, ticks = 0,
    pc = nil, ri = nil, rng = nil, mgr = nil, adv = nil, cc = nil,
    in_combat = false, simturns = false,
    last_tick = nil, recv = 0,
    seen = {}, first_seen = {}, last_snap_u = nil, last_hash = nil,
}

local function try(f)
    local ok, v = pcall(f)
    if ok then return v end
end

local function fmt(s, ...)
    local ok, v = pcall(string.format, s, ...)
    return ok and v or (s .. " <format error: " .. tostring(v) .. ">")
end

local function valid(o)
    return o ~= nil and try(function() return o:is_valid() end) == true
end

local function id_of(o)
    if o == nil then return -1 end
    return try(function() return o:GetID() end) or -2
end

local function class_of(o)
    if o == nil then return "none" end
    return try(function() return o:get_class():get_name() end) or "?"
end

local function player_of(pc)
    return try(function() return pc:GetPlayerReplicationInfo():GetPlayerNumber() end) or -1
end

local function reverse_enum(name)
    local t = {}
    local e = try(function() return h7.enum(name) end)
    if type(e) == "table" then for k, v in pairs(e) do t[v] = k end end
    return t
end
local ICT, UC, TAG, OOST = reverse_enum("EInstantCommandType"), reverse_enum("EUnitCommand"),
    reverse_enum("ECommandTag"), reverse_enum("EOOSType")
local function ename(t, v)
    if v == nil then return "nil" end
    return t[v] or tostring(v)
end

-- Counters ------------------------------------------------------------------------------------------------------

local function counters()
    local ri, rng = S.ri, S.rng
    local u = ri and try(function() return ri.mUnitActionsCounter end) or -1
    local r = rng and try(function() return rng.mCounter end) or -1
    return u, r
end

-- Log file ------------------------------------------------------------------------------------------------------

local function emit(tag, detail, important)
    local f = S.f
    if not f then return end
    local u, r = counters()
    local line = fmt("%10.3f u=%d r=%d %s%s", os.clock() - S.t0, u, r, tag, detail and (" " .. detail) or "")
    f:write(line, "\n")
    if important then
        f:flush()
        h7.warn("MPSync: " .. line)
    end
end

local function logs_dir()
    return (mod.dir:gsub("[\\/]mods[\\/][^\\/]+[\\/]?$", "")) .. "\\logs"
end

local function open(wi, pc)
    local mode = try(function() return wi.NetMode end)
    local role = mode == NM_LISTEN and "host" or mode == NM_CLIENT and "client" or ("nm" .. tostring(mode))
    local path = fmt("%s\\mpsync-%s-%s.log", logs_dir(), role, os.date("%Y%m%d-%H%M%S"))
    local f, err = io.open(path, "w")
    if not f then
        h7.warn("MPSync: can't write " .. path .. ": " .. tostring(err))
        return
    end
    f:setvbuf("full", 65536)
    S.f, S.path, S.t0 = f, path, os.clock()
    S.seen, S.first_seen, S.last_snap_u, S.last_hash = {}, {}, nil, nil
    f:write(fmt("# MPSyncDebug %s role=%s player=%s map=%s simturns=%s started=%s\n", mod.version, role,
        player_of(pc), tostring(try(function() return wi:GetMapName(false) end)), tostring(S.simturns),
        os.date("%Y-%m-%d %H:%M:%S")))
    h7.log("MPSync: tracing to " .. path)
end

local function close(reason)
    if not S.f then return end
    S.f:write(fmt("# closed (%s) at %s\n", reason, os.date("%Y-%m-%d %H:%M:%S")))
    S.f:close()
    S.f = nil
    h7.log("MPSync: closed " .. tostring(S.path) .. " (" .. reason .. ")")
end

-- State snapshots -----------------------------------------------------------------------------------------------

local function snapshot(force, why)
    if not S.f then return end
    local lines = statelib.snapshot(S.adv, S.cc, S.in_combat)
    if not lines then return end
    local h = statelib.hash(lines)
    S.last_snap_u = counters()
    if h == S.last_hash and not force then
        emit("HASH", "h=" .. h)
        return
    end
    S.last_hash = h
    emit("STATE", fmt("h=%s lines=%d%s", h, #lines, why and (" why=" .. why) or ""))
    for _, l in ipairs(lines) do S.f:write("  | ", l, "\n") end
end

-- Nothing running: no command on this PC and nothing waiting in the multiplayer queues.
local function idle()
    local ctl = S.in_combat and S.cc or S.adv
    if not ctl then return false end
    local busy = try(function() return not ctl:GetCommandQueue():IsEmpty() end)
    if busy ~= false then return false end
    local mgr = S.mgr
    if mgr and ((try(function() return #mgr.mCommandQueue + #mgr.mInstantCommandQueue end) or 1) > 0) then
        return false
    end
    return true
end

-- Queue watchdog ------------------------------------------------------------------------------------------------

local function scan(qname, list, get_uac, describe, u, now)
    for i = 1, #list do
        local c = list[i]
        local cu = get_uac(c) or -1
        local desc = describe(c)
        local key = qname .. ":" .. cu .. ":" .. desc
        local first = S.first_seen[key]
        if not first then
            first = now
            S.first_seen[key] = now
        end
        if cu < u and not S.seen[key] then
            S.seen[key] = true
            emit("STUCK", fmt("%s uac=%s %s (counter is already %d)", qname, cu, desc, u), true)
        elseif now - first > 15 and not S.seen["wait:" .. key] then
            S.seen["wait:" .. key] = true
            emit("WAIT", fmt("%s uac=%s %s queued for %.0f s", qname, cu, desc, now - first), true)
        end
    end
end

local function watchdog()
    local mgr = S.mgr
    if not mgr then return end
    local u = counters()
    local now = os.clock()
    pcall(scan, "commands", mgr.mCommandQueue, function(c) return c.UnitActionsCounter end,
        function(c) return fmt("cmd=%s src=%s", ename(UC, c.CommandType), id_of(c.CommandSource)) end, u, now)
    pcall(scan, "instant", mgr.mInstantCommandQueue, function(c) return c.UnitActionsCounter end,
        function(c) return "type=" .. ename(ICT, c.Type) end, u, now)
end

-- Refresh (once a second, on game time) -------------------------------------------------------------------------

local function live(cls)
    for _, o in ipairs(try(function() return h7.find_all(cls) end) or {}) do
        local n = try(function() return o:get_full_name() end) or ""
        if n:find(".TheWorld.", 1, true) then return o end
    end
end

local function local_pc()
    for _, pc in ipairs(try(function() return h7.find_all("H7CombatPlayerController") end) or {}) do
        if try(function() return pc:IsLocalPlayerController() end) then return pc end
    end
end

local function refresh(force)
    S.ticks = S.ticks + 1
    if not valid(S.pc) then
        S.pc = nil
        if not force and S.ticks % 2 ~= 0 then return end -- find_all scans every object; not every second
        S.pc = local_pc()
    end
    local pc = S.pc
    local wi = pc and try(function() return pc.WorldInfo end)
    local mp = wi and try(function() return wi.GRI:IsMultiplayerGame() end)
    if not mp then
        -- Map changes and loading screens look like "no game" for a moment; keep one file per session.
        S.no_mp = (S.no_mp or 0) + 1
        if S.no_mp >= 20 then close("not in a multiplayer game") end
        return
    end
    S.no_mp = 0
    if not valid(S.ri) then S.ri, S.rng, S.mgr = live("H7ReplicationInfo"), nil, nil end
    local ri = S.ri
    if not ri then return end
    if not valid(S.rng) then S.rng = try(function() return ri.mSynchRNG end) end
    if not valid(S.mgr) then S.mgr = try(function() return ri.mMpCommandManager end) end
    S.in_combat = try(function() return ri:IsCombatMap() end) == true
    S.simturns = try(function() return ri:isSimTurns() end) == true
    if S.in_combat then
        if not valid(S.cc) then S.cc = live("H7CombatController") end
    elseif not valid(S.adv) then
        S.adv = live("H7AdventureController")
    end
    if not S.f then open(wi, pc) end
    if not S.f then return end
    if S.ticks % 2 == 0 then watchdog() end
    if mod.settings.snapshots and counters() ~= S.last_snap_u and idle() then snapshot(false) end
    S.f:flush()
end

h7.every(1, function() refresh(false) end)
h7.on("map_loaded", function(map)
    emit("MAP", tostring(map))
    S.pc, S.adv, S.cc = nil, nil, nil
    refresh(true)
end)

-- Frame gaps and bursts. The game stops ticking while its window is unfocused, so a gap shows on return.
h7.on("tick", function(dt)
    local now = os.clock()
    local last = S.last_tick
    S.last_tick = now
    if not S.f then
        S.recv = 0
        return
    end
    if last and now - last >= mod.settings.gap_seconds then
        emit("GAP", fmt("real=%.2fs dt=%.3fs", now - last, dt or -1), now - last >= 2)
    end
    if S.recv >= 3 then emit("BURST", fmt("%d commands arrived in one frame", S.recv), true) end
    S.recv = 0
end)

-- Hooks ---------------------------------------------------------------------------------------------------------

local missing = {}
local function hook(path, fn, post)
    local ok, err = pcall(post and h7.hook_post or h7.hook, "MMH7Game." .. path, fn)
    if not ok then missing[#missing + 1] = path .. " (" .. tostring(err) .. ")" end
end

-- Counters
hook("H7ReplicationInfo.IncUnitActionsCounter", function()
    if not S.f then return end
    local ri = S.ri
    emit("STEP", fmt("id=%s gs=%s", try(function() return ri.mIdCounter end),
        try(function() return ri.mGameStateCounter end)))
end, true)
hook("H7ReplicationInfo.SetUnitActionsCounter", function(_, a)
    if not S.f then return end
    emit("UACSET", fmt("to=%s", a.Value), true)
end)
hook("H7ReplicationInfo.ResetIDCounterToNewId", function(_, a)
    if not S.f then return end
    emit("IDSET", fmt("to=%s", a.newIdCounter))
end)

-- Synced random numbers
hook("H7SynchRNG.GetRandomFloat", function(_, _, v)
    if not S.f or not mod.settings.trace_rng then return end
    emit("RNG", fmt("F=%.6f", v or -1))
end, true)
hook("H7SynchRNG.GetRandomInt", function(_, a, v)
    if not S.f or not mod.settings.trace_rng then return end
    emit("RNG", fmt("I(%s)=%s", a.Max, v))
end, true)
hook("H7SynchRNG.GetRandomIntRange", function(_, a, v)
    if not S.f or not mod.settings.trace_rng then return end
    emit("RNG", fmt("R(%s,%s)=%s", a.Min, a.Max, v))
end, true)
hook("H7SynchRNG.SetCounter", function(_, a)
    if not S.f then return end
    emit("RNGSET", fmt("to=%s", a.newCounter), true)
end)
hook("H7SynchRNG.RefillPool", function()
    if not S.f then return end
    emit("REFILL")
end, true)

-- Sending
hook("H7CombatPlayerController.SendPlayCommand", function(_, a)
    if not S.f then return end
    emit("SEND", fmt("uac=%s cmd=%s tag=%s src=%s tgt=%s path=%s player=%s oos=%s", a.unitActionCounter,
        ename(UC, a.CommandType), ename(TAG, a.CommandTag), id_of(a.Source), id_of(a.Target),
        try(function() return #a.Path end), a.CurrentPlayer, tostring(a.doOOSCheck)))
end)
hook("H7CombatPlayerController.SendInstantCommand", function(_, a)
    if not S.f then return end
    local c = a.Command
    emit("ISEND", fmt("uac=%s type=%s player=%s combat=%s tactics=%s to=%s", c.UnitActionsCounter,
        ename(ICT, c.Type), c.CurrentPlayer, tostring(c.IsCombatCommand), tostring(c.IsTactics), a.ToPlayer))
end)
hook("H7InstantCommandManager.StartCommand", function(_, a, ok)
    if not S.f then return end
    emit("ISTART", fmt("%s ok=%s", class_of(a.Command), tostring(ok)))
end, true)

-- The host forwarding commands (server functions only run on the host)
local function relay(kind)
    return function(pc, a)
        if not S.f then return end
        local p = a.Parameters or {}
        emit("RELAY", fmt("%s from=%s uac=%s cmd=%s src=%s combat=%s to=%s", kind, player_of(pc), p[1],
            ename(UC, p[2]), p[3], tostring(a.IsCombat), a.ToPlayer))
    end
end
hook("H7CombatPlayerController.ServerPlayUnitCommand", relay("cmd"))
hook("H7CombatPlayerController.ServerPlayUnitCommandWithPath", relay("cmd+path"))
hook("H7CombatPlayerController.ServerPlayUnitCommandWithExtPath", relay("cmd+extpath"))
hook("H7CombatPlayerController.ServerInstantCommand", function(pc, a)
    if not S.f then return end
    emit("RELAY", fmt("instant from=%s uac=%s type=%s combat=%s tactics=%s to=%s", player_of(pc),
        a.unitActionCounter, ename(ICT, a.instantCommandType), tostring(a.IsCombat), tostring(a.IsTactics),
        a.ToPlayer))
end)

-- Receiving
hook("H7CombatPlayerController.ClientPlayUnitCommandConvertedPath", function(_, a)
    if not S.f then return end
    S.recv = S.recv + 1
    local p = a.Parameters or {}
    local u = counters()
    local late = not S.simturns and p[1] ~= nil and p[1] < u
    emit("RECV", fmt("uac=%s cmd=%s tag=%s src=%s tgt=%s combat=%s%s", p[1], ename(UC, p[2]), ename(TAG, p[6]),
        p[3], p[5], tostring(a.IsCombat), late and " LATE" or ""), late)
end)
hook("H7CombatPlayerController.ClientInstantCommand", function(_, a)
    if not S.f then return end
    S.recv = S.recv + 1
    local u = counters()
    local late = not S.simturns and a.unitActionCounter ~= nil and a.unitActionCounter < u
    emit("IRECV", fmt("uac=%s type=%s player=%s combat=%s%s", a.unitActionCounter,
        ename(ICT, a.instantCommandType), a.CurrentPlayer, tostring(a.IsCombat), late and " LATE" or ""), late)
end)
hook("H7MultiplayerCommandManager.AddCommand", function(_, a)
    if not S.f then return end
    emit("QUEUE", fmt("uac=%s cmd=%s src=%s combat=%s", a.unitActionCounter, ename(UC, a.Command),
        id_of(a.CommandSource), tostring(a.IsCombat)))
end)
hook("H7MultiplayerCommandManager.AddInstantCommand", function(_, a)
    if not S.f then return end
    local c = a.instantCommand
    emit("IQUEUE", fmt("uac=%s type=%s player=%s", c.UnitActionsCounter, ename(ICT, c.Type), c.CurrentPlayer))
end)
hook("H7MultiplayerCommandManager.UnqueuePreparedCommands", function(mgr)
    if not S.f then return end
    local qc, qi = mgr.mQueuedMPCommands, mgr.mQueuedMPInstantCommands
    local u = counters()
    emit("UNQUEUE", fmt("cmds=%d instant=%d", #qc, #qi))
    for i = 1, #qc do
        local p = try(function() return qc[i].Parameters end) or {}
        if p[1] and p[1] < u then
            emit("DROP?", fmt("cmd uac=%s cmd=%s src=%s (counter %d)", p[1], ename(UC, p[2]), p[3], u), true)
        end
    end
    for i = 1, #qi do
        local c = qi[i]
        if c.UnitActionsCounter < u then
            emit("DROP?", fmt("instant uac=%s type=%s (counter %d)", c.UnitActionsCounter, ename(ICT, c.Type), u), true)
        end
    end
end)

-- Running
hook("H7CommandQueue.Enqueue", function(_, a)
    if not S.f then return end
    local c = a.Cmd
    if not c then return end
    emit("CMD", fmt("cmd=%s tag=%s src=%s tgt=%s path=%s mp=%s", ename(UC, try(function() return c:GetCommand() end)),
        ename(TAG, try(function() return c:GetCommandTag() end)), id_of(try(function() return c:GetCommandSource() end)),
        id_of(try(function() return c:GetAbilityTarget() end)), #(try(function() return c:GetPath() end) or {}),
        tostring(try(function() return c:IsMPSynchronized() end))))
end)
hook("H7InstantCommandManager.ExecuteInstantCommand", function(_, a)
    if not S.f then return end
    emit("IEXEC", class_of(a.Command))
end)

-- Out-of-sync checks and the game's repair step
local function local_check(kind)
    local ri = S.ri
    local units, res = -1, -1
    if kind == "combat" then
        units = try(function() return S.cc:GetTotalHealth() end) or -1
    else
        units = try(function() return S.adv:GetTotalUnitsCount() end) or -1
        res = try(function() return S.adv:GetTotalResCount() end) or -1
    end
    emit("CHECK", fmt("%s id=%s units=%s res=%s", kind, try(function() return ri.mIdCounter end), units, res))
end
hook("H7CombatPlayerController.CheckOutOfSynch", function()
    if not S.f then return end
    local_check("combat")
end)
hook("H7AdventurePlayerController.CheckOutOfSynch", function()
    if not S.f or S.in_combat then return end -- in combat it calls the combat version (hooked above)
    local_check("adventure")
end)
hook("H7MultiplayerCommandManager.AddOutOfSynchData", function(_, a)
    if not S.f then return end
    emit("RCHECK", fmt("from=%s uac=%s rng=%s id=%s units=%s res=%s combat=%s", a.PlayerNumber, a.unitActionCounter,
        a.SynchRNG, a.IDCounter, a.UnitsCount, a.ResCount, tostring(a.IsCombat)))
end)
hook("H7MultiplayerCommandManager.GameWentOOSMessage", function(_, a)
    if not S.f then return end
    emit("OOS", fmt("%s (uac %s)", a.msg, a.uac), true)
    snapshot(true, "oos")
end)
hook("H7MultiplayerCommandManager.GameWentOOS", function(_, a)
    if not S.f then return end
    emit("OOSTYPE", fmt("type=%s synchUpPlayer=%s", ename(OOST, a.oosType), a.synchUpPlayer), true)
end)
hook("H7MultiplayerCommandManager.StartSynchUp", function()
    if not S.f then return end
    emit("SYNCHSTART", nil, true)
end)
hook("H7CombatPlayerController.ServerGameWentOOS", function(pc, a)
    if not S.f then return end
    emit("SYNCHREQ", fmt("from=%s type=%s uac=%s to=%s synchUpPlayer=%s", player_of(pc), ename(OOST, a.oosType), a.uac,
        a.ToPlayer, a.synchUpPlayer), true)
end)
hook("H7CombatPlayerController.ClientStartSynchUp", function(_, a)
    if not S.f then return end
    emit("SYNCHASK", fmt("type=%s", ename(OOST, a.oosType)), true)
end)
hook("H7CombatPlayerController.ClientSynchUp", function(_, a)
    if not S.f then return end
    emit("SYNCH", fmt("uac=%s rng=%s id=%s combat=%s", a.unitActionCounter, a.RNGCounter, a.IDCounter,
        tostring(a.IsCombat)), true)
end)
hook("H7CombatPlayerController.ClientSynchUpArmy", function(_, a)
    if not S.f then return end
    emit("SYNCHARMY", fmt("uac=%s hero=%s stacks=%s", a.unitActionCounter, a.heroId, a.stackIDStrings))
end)
hook("H7CombatPlayerController.ClientSynchUpCombatUnit", function(_, a)
    if not S.f then return end
    emit("SYNCHUNIT", fmt("uac=%s stack=%s count=%s hp=%s", a.unitActionCounter, a.StackId, a.StackCount,
        a.TopCreatureHealth))
end)
hook("H7CombatPlayerController.ClientGameWentOOSMessage", function(_, a)
    if not S.f then return end
    emit("OOSREMOTE", fmt("%s (uac %s)", a.Message, a.uac), true)
end)
hook("H7CombatPlayerController.ClientCommandCancelled", function(_, a)
    if not S.f then return end
    emit("CANCELLED", fmt("reason=%s", a.Reason), true)
end)

-- Turn flow, tactics and sim-turn combat requests (timer-driven paths)
hook("H7AdventureController.EndMyTurn", function()
    if not S.f then return end
    emit("ENDTURN")
end)
hook("H7CombatPlayerController.SendTacticsPhaseFinished", function(_, a)
    if not S.f then return end
    emit("TACTICSSEND", fmt("defender=%s to=%s", tostring(a.IsDefender), a.ToPlayer))
end)
hook("H7CombatPlayerController.ClientTacticsPhaseFinished", function(_, a)
    if not S.f then return end
    emit("TACTICSRECV", fmt("defender=%s", tostring(a.IsDefender)))
end)
hook("H7MultiplayerCommandManager.InsertOngoingStartCombat", function(_, a)
    if not S.f then return end
    emit("STARTCOMBAT", fmt("source=%s target=%s", id_of(a.Source), id_of(a.Target)))
end)
hook("H7MultiplayerCommandManager.HandleAnswerStartCombat", function(_, a)
    if not S.f then return end
    emit("STARTCOMBATANSWER", fmt("answer=%s hero=%s sync=%s", a.answer, a.heroId, tostring(a.doSynchronise)))
end)
hook("H7MultiplayerCommandManager.FinishOngoingStartCombat", function(_, a)
    if not S.f then return end
    emit("STARTCOMBATEND", fmt("retreating=%s retreatFinished=%s", tostring(a.isRetreating),
        tostring(a.isRetreatFinished)))
end)
hook("H7SimTurnCommandManager.FinishOngoingStartCombat", function(_, a)
    if not S.f then return end
    emit("SIMCOMBATEND", fmt("retreatFinished=%s canceled=%s normalCombat=%s", tostring(a.isRetreatFinished),
        tostring(a.isCombatCanceled), tostring(a.IsNormalCombatAboutToBegin)), true)
end)

if #missing > 0 then
    h7.warn("MPSync: could not hook " .. #missing .. " function(s): " .. table.concat(missing, "; "))
end
h7.log("MPSync: ready; tracing starts in the next multiplayer game")

function on_unload()
    close("mod unloaded")
end
