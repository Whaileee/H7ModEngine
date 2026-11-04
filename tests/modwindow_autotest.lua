-- Drives ModManager's Mods window without input: opens it, turns DailyGold on and off, goes back, then opens the
-- game's own map selection to check it was restored. Run at the main menu with tests/run_ingame.sh, which takes the
-- screenshots that "SHOT <name>" lines ask for.
local function T(fmt, ...) h7.log("[autotest] " .. fmt:format(...)) end
local Obj = h7.find_class("GFxUI.GFxObject")
local ui = _G.__modmanager
local sel = h7.first("H7MapSelectCntl")
local menu = h7.first("H7MainMenuCntl")
local list = sel.mMapSelection

local function child(parent, key) return parent and parent:GetObject(key, Obj) end
local function text(key) local o = child(list, key) return o and o:GetString("text") end
local function status_of(name)
    for _, m in ipairs(h7.mods()) do
        if m.name == name then return ("enabled=%s loaded=%s"):format(tostring(m.enabled), tostring(m.loaded)) end
    end
end
local function visible(path)
    local o = list
    for key in path:gmatch("[^.]+") do o = child(o, key) end
    return o and tostring(o:GetBool("visible")) or "nil"
end
-- DailyGold's row as the window holds it (the rows themselves can't be read).
local function dailygold_row()
    local rows = child(list, "mPolledMaps")
    for i = 0, (rows and rows:GetInt("length") or 0) - 1 do
        local r = rows:GetElementObject(i, Obj)
        if r:GetString("Name") == "DailyGold" then return r:GetString("Type") end
    end
    return "not listed"
end

-- The parts ModManager changes, as one line.
local function window_state()
    return ("title=%s col2=%s col2filter=%s continue=%s noThumb=%s thumb=%s generate=%s"):format(text("mLblTitle"),
        text("mLblVictoryCondition"), visible("mMapList.mDDCondition"), text("mBtnContinue"), visible("mLblNoThumbnail"),
        visible("mMapThumbnail"), visible("mGenerateMap"))
end

-- Continue on DailyGold's row. Only while the Mods window shows: for maps it would start a game.
local function toggle_dailygold()
    assert(ui.showing, "Mods window not showing")
    for i, name in pairs(ui.rows) do
        if name == "DailyGold" then return sel:BtnContinueClicked(i, false) end
    end
    error("DailyGold not listed")
end

-- Steps run one after another with a pause, so the window can draw and the watcher can take its screenshot.
local steps = {
    function()
        if ui.showing then sel:BtnBackClicked() end -- close a window left open
        T("open Mods window")
        ui.open()
    end,
    function()
        T("mods: %s", window_state())
        T("SHOT mods_open")
    end,
    function()
        T("DailyGold before: %s", status_of("DailyGold"))
        toggle_dailygold()
    end,
    function()
        T("DailyGold after enable: %s, row says %s; %s", status_of("DailyGold"), dailygold_row(), window_state())
        T("SHOT mods_enabled")
    end,
    function()
        toggle_dailygold()
    end,
    function()
        T("DailyGold after disable: %s, row says %s", status_of("DailyGold"), dailygold_row())
        sel:BtnBackClicked()
    end,
    function()
        T("back: menu visible=%s, window showing=%s", tostring(menu.mMainMenu:GetBool("visible")), tostring(ui.showing))
        menu:StartSPCustom(false) -- the game's own custom map selection
    end,
    function()
        T("maps: %s", window_state())
        T("SHOT maps_open")
    end,
    function()
        sel:BtnBackClicked()
        T("DONE")
    end,
}

local function run(i)
    if not steps[i] then return end
    local ok, err = pcall(steps[i])
    if not ok then T("step %d failed: %s", i, tostring(err)) end
    h7.after(2, function() run(i + 1) end)
end
run(1)
return "autotest started"
