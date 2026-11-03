-- Call tracer for the day-change and damage code paths: copy into <Game>\H7ModEngine\exec\ on an adventure map,
-- then end a day and fight a manual battle. Every 3s logs which traced functions ran since the last report.
local function T(fmt, ...) h7.log("[trace] " .. fmt:format(...)) end
local NATIVE, ITERATOR, LATENT, DEFINED, DELEGATE = 0x400, 0x4, 0x8, 0x2, 0x100000

local classes = { "MMH7Game.H7Calendar", "MMH7Game.H7GameProcessor", "MMH7Game.H7CombatResult" }
local extra = {
  "MMH7Game.H7Town.OnBeginDay", "MMH7Game.H7EventManager.OnOnBeginOfDay", "MMH7Game.H7EventManager.OnOnEndOfDay",
  "MMH7Game.H7EventManager.OnOnDoDamage", "MMH7Game.H7CreatureStack.ApplyDamage", "MMH7Game.H7Unit.ApplyDamage",
  "MMH7Game.H7Player.ApplyDamage", "MMH7Game.H7HallOfHeroesManager.UpdateEndDay",
  "MMH7Game.H7AdventureController.SetNextPlayer", -- calls mCalendar.NextDay() when the last player ends a turn
}

-- h7.first may pick an archetype from a Data package instead of the live controller.
for _, c in ipairs(h7.find_all("H7AdventureController")) do T("controller instance: %s", tostring(c)) end
for _, c in ipairs(h7.find_all("H7Calendar")) do T("calendar instance: %s", tostring(c)) end
T("h7.first controller: %s", tostring(h7.first("H7AdventureController")))
for _, c in ipairs(h7.find_all("H7AdventureController", true)) do T("with templates: %s", tostring(c)) end

local counts, reported, ids = {}, {}, {}
local function trace(path)
  local ok, id = pcall(h7.hook, path, function() counts[path] = (counts[path] or 0) + 1 end)
  if ok then ids[#ids + 1] = id else T("cannot hook %s: %s", path, tostring(id)) end
end

for _, cls in ipairs(classes) do
  local owner = cls:match("[^.]+$")
  for _, f in ipairs(h7.functions(cls)) do
    local fl = f.flags
    if f.owner == owner and fl & DEFINED ~= 0 and fl & (NATIVE | ITERATOR | LATENT | DELEGATE) == 0 then
      trace(cls .. "." .. f.name)
    end
  end
end
for _, p in ipairs(extra) do trace(p) end
T("tracing %d functions", #ids)

-- Details for the two silent mods: does the hook run, and what does it see?
local n1, n2 = 0, 0
ids[#ids + 1] = h7.hook_post("MMH7Game.H7Calendar.NextDay", function(calendar)
  n1 = n1 + 1
  if n1 > 3 then return end
  T("NextDay post on %s", tostring(calendar))
  local adv = h7.first("H7AdventureController")
  local ok, pl = pcall(function() return adv and adv:GetLocalPlayer(true) end)
  T("NextDay post: controller=%s localPlayer=%s (%s)", tostring(adv), tostring(pl), tostring(ok))
end)
ids[#ids + 1] = h7.hook_post("MMH7Game.H7GameProcessor.CalculateDamage", function(_, args)
  n2 = n2 + 1
  if n2 > 5 then return end
  T("CalculateDamage post: Result=%s idx=%s forecast=%s", tostring(args.Result), tostring(args.resultIdx),
    tostring(args.forecast))
end)

local function report()
  local lines = {}
  for path, c in pairs(counts) do
    if c ~= (reported[path] or 0) then
      lines[#lines + 1] = ("%s +%d"):format(path:gsub("^MMH7Game%.", ""), c - (reported[path] or 0))
      reported[path] = c
    end
  end
  table.sort(lines)
  for _, s in ipairs(lines) do T("%s", s) end
  h7.after(3, report)
end
h7.after(3, report)
return "tracing " .. #ids
