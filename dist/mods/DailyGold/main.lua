-- DailyGold: the local (human) player receives bonus gold at the start of every new day.
-- Shows: a post-hook on a game function, finding a game object, calling game functions, a mod setting.

-- The amount is a setting (see mod.lua): players change it in the Mods menu, mod.settings has the current value.

local function local_player()
    local adventure = h7.first("H7AdventureController")
    return adventure and adventure:GetLocalPlayer(true)
end

-- H7Calendar.NextDay() runs once each time the calendar advances to a new day.
h7.hook_post("MMH7Game.H7Calendar.NextDay", function(calendar)
    local player = local_player()
    if not player then return end
    -- H7ResourceSet.ModifyCurrency(int modAmount, bool updateGui)
    local gold = mod.settings.gold
    if gold <= 0 then return end
    player:GetResourceSet():ModifyCurrency(gold, true)
    h7.log(("+%d gold for %s"):format(gold, player:GetName()))
end)
