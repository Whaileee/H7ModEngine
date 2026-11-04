-- ModManager: a "Mods" entry in the main menu that lists every mod with its description and lets players
-- enable or disable them and change their settings. The list is the game's own map selection window (H7MapSelect
-- movie), filled with mods instead of maps while it is opened from here; everything changed in it is put back
-- when it closes.
--
-- Two views share the window:
--   mods      one row per mod. Continue = Enable/Disable, the (otherwise unused) Generate Map button = Settings
--             for mods that declare settings; the details panel lists the current values.
--   settings  one row per setting of a mod. Continue = Change (next value), Generate Map = Default,
--             Back = back to the mod list.

local Obj = h7.find_class("GFxUI.GFxObject")
local Clik = h7.find_class("GFxUI.GFxClikWidget")
local SELECT = "MMH7Game.H7MapSelectCntl."

-- Column headers of the map list. Rows fill the columns from their Name, VictoryConditions[0], PlayerCount, Size
-- and Type fields. VictoryConditions is left out: the details panel would list it as "victory conditions", and
-- renaming that caption is cached by the movie for the game's own map list too. So the second column stays empty
-- and the author is shown as the details' "Creator".
local HEADERS = {
    mods = { mLblMapName = "Mod", mLblVictoryCondition = "", mLblPlayers = "Version", mLblMapSize = "Category",
             mLblMapType = "Status" },
    settings = { mLblMapName = "Setting", mLblVictoryCondition = "", mLblPlayers = "Value", mLblMapSize = "Range",
                 mLblMapType = "" },
}
-- Parts of the window that mods don't use. The picture frame shows the game's "no picture" text instead.
local UNUSED = { "mMapThumbnail", "mMapList.mDDCondition" }

-- State that has to survive reloads of this mod lives as long as the main menu.
_G.__modmanager = _G.__modmanager or {}
local ui = _G.__modmanager

-- GFxObjects returned by GetObject/AttachMovie are garbage collected by the engine when nothing in
-- UnrealScript holds them (the Flash clips stay), so they are looked up by name whenever they are used.
-- `path` may name nested parts: "mMapList.mDDCondition".
local function child(parent, path)
    for key in path:gmatch("[^.]+") do
        if not parent then return nil end
        parent = parent:GetObject(key, Obj)
    end
    return parent
end
local function select_cntl() return h7.first("H7MapSelectCntl") end
local function window() return select_cntl().mMapSelection end

-- "Failed": enabled but not loaded (the log says why).
local function status_of(m)
    if not m.enabled then return "Disabled" end
    return m.loaded and "Enabled" or "Failed"
end

local function mod_by_name(name)
    for _, m in ipairs(h7.mods()) do
        if m.name == name then return m end
    end
end

local function setting_by_key(modName, key)
    for _, s in ipairs(h7.mod_settings(modName)) do
        if s.key == key then return s end
    end
end

-- Texts shown for setting values. Row texts go through the movie's localization: all-caps words would be taken
-- for keys, so values are never upper-cased.
local function num(v)
    if v == math.floor(v) and math.abs(v) < 1e15 then return ("%d"):format(v) end
    return (("%.3f"):format(v):gsub("0+$", ""):gsub("%.$", ""))
end
local function value_text(s, v)
    if v == nil then v = s.value end
    if s.type == "bool" then return v and "On" or "Off" end
    if s.type == "number" then return num(v) end
    return tostring(v)
end
local function range_text(s)
    if s.type == "bool" then return "On / Off" end
    if s.type == "choice" then return table.concat(s.options, " / ") end
    if s.type == "number" then
        if s.min and s.max then return num(s.min) .. " - " .. num(s.max) end
        if s.min then return ">= " .. num(s.min) end
        if s.max then return "<= " .. num(s.max) end
        return "any number"
    end
    return "text"
end

-- The value after `s.value` when the player presses Change: toggles, cycles the options, or steps a number up
-- (wrapping around to the minimum).
local function next_value(s)
    if s.type == "bool" then return not s.value end
    if s.type == "choice" then
        for i, o in ipairs(s.options) do
            if o == s.value then return s.options[i % #s.options + 1] end
        end
        return s.options[1]
    end
    if s.type == "number" then
        local step = s.step or ((s.min and s.max) and (s.max - s.min) / 10 or 1)
        local v = s.value + step
        if s.max and v > s.max + step * 1e-6 then v = s.min or s.max end
        return v
    end
    return nil -- text settings can't be edited here
end

local function set_label(path, text)
    local b = child(window(), path)
    if b then b:SetString("text", text) end
end

local function set_visible(path, on)
    local o = child(window(), path)
    if o then o:SetBool("visible", on) end
end

-- Changes a window part, remembering its original value the first time (once per game session), so restore()
-- puts the window back even after a reload of this mod or a failed restore.
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

local function restore()
    ui.showing = false
    for _, s in pairs(ui.originals or {}) do
        local ok, err = pcall(function()
            local o = child(window(), s.path)
            if o then o["Set" .. s.kind](o, s.field, s.value) end
        end)
        if not ok then h7.warn(("could not restore %s.%s: %s"):format(s.path, s.field, tostring(err))) end
    end
end

local function add_row(sel, rows, n, fields)
    local o = sel:CreateObject("Object", Obj)
    o:SetInt("Index", n)
    for k, v in pairs(fields) do o:SetString(k, v) end
    rows:SetElementObject(n, o)
end

local function settings_summary(m)
    local lines = {}
    for _, s in ipairs(h7.mod_settings(m.name)) do
        lines[#lines + 1] = ("%s: %s"):format(s.label, value_text(s))
    end
    if #lines == 0 then return "" end
    return "\n\nSettings:\n" .. table.concat(lines, "\n")
end

local function fill_mods(sel, list, rows)
    local n = 0
    for _, m in ipairs(h7.mods()) do
        if not m.hidden then
            add_row(sel, rows, n, {
                Name = m.name,
                Creator = m.author ~= "" and m.author or "-",
                Description = m.description .. settings_summary(m),
                PlayerCount = m.version ~= "" and m.version or "-",
                Size = m.category ~= "" and m.category or "-",
                Type = status_of(m),
            })
            ui.rows[n] = m.name
            n = n + 1
        end
    end
    return n
end

local function fill_settings(sel, list, rows)
    local n = 0
    for _, s in ipairs(h7.mod_settings(ui.mod)) do
        local about = s.description ~= "" and (s.description .. "\n\n") or ""
        about = about .. "Default: " .. value_text(s, s.default)
        if s.reload then about = about .. "\nChanging it restarts the mod." end
        add_row(sel, rows, n, {
            Name = s.label,
            Creator = ui.mod,
            Description = about,
            PlayerCount = value_text(s),
            Size = range_text(s),
            Type = s.value == s.default and "-" or "Changed",
        })
        ui.rows[n] = s.key
        n = n + 1
    end
    return n
end

-- Rows can't be changed once added, so every change fills the list again (the selection goes back to the
-- first row; AddMaps sorts the rows by name).
local function fill()
    local sel = select_cntl()
    local list = sel.mMapSelection
    list:Reset()
    list:Update(sel.mMapThumbnail)
    ui.showing = true
    local view = ui.view or "mods"
    change("mLblTitle", "text", view == "mods" and "Mods" or ("Settings: " .. ui.mod), "String")
    for key, text in pairs(HEADERS[view]) do change(key, "text", text, "String") end
    for _, key in ipairs(UNUSED) do change(key, "visible", false, "Bool") end
    change("mLblNoThumbnail", "visible", true, "Bool")
    change("mBtnContinue", "text", child(list, "mBtnContinue"):GetString("text"), "String") -- set per row below
    change("mGenerateMap", "text", child(list, "mGenerateMap"):GetString("text"), "String")
    change("mGenerateMap", "visible", false, "Bool")
    local rows = sel:CreateArray()
    ui.rows = {} -- row index (from 0, as the window reports it) -> mod name or setting key
    local n = view == "mods" and fill_mods(sel, list, rows) or fill_settings(sel, list, rows)
    list:SetObject("mPolledMaps", rows)
    list:Invoke("AddMaps") -- selects the first row (MapClicked)
    list:Invoke("ListingMapDone")
    h7.log(("Mods window: %d %s"):format(n, view == "mods" and "mods" or ("settings of " .. ui.mod)))
end

local function open()
    ui.cntl.mMainMenu:SetVisibleSave(false)
    ui.view, ui.mod = "mods", nil
    fill()
end

local function refill()
    restore()
    fill()
end

-- Buttons for the selected row.
local function on_row(index)
    ui.selected = index
    local id = ui.rows and ui.rows[index]
    if ui.view == "settings" then
        local s = id and setting_by_key(ui.mod, id)
        local editable = s and next_value(s) ~= nil
        set_label("mBtnContinue", editable and "Change" or "-")
        set_label("mGenerateMap", "Default")
        set_visible("mGenerateMap", s ~= nil and s.value ~= s.default)
    else
        local m = id and mod_by_name(id)
        set_label("mBtnContinue", (m and m.enabled) and "Disable" or "Enable")
        set_label("mGenerateMap", "Settings")
        set_visible("mGenerateMap", m ~= nil and m.settings > 0)
    end
end

-- The window reports clicks to its controller; while it shows mods they are handled here instead.
h7.hook(SELECT .. "MapClicked", function(self, args)
    if not ui.showing then return end
    on_row(args.clickedMapIndex)
    return true -- no map thumbnail to load
end)

-- Continue: enable/disable the selected mod, or change the selected setting.
h7.hook(SELECT .. "BtnContinueClicked", function(self, args)
    if not ui.showing then return end
    local id = ui.rows[args.selectedListIndex]
    if ui.view == "settings" then
        local s = id and setting_by_key(ui.mod, id)
        local v = s and next_value(s)
        if v ~= nil then
            h7.set_mod_setting(ui.mod, s.key, v)
            refill()
        end
    else
        local m = mod_by_name(id)
        if m then h7.set_mod_enabled(m.name, not m.enabled) end -- the list is refilled on mod_toggled
    end
    return true -- don't start a map
end)

h7.on("mod_toggled", function()
    if ui.showing and ui.view == "mods" then refill() end
end)

-- Generate Map: Settings (mod list) or Default (settings list).
h7.hook(SELECT .. "BtnGenerateMapClicked", function()
    if not ui.showing then return end
    local id = ui.selected and ui.rows[ui.selected]
    if ui.view == "settings" then
        local s = id and setting_by_key(ui.mod, id)
        if s then h7.set_mod_setting(ui.mod, s.key, s.default) end
        refill()
    elseif id and mod_by_name(id) and mod_by_name(id).settings > 0 then
        ui.view, ui.mod = "settings", id
        refill()
    end
    return true
end)

-- Back: from a mod's settings back to the mod list; from the mod list it closes the window the usual way (back
-- to the main menu), after putting the window back as it was.
h7.hook(SELECT .. "BtnBackClicked", function()
    if not ui.showing then return end
    if ui.view == "settings" then
        ui.view, ui.mod = "mods", nil
        refill()
        return true
    end
    restore()
end)
h7.hook(SELECT .. "Update", function()
    if ui.showing then restore() end -- the game opens the window for maps
end)

-- The Mods button in the main menu. Its clicks arrive at H7MainMenuCntl.Options without event data, so the
-- hook checks whether the mouse is over it.
local function under_mouse(o)
    if not (o and o:GetBool("visible")) then return false end
    local mx, my = o:GetFloat("mouseX"), o:GetFloat("mouseY")
    local w, h = o:GetFloat("width") / o:GetFloat("scaleX"), o:GetFloat("height") / o:GetFloat("scaleY")
    return mx >= 0 and my >= 0 and mx < w and my < h
end

h7.hook("MMH7Game.H7MainMenuCntl.Options", function(cntl, args)
    if ui.cntl and under_mouse(child(ui.cntl.mMainMenu, "mBtnMods")) then
        open()
        return true -- don't open the options menu
    end
end)

-- The main menu's sub-menus (single player, multiplayer, extras...) hide or slide away the movie's own buttons,
-- which the movie tracks in a list our button isn't in. So the Mods button copies the Options button's
-- visibility, transparency and horizontal position.
local function follow_options()
    local cntl = ui.cntl
    if not (cntl and cntl:is_valid()) then return end
    local menu = cntl.mMainMenu
    local options, mods = child(menu, "mBtnOptions"), child(menu, "mBtnMods")
    if not (options and mods) then return end
    local visible, alpha, x = options:GetBool("visible"), options:GetFloat("alpha"), options:GetFloat("x")
    if mods:GetBool("visible") ~= visible then mods:SetBool("visible", visible) end
    if mods:GetFloat("alpha") ~= alpha then mods:SetFloat("alpha", alpha) end
    if mods:GetFloat("x") ~= x then mods:SetFloat("x", x) end
end

-- Adds the Mods button to the main menu: Extras and Options move up into the free slot above them, Mods takes
-- Options' place and Quit stays at the bottom of the frame.
local function install()
    local cntl = h7.first("H7MainMenuCntl")
    local menu = cntl and cntl.mMainMenu
    if not menu then return end
    if ui.cntl ~= cntl then -- a new main menu
        for k in pairs(ui) do ui[k] = nil end
        ui.cntl = cntl
    end
    ui.open = open -- for tests\modwindow_autotest.lua
    if not child(menu, "mBtnMods") then
        local extras, options = child(menu, "mBtnExtras"), child(menu, "mBtnOptions")
        local step = options:GetFloat("y") - extras:GetFloat("y")
        local button = menu:AttachMovie("H7TextButton", "mBtnMods", -1, Clik)
        button:AddEventListener("CLIK_click", { object = cntl, func = "Options" })
        button:SetString("htmlText", "Mods")
        for _, f in ipairs({ "x", "y", "width", "height" }) do button:SetFloat(f, options:GetFloat(f)) end
        extras:SetFloat("y", extras:GetFloat("y") - step)
        options:SetFloat("y", options:GetFloat("y") - step)
    end
    h7.log("Mods button installed")
end

h7.on("map_loaded", function(map)
    if map == "councilhub" then h7.after(1, install) else ui.cntl = nil end
end)
h7.every(0.1, function()
    if ui.cntl then pcall(follow_options) end
end)
install() -- already at the main menu (hot reload)
