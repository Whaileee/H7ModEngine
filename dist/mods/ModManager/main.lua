-- ModManager: a "Mods" entry in the main menu that lists every mod and lets players enable or disable them and
-- change their settings. The list is the game's own map selection window (H7MapSelect movie), filled with mods
-- instead of maps while it is opened from here; everything changed in it is put back when it closes.
--
-- Left: one row per mod (name, version, category, status). Continue = Enable/Disable.
-- Right (panel.lua): the selected mod's description in the picture frame, its settings as sliders, check boxes
-- and arrows below it, numbers that are typed in a number box. Changes are saved at once.

local Obj = h7.find_class("GFxUI.GFxObject")
local Clik = h7.find_class("GFxUI.GFxClikWidget")
local SELECT = "MMH7Game.H7MapSelectCntl."
local panel = mod.require("panel")

-- Column headers of the map list. Rows fill the columns from their Name, VictoryConditions[0], PlayerCount, Size
-- and Type fields. VictoryConditions is left out (the second column stays empty): renaming its caption would be
-- cached by the movie for the game's own map list too.
local HEADERS = { mLblMapName = "Mod", mLblVictoryCondition = "", mLblPlayers = "Version", mLblMapSize = "Category",
    mLblMapType = "Status" }
-- Parts of the window that mods don't use: the picture, its "no picture" text, the map details (the panel takes
-- their place), Generate Map and the second column's filter.
local UNUSED = { "mMapThumbnail", "mLblNoThumbnail", "mMapDetails", "mGenerateMap", "mMapList.mDDCondition" }

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
    pcall(panel.hide_all)
    for _, s in pairs(ui.originals or {}) do
        local ok, err = pcall(function()
            local o = child(window(), s.path)
            if o then o["Set" .. s.kind](o, s.field, s.value) end
        end)
        if not ok then h7.warn(("could not restore %s.%s: %s"):format(s.path, s.field, tostring(err))) end
    end
end

-- Rows can't be changed once added, so enabling or disabling a mod fills the list again (the selection goes back
-- to the first row; AddMaps sorts the rows by name). Row texts go through the movie's localization: all-caps
-- words would be taken for keys.
local function fill()
    local sel = select_cntl()
    local list = sel.mMapSelection
    list:Reset()
    list:Update(sel.mMapThumbnail)
    ui.showing = true
    change("mLblTitle", "text", "Mods", "String")
    for key, text in pairs(HEADERS) do change(key, "text", text, "String") end
    for _, key in ipairs(UNUSED) do change(key, "visible", false, "Bool") end
    change("mBtnContinue", "text", child(list, "mBtnContinue"):GetString("text"), "String") -- set per row
    local rows = sel:CreateArray()
    ui.rows = {} -- row index (from 0, as the window reports it) -> mod name
    local n = 0
    for _, m in ipairs(h7.mods()) do
        if not m.hidden then
            local o = sel:CreateObject("Object", Obj)
            o:SetInt("Index", n)
            o:SetString("Name", m.name)
            o:SetString("PlayerCount", m.version ~= "" and m.version or "-")
            o:SetString("Size", m.category ~= "" and m.category or "-")
            o:SetString("Type", status_of(m))
            rows:SetElementObject(n, o)
            ui.rows[n] = m.name
            n = n + 1
        end
    end
    list:SetObject("mPolledMaps", rows)
    list:Invoke("AddMaps") -- selects the first row (MapClicked)
    list:Invoke("ListingMapDone")
    h7.log(("Mods window: %d mods"):format(n))
end

local function open()
    ui.cntl.mMainMenu:SetVisibleSave(false)
    fill()
end

-- A row was selected: its description and settings on the right, Enable/Disable on Continue.
local function on_row(index)
    ui.selected = index
    local m = ui.rows and mod_by_name(ui.rows[index])
    local b = child(window(), "mBtnContinue")
    if b then b:SetString("text", (m and m.enabled) and "Disable" or "Enable") end
    panel.show(m)
end

-- The window reports clicks to its controller; while it shows mods they are handled here instead.
h7.hook(SELECT .. "MapClicked", function(self, args)
    if not ui.showing then return end
    on_row(args.clickedMapIndex)
    return true -- no map thumbnail to load
end)

h7.hook(SELECT .. "BtnContinueClicked", function(self, args)
    if not ui.showing then return end
    local m = mod_by_name(ui.rows[args.selectedListIndex])
    if m then h7.set_mod_enabled(m.name, not m.enabled) end -- the list is filled again on mod_toggled
    return true -- don't start a map
end)

h7.on("mod_toggled", function()
    if ui.showing then
        restore()
        fill()
    end
end)

h7.hook(SELECT .. "BtnGenerateMapClicked", function()
    if ui.showing then return true end
end)

-- Back closes the window the usual way (back to the main menu); first put the window back as it was.
h7.hook(SELECT .. "BtnBackClicked", function()
    if ui.showing then restore() end
end)
h7.hook(SELECT .. "Update", function()
    if ui.showing then restore() end -- the game opens the window for maps
end)

-- Mouse and keyboard for the settings controls (Flash doesn't report which of our clips was clicked). While a
-- number box is being edited it takes the keys; the post-hook then reports them as handled, so the game and the
-- menu don't act on them (Escape would close the window).
h7.hook("Engine.Console.InputKey", function(console, args)
    ui.key_used = false
    if not ui.showing then return end
    if args.Key == "LeftMouseButton" then
        if args.Event == panel.PRESSED then
            pcall(panel.press)
        elseif args.Event == panel.RELEASED then
            pcall(panel.release)
        end
        return
    end
    local ok, used = pcall(panel.key, args.Key, args.Event)
    ui.key_used = ok and used
end)
h7.hook_post("Engine.Console.InputKey", function(console, args, handled)
    if ui.key_used then
        ui.key_used = false
        return true
    end
end)
h7.on("tick", function()
    if ui.showing then pcall(panel.tick) end
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

-- The main menu's sub-menus (single player, multiplayer, extras...) hide the movie's own buttons, which the movie
-- tracks in a list our button isn't in. So the Mods button copies the Options button's visibility,
-- transparency and horizontal position.
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
    ui.panel = panel
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
