-- HeroMovement: the human player's heroes get more movement points per day.
-- Shows: filtering a hook by owner, and caching per object to keep a hot hook cheap.

-- Remember which heroes belong to the local player (GetMovementPoints runs often, e.g. for path previews).
local is_local = {}
h7.on("map_loaded", function() is_local = {} end)

local function owned_by_local_player(hero)
    local key = hero:get_address()
    if is_local[key] == nil then
        local player = hero:GetPlayer()
        is_local[key] = player ~= nil and player:IsControlledByLocalPlayer()
    end
    return is_local[key]
end

-- int H7EditorHero.GetMovementPoints(): the hero's movement points per day.
h7.hook_post("MMH7Game.H7EditorHero.GetMovementPoints", function(hero, args, points)
    if mod.settings.who == "Everyone" or owned_by_local_player(hero) then
        return math.floor(points * mod.settings.multiplier + 0.5)
    end
end)
