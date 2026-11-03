-- Main menu UI experiment: adds a "Mods" button between Options and Quit and opens a popup when it is clicked.
-- Copy into <Game>\H7ModEngine\exec\ at the main menu; safe to re-run (it reuses what it created).
local function T(fmt, ...) h7.log("[menulab] " .. fmt:format(...)) end
local Clik = h7.find_class("GFxUI.GFxClikWidget")
local Obj = h7.find_class("GFxUI.GFxObject") -- omitted optional args are 0/none, so pass them explicitly

local cntl = h7.first("H7MainMenuCntl")
local mm = cntl.mMainMenu

local function geo(o)
  if not o then return "nil" end
  return ("x=%.0f y=%.0f w=%.0f h=%.0f sx=%.2f visible=%s"):format(o:GetFloat("x"), o:GetFloat("y"),
    o:GetFloat("width"), o:GetFloat("height"), o:GetFloat("scaleX"), tostring(o:GetBool("visible")))
end

-- Clicks on our buttons arrive at H7MainMenuCntl.Options with empty EventData, so find the button under the mouse.
local function under_mouse(o)
  if not o:GetBool("visible") then return false, "hidden" end
  local mx, my = o:GetFloat("mouseX"), o:GetFloat("mouseY")
  local w, h = o:GetFloat("width") / o:GetFloat("scaleX"), o:GetFloat("height") / o:GetFloat("scaleY")
  return mx >= 0 and my >= 0 and mx < w and my < h, ("mouse %.0f,%.0f in %.0fx%.0f"):format(mx, my, w, h)
end

-- 1) the button: Extras and Options move up one slot into the empty space, Mods takes Options' place, Quit stays
local btn = _G.menulab_btn
if not btn then
  local extras, options = mm:GetObject("mBtnExtras", Obj), mm:GetObject("mBtnOptions", Obj)
  local step = options:GetFloat("y") - extras:GetFloat("y")
  btn = mm:AttachMovie("H7TextButton", "mBtnMods", -1, Clik)
  _G.menulab_btn = btn
  btn:SetString("text", "Mods")
  btn:SetFloat("width", options:GetFloat("width"))
  btn:SetFloat("height", options:GetFloat("height"))
  btn:SetFloat("x", options:GetFloat("x"))
  btn:SetFloat("y", options:GetFloat("y"))
  extras:SetFloat("y", extras:GetFloat("y") - step)
  options:SetFloat("y", options:GetFloat("y") - step)
  btn:AddEventListener("CLIK_click", { object = cntl, func = "Options" })
end
for _, n in ipairs({ "mBtnTutorial", "mBtnExtras", "mBtnOptions", "mBtnMods", "mBtnQuit" }) do
  T("%-12s %s", n, geo(mm:GetObject(n, Obj)))
end

-- 2) the popup
local function open_popup()
  local root = cntl.mRootMC
  local popup = _G.menulab_popup
  if not popup then
    popup = root:AttachMovie("H7CustomDifficultyPopUp", "mModsPopup", -1, Obj)
    _G.menulab_popup = popup
    T("popup created: %s", tostring(popup))
  end
  popup:SetBool("visible", true)
  T("popup %s numChildren=%s", geo(popup), tostring(popup:GetInt("numChildren")))
  -- Timeline children of a freshly attached clip may only exist after the next frame.
  h7.after(0.5, function()
    T("popup after 0.5s %s numChildren=%s", geo(popup), tostring(popup:GetInt("numChildren")))
    for _, n in ipairs({ "mBG", "mTitle", "mCloseButton", "mStartResources", "mNeutralStrength", "mNeutralGrowth",
      "mAICheating", "mBtnOK", "mLblGeneralDifficulty", "mDDGeneralDifficulty" }) do
      T("  %-22s %s", n, geo(popup:GetObject(n, Obj)))
    end
    local title = popup:GetObject("mTitle", Obj)
    if title then title:SetString("text", "Mods") end
  end)
end

if _G.menulab_hook then h7.unhook(_G.menulab_hook) end
_G.menulab_hook = h7.hook("MMH7Game.H7MainMenuCntl.Options", function(self, args)
  local ours, why = under_mouse(btn)
  local _, why_opt = under_mouse(mm:GetObject("mBtnOptions", Obj))
  T("Options called: mods button %s (%s); options button %s", tostring(ours), why, why_opt)
  if not ours then return end
  open_popup()
  return true
end)
return "ok"
