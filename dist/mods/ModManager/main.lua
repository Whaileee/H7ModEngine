-- ModManager: a "Mods" entry in the main menu that lists every mod with its description and lets players
-- enable or disable them. The list is the game's own map selection window (H7MapSelect movie), filled with
-- mods instead of maps while it is opened from here; everything changed in it is put back when it closes.

local Obj = h7.find_class("GFxUI.GFxObject")
local Clik = h7.find_class("GFxUI.GFxClikWidget")
local SELECT = "MMH7Game.H7MapSelectCntl."

-- Column headers of the map list, in order, and the row fields that fill each column.
local HEADERS = { mLblMapName = "Mod", mLblVictoryCondition = "Author", mLblPlayers = "Version",
    mLblMapSize = "Category", mLblMapType = "Status" }
-- Parts of the window that mods don't use.
local UNUSED = { "mMapThumbnail", "mLblNoThumbnail", "mGenerateMap" }

-- State that has to survive reloads of this mod lives as long as the main menu.
_G.__modmanager = _G.__modmanager or {}
local ui = _G.__modmanager

-- GFxObjects returned by GetObject/AttachMovie are garbage collected by the engine when nothing in
-- UnrealScript holds them (the Flash clips stay), so they are looked up by name whenever they are used.
local function child(parent, key) return parent and parent:GetObject(key, Obj) end
local function select_cntl() return h7.first("H7MapSelectCntl") end
local function window() return select_cntl().mMapSelection end

local function status_text(enabled) return enabled and "Enabled" or "Disabled" end

local function mod_by_name(name)
    for _, m in ipairs(h7.mods()) do
        if m.name == name then return m end
    end
end

-- Remembers a window part's current value (once per opening) so close() can put it back.
local function change(key, field, value, kind)
    local o = child(window(), key)
    if not o then return end
    local get, set = "Get" .. kind, "Set" .. kind
    ui.saved[#ui.saved + 1] = { key = key, field = field, kind = kind, value = o[get](o, field) }
    o[set](o, field, value)
end

local function restore()
    for i = #ui.saved, 1, -1 do
        local s = ui.saved[i]
        local o = child(window(), s.key)
        if o then o["Set" .. s.kind](o, s.field, s.value) end
    end
    ui.saved = {}
    ui.showing = false
end

local function set_continue_label(enabled)
    local b = child(window(), "mBtnContinue")
    if b then b:SetString("text", enabled and "Disable" or "Enable") end
end

local function fill(sel, list)
    local rows = sel:CreateArray()
    ui.rows = {} -- row index (from 0, as the window reports it) -> mod name
    local n = 0
    for _, m in ipairs(h7.mods()) do
        if not m.hidden then
            local i = n
            n = n + 1
            ui.rows[i] = m.name
            local o = sel:CreateObject("Object", Obj)
            o:SetInt("Index", i)
            o:SetString("Name", m.name)
            o:SetString("Creator", m.author)
            o:SetString("Description", m.description)
            local author = sel:CreateArray()
            author:SetElementString(0, m.author)
            o:SetObject("VictoryConditions", author)
            local version = sel:CreateArray()
            version:SetElementString(0, m.version)
            o:SetObject("LoseConditions", version)
            o:SetString("PlayerCount", m.version)
            o:SetString("Size", (m.category or "") ~= "" and m.category or "-")
            o:SetString("Type", status_text(m.enabled))
            rows:SetElementObject(i, o)
        end
    end
    list:SetObject("mPolledMaps", rows)
    list:Invoke("AddMaps")
    list:Invoke("ListingMapDone")
    h7.log(("Mods window: %d mods"):format(n))
end

local function open()
    local sel = select_cntl()
    local list = sel.mMapSelection
    ui.cntl.mMainMenu:SetVisibleSave(false)
    list:Reset()
    list:Update(sel.mMapThumbnail)
    ui.showing, ui.saved = true, {}
    change("mLblTitle", "text", "Mods", "String")
    for key, text in pairs(HEADERS) do change(key, "text", text, "String") end
    for _, key in ipairs(UNUSED) do change(key, "visible", false, "Bool") end
    -- The details take the picture's place.
    local thumb = child(list, "mMapThumbnail")
    if thumb then change("mMapDetails", "y", thumb:GetFloat("y"), "Float") end
    ui.saved[#ui.saved + 1] = { key = "mBtnContinue", field = "text", kind = "String",
        value = child(list, "mBtnContinue"):GetString("text") }
    fill(sel, list)
end

-- The window reports clicks to its controller; while it shows mods they are handled here instead.
h7.hook(SELECT .. "MapClicked", function(self, args)
    if not ui.showing then return end
    local m = mod_by_name(ui.rows[args.clickedMapIndex])
    if m then set_continue_label(m.enabled) end
    local details = child(window(), "mMapDetails")
    local added = details and child(details, "mChildsAdded")
    if added and not ui.logged_details then -- which labels the details panel shows, to rename them
        ui.logged_details = true
        for i = 0, added:GetInt("length") - 1 do
            local c = added:GetElementObject(i, Obj)
            h7.log(("details child %d: %s"):format(i, c and tostring(c:GetString("text")) or "nil"))
        end
    end
    return true -- no map thumbnail to load
end)

h7.hook(SELECT .. "BtnContinueClicked", function(self, args)
    if not ui.showing then return end
    local m = mod_by_name(ui.rows[args.selectedListIndex])
    if not m then return true end
    h7.set_mod_enabled(m.name, not m.enabled)
    local enabled = not m.enabled
    set_continue_label(enabled)
    local item = child(child(window(), "mMapList"), "mSelectedMapListItem")
    if item then
        local lbl = child(item, "mLblMapType")
        if lbl then lbl:SetString("text", status_text(enabled)) end
        local data = child(item, "mData")
        if data then data:SetString("Type", status_text(enabled)) end -- what the Status filter sees
    end
    return true -- don't start a map
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
    if map == "councilhub" then h7.after(1, install) end
end)
install() -- already at the main menu (hot reload)
