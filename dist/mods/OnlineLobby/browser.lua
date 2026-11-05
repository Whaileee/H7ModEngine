-- The online game browser: the game's own map selection window (H7MapSelect movie, the same window the Mods menu
-- uses) filled with the lobby's games while it is opened from Online > Join. Everything changed in the window is
-- put back when it closes.
--
-- Columns: Host | Map | Players | Version (edition + engine) | Status (Compatible / Differs).
-- Right: the map's picture in the frame (from the game's content scanner, like the map window does it); below it
-- the game's description and "Compatible with your game." or what differs.
-- Continue = Join, Generate Map = Refresh, Back = main menu.
--
-- The window code follows dist/mods/ModManager/main.lua (see its CLAUDE.md for the rules learnt there: look clips up
-- by name every time, restore everything, no ALL-CAPS row texts).

local B = {}

local Obj = h7.find_class("GFxUI.GFxObject")
local SELECT = "MMH7Game.H7MapSelectCntl."

local HEADERS = { mLblMapName = "Host", mLblVictoryCondition = "Map", mLblPlayers = "Players",
    mLblMapSize = "Version", mLblMapType = "Status" }
-- The map filters would filter our texts; "no maps found" is the map list's own (the panel says when no games are
-- listed); the map details are replaced by our description. Hidden after the list is filled: an empty list shows
-- them again. The picture frame stays: it shows the selected game's map.
local UNUSED = { "mMapDetails", "mMapList.mDDCondition", "mMapList.mDDPlayerAmount", "mMapList.mDDMapSize",
    "mMapList.mDDMapType", "mMapList.mLblNoMaps", "mLblNoMaps" }
local AREA = { x = 1342, y = 640, w = 470 }  -- the details panel below the picture (window coordinates, 1920x1080)
local LABELS = { "mOnlineInfo", "mOnlineDiff" }
local MAX_DIFF_LINES = 10

-- Survives a hot reload of the mod while the main menu lives.
_G.__onlinebrowser = _G.__onlinebrowser or {}
local ui = _G.__onlinebrowser

-- Set by main.lua: fetch(isDuel, fn(games, err)), compatibility(game) -> issues, join(game, issues).
B.fetch, B.compatibility, B.join = nil, nil, nil

local function child(parent, path)
    for key in path:gmatch("[^.]+") do
        if not parent then return nil end
        parent = parent:GetObject(key, Obj)
    end
    return parent
end
local function select_cntl() return h7.first("H7MapSelectCntl") end
local function window() local s = select_cntl() return s and s.mMapSelection end

local function change(path, field, value, kind)
    local o = child(window(), path)
    if not o then return end
    ui.originals = ui.originals or {}
    local key = path .. "|" .. field
    if ui.originals[key] == nil then
        ui.originals[key] = { path = path, field = field, kind = kind, value = o["Get" .. kind](o, field) }
    end
    o["Set" .. kind](o, field, value)
end

local function hide(name)
    local o = child(window(), name)
    if o then o:SetBool("visible", false) end
end

local function label(name, text, x, y, w, size)
    local w0 = window()
    local o = child(w0, name) or w0:AttachMovie("H7Label", name, -1, Obj)
    o:SetBool("multiline", true)
    o:SetBool("wordWrap", true)
    o:SetFloat("size", size)
    o:SetFloat("width", w)
    o:SetString("text", text)
    o:SetFloat("x", x)
    o:SetFloat("y", y)
    o:SetBool("visible", true)
end

local function restore()
    ui.showing = false
    ui.thumb = nil
    for _, n in ipairs(LABELS) do pcall(hide, n) end
    for _, s in pairs(ui.originals or {}) do
        local ok, err = pcall(function()
            local o = child(window(), s.path)
            if o then o["Set" .. s.kind](o, s.field, s.value) end
        end)
        if not ok then h7.warn(("could not restore %s.%s: %s"):format(s.path, s.field, tostring(err))) end
    end
end

-- Row texts go through the movie's localisation: ALL-CAPS words would be taken for keys, so none are used.
local function players_text(g) return ("%d/%d"):format(g.players or 1, g.slots or 2) end
-- The game's name for the map ("Pustynny kanion 2"), else the file name.
local function map_text(g)
    local map
    if B.setting_texts then map = select(3, B.setting_texts(g)) end
    if type(map) == "string" and map ~= "" and not map:find("^%[") then return map end
    return tostring(g.map)
end

local function version_text(g) return ("%s %s"):format(g.edition or "base", tostring(g.engine_version or "?")) end

-- The map's entry in the game's content scanner (duels use combat maps), and its map data.
local function find_map(g)
    local scanner = h7.find_class("MMH7Game.H7TransitionData"):get_default():GetInstance():GetContentScanner()
    local want = tostring(g.map):upper()
    local lists = { { scanner.mCollection_AdventureData, "AdventureMapData" }, { scanner.mCollection_CombatData, "CombatMapData" } }
    if g.type == "duel" then lists[1], lists[2] = lists[2], lists[1] end
    for _, l in ipairs(lists) do
        local arr, field = l[1], l[2]
        for i = 1, #arr do
            local e = arr[i]
            if tostring(e.Filename):upper() == want then return e, e[field] end
        end
    end
end

-- Like H7MapSelectCntl.MapClicked: stream the thumbnail into the window's texture, then update it every frame.
local function show_thumbnail(g)
    local sel = select_cntl()
    local w = sel.mMapSelection
    local ok, err = pcall(function()
        local e, data = find_map(g)
        if not (e and data and data.mIsThumbnailDataAvailable) then
            ui.thumb = nil
            return w:NoThumbnailAvailable()
        end
        local tex = sel.mMapThumbnail
        -- No return value: the results are the out parameters (texRawData, isTextureReinitialized).
        local _, reinit = tex:SwitchStreamingTo(e.Filename, data.mThumbnailData, 0)
        ui.thumb = true
        if reinit == 1 then w:ReloadThumbnail() end
        w:ShowThumbnail()
    end)
    if not ok then
        h7.warn("Online browser: no map picture for " .. tostring(g.map) .. ": " .. tostring(err))
        pcall(function() w:NoThumbnailAvailable() end)
    end
end

-- Stream the picture in, then stop: updating a finished texture every frame re-uploads it and makes the screen
-- flicker (measured: ~140 updates/s, 2026-11-05).
h7.on("tick", function()
    if not (ui.showing and ui.thumb) then return end
    local ok, ready = pcall(function()
        local tex = select_cntl().mMapThumbnail
        tex:PerformUpdate()
        return tex:IsTextureReady()
    end)
    if not ok or ready then ui.thumb = nil end
end)

local function show_details(index)
    local row = ui.rows and ui.rows[index]
    ui.selected = index
    if not row then
        ui.thumb = nil
        pcall(function() window():NoThumbnailAvailable() end)
        label("mOnlineInfo", ui.status or "No games listed. Use Refresh to look again.", AREA.x, AREA.y, AREA.w, 18)
        hide("mOnlineDiff")
        return
    end
    show_thumbnail(row.game)
    local g, issues = row.game, row.issues
    local st = type(g.settings) == "table" and g.settings or {}
    local turns, skills
    if B.setting_texts then turns, skills = B.setting_texts(g) end
    local rules = {}
    if turns and turns ~= "" then rules[#rules + 1] = "Turns: " .. turns end
    if skills and skills ~= "" then rules[#rules + 1] = "Skills: " .. skills end
    local info = {
        g.name .. "  -  " .. map_text(g),
        "Players: " .. players_text(g) .. (#rules > 0 and ("    " .. table.concat(rules, "    ")) or ""),
        ("Game %s    %s    H7ModEngine %s"):format(tostring(g.game_version), tostring(g.edition or "base"),
            tostring(g.engine_version)),
    }
    if st.saved_game == true then info[#info + 1] = "Continues a saved game" end
    label("mOnlineInfo", table.concat(info, "\n"), AREA.x, AREA.y, AREA.w, 18)
    local diff
    if #issues == 0 then
        diff = "Compatible with your game."
    else
        local lines = { "Differs from your game:" }
        for i = 1, math.min(#issues, MAX_DIFF_LINES) do lines[#lines + 1] = "- " .. issues[i] end
        if #issues > MAX_DIFF_LINES then lines[#lines + 1] = ("- and %d more"):format(#issues - MAX_DIFF_LINES) end
        diff = table.concat(lines, "\n")
    end
    label("mOnlineDiff", diff, AREA.x, AREA.y + 24 * #info + 12, AREA.w, 16)
    local b = child(window(), "mBtnContinue")
    if b then b:SetString("text", "Join") end
end

local function fill(games)
    local sel = select_cntl()
    local list = sel.mMapSelection
    list:Reset()
    list:Update(sel.mMapThumbnail)
    ui.showing = true
    change("mLblTitle", "text", ui.duel and "Online duels" or "Online games", "String")
    for key, text in pairs(HEADERS) do change(key, "text", text, "String") end
    change("mBtnContinue", "text", child(list, "mBtnContinue"):GetString("text"), "String") -- set per row
    change("mGenerateMap", "text", "Refresh", "String")

    local rows = sel:CreateArray()
    ui.rows = {}
    for n, g in ipairs(games or {}) do
        local issues = B.compatibility(g)
        local o = sel:CreateObject("Object", Obj)
        o:SetInt("Index", n - 1)
        o:SetString("Name", g.name)
        local vc = sel:CreateArray()
        vc:SetElementString(0, map_text(g))
        o:SetObject("VictoryConditions", vc)
        o:SetString("PlayerCount", players_text(g))
        o:SetString("Size", version_text(g))
        o:SetString("Type", #issues == 0 and "Compatible" or "Differs")
        rows:SetElementObject(n - 1, o)
        ui.rows[n - 1] = { game = g, issues = issues }
    end
    list:SetObject("mPolledMaps", rows)
    list:Invoke("AddMaps") -- selects the first row (MapClicked)
    list:Invoke("ListingMapDone")
    for _, key in ipairs(UNUSED) do change(key, "visible", false, "Bool") end
    if #(games or {}) == 0 then show_details(nil) end
    h7.log(("Online browser: %d game(s)"):format(#(games or {})))
end

local function refresh()
    ui.status = "Looking for games..."
    fill({})
    B.fetch(ui.duel, function(games, err)
        if not ui.showing then return end
        ui.status = err and ("The online lobby is not reachable (" .. err .. ").") or nil
        restore()
        fill(games or {})
    end)
end

-- Opens the browser for duels or skirmish games (from the main menu's Online > Join).
function B.open(cntl, isDuel)
    ui.duel = isDuel
    cntl.mMainMenu:SetVisibleSave(false)
    refresh()
end

function B.showing() return ui.showing end

h7.hook(SELECT .. "MapClicked", function(self, args)
    if not ui.showing then return end
    show_details(args.clickedMapIndex)
    return true -- no map thumbnail to load
end)

h7.hook(SELECT .. "BtnContinueClicked", function(self, args)
    if not ui.showing then return end
    local row = ui.rows and ui.rows[args.selectedListIndex]
    if row then B.join(row.game, row.issues) end
    return true -- don't start a map
end)

h7.hook(SELECT .. "BtnGenerateMapClicked", function()
    if not ui.showing then return end
    restore()
    refresh()
    return true
end)

h7.hook(SELECT .. "BtnBackClicked", function()
    if ui.showing then restore() end -- then the game closes the window the usual way
end)
h7.hook(SELECT .. "Update", function()
    if ui.showing then restore() end -- the game opens the window for maps
end)

function B.close()
    if ui.showing then restore() end
end

return B
