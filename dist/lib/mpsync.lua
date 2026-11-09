-- mpsync: traces the multiplayer lockstep (docs/DESYNC.md). Shared by MPSyncDebug (writes a log file) and
-- OnlineLobby (keeps the last lines in memory for opt-in desync reports).
--
--   local mpsync = require("mpsync")
--   local tracer = mpsync.new{
--       settings = { trace_rng = true, gap_seconds = 0.5, snapshots = true }, -- read live (mod.settings works)
--       on_start = function(info) end,        -- a multiplayer game began: {role, player, map, simturns, header}
--       on_line = function(line, tag, important) end, -- every line, including snapshot lines ("  | ...", tag "|")
--       on_stop = function(reason) end,       -- the game ended (or the tracer was destroyed)
--   }
--   tracer.snapshot(true, "why")             -- log the game state now, even while a command runs
--   tracer.destroy()                         -- remove its hooks, events and timers
--
-- Call new() from the mod's own code: the hooks, events and timers belong to the mod that creates them, and each
-- tracer has its own state, so two mods can trace at the same time.
--
-- Line format: <seconds since start> u=<action counter> r=<synced RNG counter> <TAG> <details>
--   STEP     the action counter (UAC) went up        SEND / ISEND   this PC sent a command / instant command
--   RELAY    the host forwarded a player's command   RECV / IRECV   a command arrived
--   LATE / ILATE   a command arrived with a counter already behind ours (the game will drop it)
--   QUEUE    a command waits for its counter         CMD / IEXEC    a command was started here
--   DROP?    a queued command is behind the counter and will be thrown away (the game's "illegal uac")
--   STUCK    a queued command can never run (its counter is already past); WAIT: queued for over 15 s
--   RNG      a draw from the synced generator        CHECK / RCHECK our / the other side's out-of-sync check
--   OOS      the game detected an out-of-sync state  SYNCH*         the game's repair step
--   GAP      no frame ran for a while (the window was in the background); BURST: many commands in one frame
--   HASH / STATE   the game state while nothing runs (STATE lists it when it changed)
-- tools\mpsync_diff.py compares two players' lines.

local M = {}

M.IMPORTANT = { STUCK = true, WAIT = true, ["DROP?"] = true, OOS = true, OOSTYPE = true, OOSREMOTE = true,
    SYNCHSTART = true, SYNCHREQ = true, SYNCHASK = true, SYNCH = true, UACSET = true, RNGSET = true,
    CANCELLED = true, SIMCOMBATEND = true, BURST = true, LATE = true, ILATE = true }

local NM_LISTEN, NM_CLIENT = 2, 3 -- ENetMode

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

local enums
local function ename(kind, v)
    if v == nil then return "nil" end
    if not enums then
        enums = {}
        for key, name in pairs({ ICT = "EInstantCommandType", UC = "EUnitCommand", TAG = "ECommandTag",
            OOST = "EOOSType" }) do
            local t = {}
            local e = try(function() return h7.enum(name) end)
            if type(e) == "table" then for k, n in pairs(e) do t[n] = k end end
            enums[key] = t
        end
    end
    return enums[kind][v] or tostring(v)
end

-- Game state snapshot ---------------------------------------------------------------------------------------------
-- One line per player, hero army or combat stack, in a fixed order, so two peers' snapshots at the same action
-- counter can be compared line by line.

local function pos(p)
    if type(p) ~= "table" then return "?" end
    return ("%s,%s"):format(tostring(p.X), tostring(p.Y))
end

local function stack_list(list, combat)
    local out = {}
    for i = 1, #list do
        local s = list[i]
        if s then
            local name = try(function() return s:GetStackType():GetName() end) or "?"
            local size = try(function() return s:GetStackSize() end) or -1
            if combat then
                local hp = try(function() return s:GetTopCreatureHealth() end) or -1
                out[#out + 1] = ("%s#%d x%d hp%d @%s"):format(name, id_of(s), size, hp,
                    pos(try(function() return s:GetGridPosition() end)))
            elseif size > 0 then
                out[#out + 1] = ("%s x%d"):format(name, size)
            end
        end
    end
    return table.concat(out, ", ")
end

local function adventure_state(adv, lines)
    local players = adv.mPlayers
    for i = 1, #players do
        local pl = players[i]
        local num = try(function() return pl:GetPlayerNumber() end) or -1
        local res = try(function() return pl:GetResourceSet():GetAllResourceAmounts() end)
        lines[#lines + 1] = ("player %d res=%s"):format(num, type(res) == "table" and table.concat(res, "/") or "?")
    end
    local armies = adv.mArmies
    local heroes = {}
    for i = 1, #armies do
        local army = armies[i]
        local hero = army and army.mHero
        if hero and try(function() return hero:IsHero() end) then
            local owner = try(function() return army:GetPlayer():GetPlayerNumber() end) or -1
            local cell = try(function() return army.mCell:GetCellPosition() end)
            local mp = try(function() return hero:GetCurrentMovementPoints() end) or -1
            heroes[#heroes + 1] = ("hero %d p%d %s @%s mp=%.2f | %s"):format(id_of(hero), owner,
                try(function() return hero:GetName() end) or "?", pos(cell), mp,
                stack_list(army.mBaseCreatureStacks, false))
        end
    end
    table.sort(heroes)
    for _, l in ipairs(heroes) do lines[#lines + 1] = l end
end

local function combat_state(cc, lines)
    for _, side in ipairs({ "mArmyAttacker", "mArmyDefender" }) do
        local army = cc[side]
        if army then
            lines[#lines + 1] = ("%s %s"):format(side == "mArmyAttacker" and "attacker" or "defender",
                stack_list(army.mCreatureStacks, true))
        end
    end
end

-- Lines describing the state, or nil when nothing could be read.
function M.state_lines(adv, cc, in_combat)
    local lines = {}
    if in_combat and cc then
        pcall(combat_state, cc, lines)
    elseif adv then
        pcall(adventure_state, adv, lines)
    end
    if #lines == 0 then return nil end
    return lines
end

-- 32-bit FNV-1a of the lines, as 8 hex digits.
function M.hash(lines)
    local h = 2166136261
    for _, l in ipairs(lines) do
        for i = 1, #l do
            h = ((h ~ l:byte(i)) * 16777619) & 0xffffffff
        end
        h = ((h ~ 10) * 16777619) & 0xffffffff
    end
    return ("%08x"):format(h)
end

-- Finding the game's objects -------------------------------------------------------------------------------------

local function live(cls)
    for _, o in ipairs(try(function() return h7.find_all(cls) end) or {}) do
        local n = try(function() return o:get_full_name() end) or ""
        if n:find(".TheWorld.", 1, true) then return o end -- not class defaults or archetypes
    end
end

local function local_pc()
    for _, pc in ipairs(try(function() return h7.find_all("H7CombatPlayerController") end) or {}) do
        if try(function() return pc:IsLocalPlayerController() end) then return pc end
    end
end

-- The tracer -----------------------------------------------------------------------------------------------------

function M.new(opts)
    local cfg = opts.settings or {}
    local S = {
        active = false, t0 = 0, ticks = 0, no_mp = 0,
        pc = nil, ri = nil, rng = nil, mgr = nil, adv = nil, cc = nil,
        in_combat = false, simturns = false,
        last_tick = nil, recv = 0,
        seen = {}, first_seen = {}, last_snap_u = nil, last_hash = nil,
    }
    local ids = { hooks = {}, events = {}, timers = {} }
    local T = { info = nil }

    local function counters()
        local ri, rng = S.ri, S.rng
        local u = ri and try(function() return ri.mUnitActionsCounter end) or -1
        local r = rng and try(function() return rng.mCounter end) or -1
        return u, r
    end
    T.counters = counters

    local function emit(tag, detail, important)
        if not S.active then return end
        local u, r = counters()
        local line = fmt("%10.3f u=%d r=%d %s%s", os.clock() - S.t0, u, r, tag, detail and (" " .. detail) or "")
        opts.on_line(line, tag, important or M.IMPORTANT[tag] == true)
    end

    local function start(wi, pc)
        local mode = try(function() return wi.NetMode end)
        local role = mode == NM_LISTEN and "host" or mode == NM_CLIENT and "client" or ("nm" .. tostring(mode))
        local info = { role = role, player = player_of(pc), simturns = S.simturns,
            map = tostring(try(function() return wi:GetMapName(false) end)) }
        info.header = fmt("# mpsync role=%s player=%s map=%s simturns=%s started=%s", role, info.player, info.map,
            tostring(info.simturns), os.date("%Y-%m-%d %H:%M:%S"))
        S.active, S.t0 = true, os.clock()
        S.seen, S.first_seen, S.last_snap_u, S.last_hash = {}, {}, nil, nil
        T.info = info
        if opts.on_start then opts.on_start(info) end
    end

    local function stop(reason)
        if not S.active then return end
        S.active = false
        T.info = nil
        if opts.on_stop then opts.on_stop(reason) end
    end

    local function snapshot(force, why)
        if not S.active then return end
        local lines = M.state_lines(S.adv, S.cc, S.in_combat)
        if not lines then return end
        local h = M.hash(lines)
        S.last_snap_u = counters()
        if h == S.last_hash and not force then
            emit("HASH", "h=" .. h)
            return
        end
        S.last_hash = h
        emit("STATE", fmt("h=%s lines=%d%s", h, #lines, why and (" why=" .. why) or ""))
        for _, l in ipairs(lines) do opts.on_line("  | " .. l, "|", false) end
    end
    T.snapshot = snapshot

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
                emit("STUCK", fmt("%s uac=%s %s (counter is already %d)", qname, cu, desc, u))
            elseif now - first > 15 and not S.seen["wait:" .. key] then
                S.seen["wait:" .. key] = true
                emit("WAIT", fmt("%s uac=%s %s queued for %.0f s", qname, cu, desc, now - first))
            end
        end
    end

    local function watchdog()
        local mgr = S.mgr
        if not mgr then return end
        local u = counters()
        local now = os.clock()
        pcall(scan, "commands", mgr.mCommandQueue, function(c) return c.UnitActionsCounter end,
            function(c) return fmt("cmd=%s src=%s", ename("UC", c.CommandType), id_of(c.CommandSource)) end, u, now)
        pcall(scan, "instant", mgr.mInstantCommandQueue, function(c) return c.UnitActionsCounter end,
            function(c) return "type=" .. ename("ICT", c.Type) end, u, now)
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
            -- Map changes and loading screens look like "no game" for a moment; keep one trace per session.
            S.no_mp = S.no_mp + 1
            if S.no_mp >= 20 then stop("not in a multiplayer game") end
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
        if not S.active then start(wi, pc) end
        if S.ticks % 2 == 0 then watchdog() end
        if cfg.snapshots and counters() ~= S.last_snap_u and idle() then snapshot(false) end
    end

    ids.timers[#ids.timers + 1] = h7.every(1, function() refresh(false) end)
    ids.events[#ids.events + 1] = h7.on("map_loaded", function(map)
        emit("MAP", tostring(map))
        S.pc, S.adv, S.cc = nil, nil, nil
        refresh(true)
    end)

    -- Frame gaps and bursts. The game stops ticking while its window is unfocused, so a gap shows on return.
    ids.events[#ids.events + 1] = h7.on("tick", function(dt)
        local now = os.clock()
        local last = S.last_tick
        S.last_tick = now
        if not S.active then
            S.recv = 0
            return
        end
        if last and now - last >= (cfg.gap_seconds or 0.5) then
            emit("GAP", fmt("real=%.2fs dt=%.3fs", now - last, dt or -1), now - last >= 2)
        end
        if S.recv >= 3 then emit("BURST", fmt("%d commands arrived in one frame", S.recv)) end
        S.recv = 0
    end)

    -- Hooks -------------------------------------------------------------------------------------------------------

    local missing = {}
    local function hook(path, fn, post)
        local ok, id = pcall(post and h7.hook_post or h7.hook, "MMH7Game." .. path, fn)
        if ok then ids.hooks[#ids.hooks + 1] = id
        else missing[#missing + 1] = path .. " (" .. tostring(id) .. ")" end
    end

    -- Counters
    hook("H7ReplicationInfo.IncUnitActionsCounter", function()
        if not S.active then return end
        local ri = S.ri
        emit("STEP", fmt("id=%s gs=%s", try(function() return ri.mIdCounter end),
            try(function() return ri.mGameStateCounter end)))
    end, true)
    hook("H7ReplicationInfo.SetUnitActionsCounter", function(_, a)
        if not S.active then return end
        emit("UACSET", fmt("to=%s", a.Value))
    end)
    hook("H7ReplicationInfo.ResetIDCounterToNewId", function(_, a)
        if not S.active then return end
        emit("IDSET", fmt("to=%s", a.newIdCounter))
    end)

    -- Synced random numbers
    hook("H7SynchRNG.GetRandomFloat", function(_, _, v)
        if not S.active or not cfg.trace_rng then return end
        emit("RNG", fmt("F=%.6f", v or -1))
    end, true)
    hook("H7SynchRNG.GetRandomInt", function(_, a, v)
        if not S.active or not cfg.trace_rng then return end
        emit("RNG", fmt("I(%s)=%s", a.Max, v))
    end, true)
    hook("H7SynchRNG.GetRandomIntRange", function(_, a, v)
        if not S.active or not cfg.trace_rng then return end
        emit("RNG", fmt("R(%s,%s)=%s", a.Min, a.Max, v))
    end, true)
    hook("H7SynchRNG.SetCounter", function(_, a)
        if not S.active then return end
        emit("RNGSET", fmt("to=%s", a.newCounter))
    end)
    hook("H7SynchRNG.RefillPool", function()
        if not S.active then return end
        emit("REFILL")
    end, true)

    -- Sending
    hook("H7CombatPlayerController.SendPlayCommand", function(_, a)
        if not S.active then return end
        emit("SEND", fmt("uac=%s cmd=%s tag=%s src=%s tgt=%s path=%s player=%s oos=%s", a.unitActionCounter,
            ename("UC", a.CommandType), ename("TAG", a.CommandTag), id_of(a.Source), id_of(a.Target),
            try(function() return #a.Path end), a.CurrentPlayer, tostring(a.doOOSCheck)))
    end)
    hook("H7CombatPlayerController.SendInstantCommand", function(_, a)
        if not S.active then return end
        local c = a.Command
        emit("ISEND", fmt("uac=%s type=%s player=%s combat=%s tactics=%s to=%s", c.UnitActionsCounter,
            ename("ICT", c.Type), c.CurrentPlayer, tostring(c.IsCombatCommand), tostring(c.IsTactics), a.ToPlayer))
    end)
    hook("H7InstantCommandManager.StartCommand", function(_, a, ok)
        if not S.active then return end
        emit("ISTART", fmt("%s ok=%s", class_of(a.Command), tostring(ok)))
    end, true)

    -- The host forwarding commands (server functions only run on the host)
    local function relay(kind)
        return function(pc, a)
            if not S.active then return end
            local p = a.Parameters or {}
            emit("RELAY", fmt("%s from=%s uac=%s cmd=%s src=%s combat=%s to=%s", kind, player_of(pc), p[1],
                ename("UC", p[2]), p[3], tostring(a.IsCombat), a.ToPlayer))
        end
    end
    hook("H7CombatPlayerController.ServerPlayUnitCommand", relay("cmd"))
    hook("H7CombatPlayerController.ServerPlayUnitCommandWithPath", relay("cmd+path"))
    hook("H7CombatPlayerController.ServerPlayUnitCommandWithExtPath", relay("cmd+extpath"))
    hook("H7CombatPlayerController.ServerInstantCommand", function(pc, a)
        if not S.active then return end
        emit("RELAY", fmt("instant from=%s uac=%s type=%s combat=%s tactics=%s to=%s", player_of(pc),
            a.unitActionCounter, ename("ICT", a.instantCommandType), tostring(a.IsCombat), tostring(a.IsTactics),
            a.ToPlayer))
    end)

    -- Receiving
    hook("H7CombatPlayerController.ClientPlayUnitCommandConvertedPath", function(_, a)
        if not S.active then return end
        S.recv = S.recv + 1
        local p = a.Parameters or {}
        local u = counters()
        local late = not S.simturns and p[1] ~= nil and p[1] < u
        emit(late and "LATE" or "RECV", fmt("uac=%s cmd=%s tag=%s src=%s tgt=%s combat=%s", p[1], ename("UC", p[2]),
            ename("TAG", p[6]), p[3], p[5], tostring(a.IsCombat)))
    end)
    hook("H7CombatPlayerController.ClientInstantCommand", function(_, a)
        if not S.active then return end
        S.recv = S.recv + 1
        local u = counters()
        local late = not S.simturns and a.unitActionCounter ~= nil and a.unitActionCounter < u
        emit(late and "ILATE" or "IRECV", fmt("uac=%s type=%s player=%s combat=%s", a.unitActionCounter,
            ename("ICT", a.instantCommandType), a.CurrentPlayer, tostring(a.IsCombat)))
    end)
    hook("H7MultiplayerCommandManager.AddCommand", function(_, a)
        if not S.active then return end
        emit("QUEUE", fmt("uac=%s cmd=%s src=%s combat=%s", a.unitActionCounter, ename("UC", a.Command),
            id_of(a.CommandSource), tostring(a.IsCombat)))
    end)
    hook("H7MultiplayerCommandManager.AddInstantCommand", function(_, a)
        if not S.active then return end
        local c = a.instantCommand
        emit("IQUEUE", fmt("uac=%s type=%s player=%s", c.UnitActionsCounter, ename("ICT", c.Type), c.CurrentPlayer))
    end)
    hook("H7MultiplayerCommandManager.UnqueuePreparedCommands", function(mgr)
        if not S.active then return end
        local qc, qi = mgr.mQueuedMPCommands, mgr.mQueuedMPInstantCommands
        local u = counters()
        emit("UNQUEUE", fmt("cmds=%d instant=%d", #qc, #qi))
        for i = 1, #qc do
            local p = try(function() return qc[i].Parameters end) or {}
            if p[1] and p[1] < u then
                emit("DROP?", fmt("cmd uac=%s cmd=%s src=%s (counter %d)", p[1], ename("UC", p[2]), p[3], u))
            end
        end
        for i = 1, #qi do
            local c = qi[i]
            if c.UnitActionsCounter < u then
                emit("DROP?", fmt("instant uac=%s type=%s (counter %d)", c.UnitActionsCounter, ename("ICT", c.Type), u))
            end
        end
    end)

    -- Running
    hook("H7CommandQueue.Enqueue", function(_, a)
        if not S.active then return end
        local c = a.Cmd
        if not c then return end
        emit("CMD", fmt("cmd=%s tag=%s src=%s tgt=%s path=%s mp=%s",
            ename("UC", try(function() return c:GetCommand() end)),
            ename("TAG", try(function() return c:GetCommandTag() end)),
            id_of(try(function() return c:GetCommandSource() end)),
            id_of(try(function() return c:GetAbilityTarget() end)), #(try(function() return c:GetPath() end) or {}),
            tostring(try(function() return c:IsMPSynchronized() end))))
    end)
    hook("H7InstantCommandManager.ExecuteInstantCommand", function(_, a)
        if not S.active then return end
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
        if not S.active then return end
        local_check("combat")
    end)
    hook("H7AdventurePlayerController.CheckOutOfSynch", function()
        if not S.active or S.in_combat then return end -- in combat it calls the combat version (hooked above)
        local_check("adventure")
    end)
    hook("H7MultiplayerCommandManager.AddOutOfSynchData", function(_, a)
        if not S.active then return end
        emit("RCHECK", fmt("from=%s uac=%s rng=%s id=%s units=%s res=%s combat=%s", a.PlayerNumber,
            a.unitActionCounter, a.SynchRNG, a.IDCounter, a.UnitsCount, a.ResCount, tostring(a.IsCombat)))
    end)
    hook("H7MultiplayerCommandManager.GameWentOOSMessage", function(_, a)
        if not S.active then return end
        emit("OOS", fmt("%s (uac %s)", a.msg, a.uac))
        snapshot(true, "oos")
    end)
    hook("H7MultiplayerCommandManager.GameWentOOS", function(_, a)
        if not S.active then return end
        emit("OOSTYPE", fmt("type=%s synchUpPlayer=%s", ename("OOST", a.oosType), a.synchUpPlayer))
    end)
    hook("H7MultiplayerCommandManager.StartSynchUp", function()
        if not S.active then return end
        emit("SYNCHSTART")
    end)
    hook("H7CombatPlayerController.ServerGameWentOOS", function(pc, a)
        if not S.active then return end
        emit("SYNCHREQ", fmt("from=%s type=%s uac=%s to=%s synchUpPlayer=%s", player_of(pc), ename("OOST", a.oosType),
            a.uac, a.ToPlayer, a.synchUpPlayer))
    end)
    hook("H7CombatPlayerController.ClientStartSynchUp", function(_, a)
        if not S.active then return end
        emit("SYNCHASK", fmt("type=%s", ename("OOST", a.oosType)))
    end)
    hook("H7CombatPlayerController.ClientSynchUp", function(_, a)
        if not S.active then return end
        emit("SYNCH", fmt("uac=%s rng=%s id=%s combat=%s", a.unitActionCounter, a.RNGCounter, a.IDCounter,
            tostring(a.IsCombat)))
    end)
    hook("H7CombatPlayerController.ClientSynchUpArmy", function(_, a)
        if not S.active then return end
        emit("SYNCHARMY", fmt("uac=%s hero=%s stacks=%s", a.unitActionCounter, a.heroId, a.stackIDStrings))
    end)
    hook("H7CombatPlayerController.ClientSynchUpCombatUnit", function(_, a)
        if not S.active then return end
        emit("SYNCHUNIT", fmt("uac=%s stack=%s count=%s hp=%s", a.unitActionCounter, a.StackId, a.StackCount,
            a.TopCreatureHealth))
    end)
    hook("H7CombatPlayerController.ClientGameWentOOSMessage", function(_, a)
        if not S.active then return end
        emit("OOSREMOTE", fmt("%s (uac %s)", a.Message, a.uac))
    end)
    hook("H7CombatPlayerController.ClientCommandCancelled", function(_, a)
        if not S.active then return end
        emit("CANCELLED", fmt("reason=%s", a.Reason))
    end)

    -- Turn flow, tactics and sim-turn combat requests (timer-driven paths)
    hook("H7AdventureController.EndMyTurn", function()
        if not S.active then return end
        emit("ENDTURN")
    end)
    hook("H7CombatPlayerController.SendTacticsPhaseFinished", function(_, a)
        if not S.active then return end
        emit("TACTICSSEND", fmt("defender=%s to=%s", tostring(a.IsDefender), a.ToPlayer))
    end)
    hook("H7CombatPlayerController.ClientTacticsPhaseFinished", function(_, a)
        if not S.active then return end
        emit("TACTICSRECV", fmt("defender=%s", tostring(a.IsDefender)))
    end)
    hook("H7MultiplayerCommandManager.InsertOngoingStartCombat", function(_, a)
        if not S.active then return end
        emit("STARTCOMBAT", fmt("source=%s target=%s", id_of(a.Source), id_of(a.Target)))
    end)
    hook("H7MultiplayerCommandManager.HandleAnswerStartCombat", function(_, a)
        if not S.active then return end
        emit("STARTCOMBATANSWER", fmt("answer=%s hero=%s sync=%s", a.answer, a.heroId, tostring(a.doSynchronise)))
    end)
    hook("H7MultiplayerCommandManager.FinishOngoingStartCombat", function(_, a)
        if not S.active then return end
        emit("STARTCOMBATEND", fmt("retreating=%s retreatFinished=%s", tostring(a.isRetreating),
            tostring(a.isRetreatFinished)))
    end)
    hook("H7SimTurnCommandManager.FinishOngoingStartCombat", function(_, a)
        if not S.active then return end
        emit("SIMCOMBATEND", fmt("retreatFinished=%s canceled=%s normalCombat=%s", tostring(a.isRetreatFinished),
            tostring(a.isCombatCanceled), tostring(a.IsNormalCombatAboutToBegin)))
    end)

    T.missing = missing -- functions that could not be hooked (callers log them)

    function T.destroy()
        stop("tracer removed")
        for _, id in ipairs(ids.hooks) do pcall(h7.unhook, id) end
        for _, id in ipairs(ids.events) do pcall(h7.off, id) end
        for _, id in ipairs(ids.timers) do pcall(h7.cancel, id) end
        ids = { hooks = {}, events = {}, timers = {} }
    end

    function T.active() return S.active end

    return T
end

return M
