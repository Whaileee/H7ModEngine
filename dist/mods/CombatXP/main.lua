-- CombatXP: heroes earn more experience from combat (all heroes, AI included).
-- Shows: a post-hook that replaces a function's return value.

-- int H7CombatArmy.GetKilledCreaturesXP() computes the experience a fight is worth.
-- A post-hook receives (self, args, returnValue); returning a value replaces the result.
h7.hook_post("MMH7Game.H7CombatArmy.GetKilledCreaturesXP", function(army, args, xp)
    if xp <= 0 then return end
    local boosted = math.floor(xp * mod.settings.multiplier + 0.5)
    h7.log(("Combat XP %d -> %d"):format(xp, boosted))
    return boosted
end)
