-- OnlineLobby: makes the main menu's Online multiplayer work again without Ubisoft's (shut down) services.
--
-- Online games run on the game's own LAN code path: the host's PC is the server and players connect to it
-- directly. The lobby server (H7Lobby) only lists the games:
--   hosting  CreateOnlineGame(IsLAN=false) is switched to LAN, then the game is registered with the lobby
--            (UPnP opens port 27015 first) and kept alive with a heartbeat until it starts or the host leaves.
--   finding  Online > Join opens browser.lua: the map selection window (like the Mods menu) listing the lobby's
--            games: Host | Map | Players | Version (edition + engine) | Status (Compatible / Differs).
--   joining  the chosen game becomes the manager's only search result, with a native LAN session record pointing
--            at the host (h7.net.set_lan_host), and goes through the game's JoinSelected -> JoinOnlineGame ->
--            GetResolvedConnectString -> "start ip:port". A game whose version, engine, edition, content packs
--            (CookedPC) or gameplay mods differ asks first, naming what differs.
-- Design notes: docs/ONLINE.md in the H7ModEngine repository. Lobby API: README of H7Lobby.

local json = h7.json
local LOBBY = tostring(h7.config("Online", "LobbyUrl", "https://h7-lobby.whaileee.workers.dev")):gsub("/+$", "")
local LOBBY_API = 1      -- the lobby API version this mod speaks
local PORT = 27015
local GAME_VERSION = "2.2.1" -- the only build the engine supports
local MP = "MMH7Game.H7MultiplayerGameManager."

local state = {
    content = nil,         -- h7.game_content() result (community content packs in CookedPC)
    hostRequested = false, -- an Online game is being created
    host = nil,            -- { id, token, timer, ... } while our game is in the lobby
    confirm = nil,         -- the lobby game while the "join anyway?" popup is open
}

local function log(fmt, ...) h7.log(fmt:format(...)) end

local function manager() return h7.first("H7MultiplayerGameManager") end

local function request_popup()
    local ok, p = pcall(function() return h7.first("H7RequestPopupCntl"):GetRequestPopup() end)
    return ok and p or nil
end

local function popup(text)
    local p = request_popup()
    if p then pcall(function() p:OKPopup(text, "OK") end) end
end

h7.game_content(function(c)
    state.content = c
    log("Online: this game is %s (%d content pack(s)), engine %s", c.modded and "modded" or "base", #c.packs, h7.version)
end)

-- ---------------------------------------------------------------------------------------------------------------
-- What this game runs, and comparing it with a listed game
-- ---------------------------------------------------------------------------------------------------------------

-- Lua mods that change gameplay: loaded, not hidden, not tools.
local function gameplay_mods()
    local list = {}
    for _, m in ipairs(h7.mods()) do
        if m.loaded and not m.hidden and m.category ~= "Developer" and m.name ~= "OnlineLobby" then
            list[#list + 1] = { name = m.name, version = tostring(m.version or "") }
        end
    end
    table.sort(list, function(a, b) return a.name < b.name end)
    return list
end

local function content_packs()
    local list = {}
    for _, p in ipairs(state.content and state.content.packs or {}) do
        list[#list + 1] = { name = p.name, version = p.version, files = p.files }
    end
    return list
end

local function edition() return (state.content and state.content.modded) and "modded" or "base" end

-- Lines describing how list `theirs` differs from `ours` ({name, version}), e.g. "mods/Conflux: different version".
local function diff_lists(kind, ours, theirs, out)
    local mine, other = {}, {}
    for _, m in ipairs(ours or {}) do mine[m.name] = m.version end
    for _, m in ipairs(theirs or {}) do other[m.name] = m.version end
    local names = {}
    for n in pairs(mine) do names[#names + 1] = n end
    for n in pairs(other) do if mine[n] == nil then names[#names + 1] = n end end
    table.sort(names)
    for _, n in ipairs(names) do
        local a, b = mine[n], other[n]
        if a == nil then
            out[#out + 1] = ("%s %s: only the host has it (%s)"):format(kind, n, tostring(b))
        elseif b == nil then
            out[#out + 1] = ("%s %s: the host doesn't have it"):format(kind, n)
        elseif a ~= b then
            out[#out + 1] = ("%s %s: host %s, yours %s"):format(kind, n, tostring(b), tostring(a))
        end
    end
end

local function compatibility(g)
    local issues = {}
    if g.game_version ~= GAME_VERSION then
        issues[#issues + 1] = ("Game version: host %s, yours %s"):format(tostring(g.game_version), GAME_VERSION)
    end
    if g.engine_version ~= h7.version then
        issues[#issues + 1] = ("H7ModEngine: host %s, yours %s"):format(tostring(g.engine_version), h7.version)
    end
    local theirEdition = g.edition or "base"
    if theirEdition ~= edition() then
        issues[#issues + 1] = ("Edition: host %s, yours %s"):format(theirEdition, edition())
    end
    if (g.content_fingerprint or "") ~= ((state.content and state.content.fingerprint) or "") then
        diff_lists("Content", content_packs(), g.content, issues)
    end
    diff_lists("Mod", gameplay_mods(), g.mods, issues)
    return issues
end

local function api(method, path, body, token, fn)
    local headers = token and { Authorization = "Bearer " .. token } or nil
    return h7.http.request({ url = LOBBY .. path, method = method, body = body, headers = headers, timeout = 10 }, function(res)
        local data = res.ok and res.body ~= "" and json.decode(res.body) or nil
        if fn then fn(res, data) end
    end)
end

-- ---------------------------------------------------------------------------------------------------------------
-- Menu: the Online entries refuse to open while not logged in to Ubisoft's (gone) services. Allow them.
-- ---------------------------------------------------------------------------------------------------------------

h7.hook("MMH7Game.H7MainMenuCntl.CheckConnection", function() return true end) -- skip the Ubisoft checks
h7.hook_post("MMH7Game.H7MainMenuCntl.CheckConnection", function() return true end) -- and report "connected"

-- ---------------------------------------------------------------------------------------------------------------
-- Hosting
-- ---------------------------------------------------------------------------------------------------------------

local function stop_hosting(reason)
    local h = state.host
    if not h then return end
    state.host = nil
    if h.timer then h7.cancel(h.timer) end
    log("Online game %s removed from the lobby (%s)", h.id or "(not listed yet)", reason)
    if h.id then api("DELETE", "/v1/games/" .. h.id, nil, h.token) end
    if h.upnpOk then h7.net.upnp_remove(PORT, "UDP") end
end

local function player_count()
    local ok, n = pcall(function() return #h7.first("H7ReplicationInfo").PRIArray end)
    return ok and n or nil
end

local function describe_game(settings, isDuel)
    return {
        name = (settings:GetServerName() or ""):sub(1, 40),
        type = isDuel and "duel" or "skirmish",
        map = settings:GetMapFilepath(),
        map_info = settings:GetMapInfoNumber(),
        slots = math.max(2, math.min(8, settings.NumPublicConnections)),
        players = 1,
        port = PORT,
        game_version = GAME_VERSION,
        engine_version = h7.version,
        edition = edition(),
        content = content_packs(),
        content_fingerprint = state.content and state.content.fingerprint or "",
        mods = gameplay_mods(),
        settings = {
            turn_type = settings:GetTurnType(),
            skill_type = settings:GetSkillType(),
            closed_slots = settings:GetNumClosedSlots(),
            ai_slots = settings:GetNumAISlots(),
            saved_game = settings:isSavedGame(),
            rmg = settings:GetRMGString(),
        },
    }
end

local function heartbeat()
    local h = state.host
    if not h or not h.id then return end
    api("POST", "/v1/games/" .. h.id .. "/heartbeat", { players = player_count() }, h.token, function(res)
        if res.status == 404 and state.host == h then
            -- The lobby dropped us (e.g. heartbeats were late while the game window was in the background).
            log("Lobby forgot our game; registering again")
            h.id = nil
            h.register()
        end
    end)
end

local function publish(isDuel)
    local settings = manager():GetOnlineGameSettings()
    if not settings then return log("Online: no game settings after creating the game; not listed") end
    stop_hosting("replaced")
    local h = { upnp = mod.settings.upnp }
    state.host = h
    local body = describe_game(settings, isDuel)

    function h.register()
        api("POST", "/v1/games", body, nil, function(res, data)
            if state.host ~= h then return end -- the host left meanwhile
            if res.status ~= 201 or not data then
                local why = data and data.error or res.error or ("HTTP " .. res.status)
                log("Online: lobby registration failed: %s", why)
                popup("Could not list the game in the online lobby (" .. why .. "). Players on your LAN can still join.")
                return
            end
            h.id, h.token = data.id, data.token
            log("Online game listed in the lobby as %s at %s:%d", data.id, data.ip, data.port)
            if not h.upnpOk then
                -- The lobby can't test UDP, so this is the host's only hint that nobody outside can connect.
                local why = h.upnp and ("your router didn't open it automatically (" .. tostring(h.upnpError) .. ")")
                    or "opening it automatically (UPnP) is off in the OnlineLobby settings"
                popup(("Your game is listed online. Players outside your network can join only if UDP port %d "
                    .. "reaches this PC: %s. Forward UDP %d to %s in your router, or play over a virtual LAN.")
                    :format(PORT, why, PORT, h7.net.local_ip() or "this PC"))
            end
        end)
    end

    -- The host's game listens on UDP 27015 only (no TCP listener, measured 2026-11-05), so only UDP is mapped.
    if h.upnp then
        h7.net.upnp_add(PORT, "UDP", function(r)
            h.upnpOk, h.upnpError = r.ok, r.error
            if r.ok then log("UPnP: the router forwards UDP %d to this PC", PORT)
            else log("UPnP UDP %d: %s", PORT, tostring(r.error)) end
            if r.external_ip and r.external_ip ~= "" then body.ipv4 = r.external_ip end
            if state.host == h then h.register() end
        end, "Heroes VII online game")
    else
        h.register()
    end
    h.timer = h7.every(15, heartbeat)
end

h7.hook(MP .. "CreateOnlineGame", function(self, args)
    state.hostRequested = not args.IsLAN
    if state.hostRequested then
        state.hostDuel = args.IsDuel
        args.IsLAN = true -- host on the LAN path: no Ubisoft session, no Storm sockets
        log("Online: hosting a %s through the LAN path", args.IsDuel and "duel" or "skirmish")
    end
end)

h7.hook_post(MP .. "OnCreateH7GameSessionComplete", function(self, args)
    if not state.hostRequested then return end
    state.hostRequested = false
    if args.bWasSuccessful then publish(state.hostDuel) end
end)

-- Leaving the lobby or starting the game takes it off the list.
h7.hook_post("MMH7Game.H7ReplicationInfo.UpdateOnlineGameStarted", function() stop_hosting("game started") end)
h7.hook_post("IpDrv.OnlineGameInterfaceImpl.DestroyOnlineGame", function() stop_hosting("session ended") end)
h7.on("map_loaded", function(map)
    if map:lower() == "councilhub" then stop_hosting("back in the main menu") end
end)

-- ---------------------------------------------------------------------------------------------------------------
-- Finding games: Online > Join opens the browser (browser.lua, the map selection window) with the lobby's games
-- ---------------------------------------------------------------------------------------------------------------

local browser = mod.require("browser")

browser.compatibility = compatibility

function browser.fetch(isDuel, fn)
    api("GET", "/v1/games?type=" .. (isDuel and "duel" or "skirmish"), nil, nil, function(res, data)
        if not (res.status == 200 and data and type(data.games) == "table") then
            local why = data and data.error or res.error or ("HTTP " .. res.status)
            log("Online: lobby unavailable: %s", why)
            return fn(nil, why)
        end
        if data.api ~= nil and data.api ~= LOBBY_API then
            log("Online: the lobby speaks API %s, this mod %d; listing what it understands", tostring(data.api), LOBBY_API)
        end
        local games = {}
        for _, g in ipairs(data.games) do
            if mod.settings.show_mismatched or #compatibility(g) == 0 then games[#games + 1] = g end
        end
        log("Online: %d game(s) in the lobby, %d listed", #data.games, #games)
        fn(games)
    end)
end

-- Map name, turn type and skill system as the game names them (from a settings object, like its own browser).
function browser.setting_texts(g)
    local st = type(g.settings) == "table" and g.settings or {}
    local ok, turns, skills, map = pcall(function()
        local s = h7.new_object("H7OnlineGameSettings")
        s:SetTurnType(tonumber(st.turn_type) or 0)
        s:SetSkillType(tonumber(st.skill_type) or 0)
        s:SetMapFilepath(tostring(g.map))
        s:SetMapInfoNumber(tonumber(g.map_info) or 0)
        s:SetGameType(g.type == "duel" and 1 or 0) -- duel maps are localised in another section
        return s:GetTurnTypeAsStringLocalized(), s:GetSkillTypeAsStringLocalized(), s:GetMapNameLocalized()
    end)
    if ok then return turns, skills, map end
end

for _, f in ipairs({ { "MPJoinINET", false }, { "MPDuelJoinINET", true } }) do
    h7.hook("MMH7Game.H7MainMenuCntl." .. f[1], function(cntl)
        browser.open(cntl, f[2])
        return true -- instead of the game's (Ubisoft) browser
    end)
end

-- ---------------------------------------------------------------------------------------------------------------
-- Joining: one search result that points at the host, joined through the game's own JoinSelected
-- ---------------------------------------------------------------------------------------------------------------

local function join(g)
    local m = manager()
    if not m then return end
    local st = type(g.settings) == "table" and g.settings or {}
    local s = h7.new_object("H7OnlineGameSettings")
    s.bIsLanMatch = true -- joins through the LAN path, never Ubisoft's
    s:SetNumPublicConnections(g.slots)
    s.NumOpenPublicConnections = math.max(0, g.slots - (g.players or 1))
    s:SetServerName(g.name)
    s:SetMapFilepath(g.map)
    s:SetMapInfoNumber(g.map_info or 0)
    s:SetGameType(g.type == "duel" and 1 or 0)
    s:SetTurnType(tonumber(st.turn_type) or 0)
    s:SetSkillType(tonumber(st.skill_type) or 0)
    s:SetNumClosedSlots(tonumber(st.closed_slots) or 0)
    s:SetNumAISlots(tonumber(st.ai_slots) or 0)
    s:SetIsSavedGame(st.saved_game == true)
    if type(st.rmg) == "string" and st.rmg ~= "" then s:SetRMGString(st.rmg) end
    m.SearchResults = {}
    m.SearchResults:add({ GameSettings = s })
    h7.net.set_lan_host(m.SearchResults[1], g.ip, g.port)
    log("Online: joining %s (%s) at %s:%d", g.name, g.id, g.ip, g.port)
    m:JoinSelected(0) -- map check, then JoinOnlineGame -> GetResolvedConnectString -> start ip:port
end

local MAX_LINES = 12

function browser.join(g, issues)
    if #issues == 0 then return join(g) end
    local p = request_popup()
    if not p then return join(g) end -- can't ask
    local lines = {}
    for i = 1, math.min(#issues, MAX_LINES) do lines[i] = "- " .. issues[i] end
    if #issues > MAX_LINES then lines[#lines + 1] = ("- and %d more (see the engine log)"):format(#issues - MAX_LINES) end
    log("Online: asking before joining %s: %s", g.name, table.concat(issues, "; "))
    state.confirm = g
    p:YesNoPopup(("This game may not be compatible with yours and could go out of sync:\n%s\n\nJoin anyway?")
        :format(table.concat(lines, "\n")), "Join anyway", "Cancel")
end

h7.hook_post("MMH7Game.H7GFxRequestPopup.YesButtonClicked", function()
    local g = state.confirm
    if not g then return end
    state.confirm = nil
    log("Online: joining anyway")
    h7.after(0, function() join(g) end)
end)

for _, f in ipairs({ "NoButtonClicked", "EscapePressed" }) do
    h7.hook_post("MMH7Game.H7GFxRequestPopup." .. f, function()
        if state.confirm then
            state.confirm = nil
            log("Online: join cancelled")
        end
    end)
end

function on_unload()
    stop_hosting("mod unloaded")
    browser.close()
end

log("OnlineLobby ready (lobby %s)", LOBBY)
