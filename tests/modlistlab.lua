-- Mods list experiment: opens the map selection window (H7MapSelect movie) and fills its list with mods.
-- Copy into <Game>\H7ModEngine\exec\ at the main menu; Back closes the window.
local function T(fmt, ...) h7.log("[modlab] " .. fmt:format(...)) end
local Obj = h7.find_class("GFxUI.GFxObject") -- omitted optional args are 0/none, so pass them explicitly
local sel = h7.first("H7MapSelectCntl")
local list = sel.mMapSelection
T("cntl %s list %s", tostring(sel), tostring(list))

-- Block the window's calls into the map logic while experimenting.
for _, id in ipairs(_G.modlab_hooks or {}) do h7.unhook(id) end
_G.modlab_hooks = {}
for _, f in ipairs({ "MapClicked", "BtnContinueClicked", "BtnGenerateMapClicked" }) do
  table.insert(_G.modlab_hooks, h7.hook("MMH7Game.H7MapSelectCntl." .. f, function(self, args)
    T("%s index=%s", f, tostring(args.clickedMapIndex or args.selectedListIndex))
    return true
  end))
end

-- Close ModManager's popup if it is open (it is drawn above this window).
local mm = _G.__modmanager
if mm and mm.popup_open then
  mm.popup_open = false
  local p = mm.cntl.mRootMC:GetObject("mModsPopup", Obj)
  if p then p:SetBool("visible", false) end
end

h7.first("H7MainMenuCntl").mMainMenu:SetVisibleSave(false)
list:Reset() -- drop the rows of an earlier run
list:Update(sel.mMapThumbnail)

-- Every field the game's AddMap/AddCustomCampaigns set, with a marker showing where each ends up.
local arr = sel:CreateArray()
local n = 0
for _, m in ipairs(h7.mods()) do
  if not m.hidden then
    local o = sel:CreateObject("Object", Obj)
    o:SetInt("Index", n)
    o:SetString("Name", m.name)
    o:SetString("Creator", "C:" .. m.author)
    o:SetString("Description", m.description)
    local vc = sel:CreateArray()
    vc:SetElementString(0, "V:" .. m.author)
    local lc = sel:CreateArray()
    lc:SetElementString(0, "L:" .. m.version)
    o:SetObject("VictoryConditions", vc)
    o:SetObject("LoseConditions", lc)
    o:SetString("PlayerCount", m.version)
    o:SetString("Size", "S:size")
    o:SetString("Type", m.enabled and "Enabled" or "Disabled")
    arr:SetElementObject(n, o)
    n = n + 1
  end
end
list:SetObject("mPolledMaps", arr)
list:Invoke("AddMaps")
list:Invoke("ListingMapDone")
T("added %d", n)

local function geo(o)
  if not o then return "nil" end
  return ("x=%.0f y=%.0f w=%.0f h=%.0f vis=%s"):format(o:GetFloat("x"), o:GetFloat("y"), o:GetFloat("width"),
    o:GetFloat("height"), tostring(o:GetBool("visible")))
end
for _, k in ipairs({ "mLblTitle", "mLblMapName", "mLblVictoryCondition", "mLblPlayers", "mLblMapSize", "mLblMapType",
  "mDropDifficulty", "mCustomDifficulty", "mMapThumbnail", "mLblNoThumbnail", "mMapList", "mMapDetails",
  "mBtnContinue", "mBtnBack", "mGenerateMap", "mLoadingAnimationMaps" }) do
  local o = list:GetObject(k, Obj)
  T("%-22s %s %s", k, geo(o), o and tostring(o:GetString("text")) or "")
end
local ml = list:GetObject("mMapList", Obj)
for _, k in ipairs({ "mDDCondition", "mDDPlayerAmount", "mDDMapSize", "mDDMapType", "mBtnClearFilters",
  "mBtnSortMapNameUp", "mBtnSortMapNameDown", "mLblNoMaps", "mScrollBar" }) do
  T("  mMapList.%-18s %s", k, geo(ml and ml:GetObject(k, Obj)))
end
return "ok"
