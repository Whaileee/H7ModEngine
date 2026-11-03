-- DailyGold: the local (human) player receives bonus gold at the start of every new day.
-- Shows: a post-hook on a game function, finding a game object, calling game functions.

local GOLD_PER_DAY = 500 -- edit while the game runs; the mod reloads automatically

local function local_player()
    local adventure = h7.first("H7AdventureController")
    return adventure and adventure:GetLocalPlayer(true)
end

-- H7Calendar.NextDay() runs once each time the calendar advances to a new day.
h7.hook_post("MMH7Game.H7Calendar.NextDay", function(calendar)
    local player = local_player()
    if not player then return end
    -- H7ResourceSet.ModifyCurrency(int modAmount, bool updateGui)
    player:GetResourceSet():ModifyCurrency(GOLD_PER_DAY, true)
    h7.log(("+%d gold for %s"):format(GOLD_PER_DAY, player:GetName()))
end)
