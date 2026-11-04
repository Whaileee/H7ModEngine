-- DamageScale: scales all combat damage dealt by creatures, heroes and spells.
-- Shows: reading a hooked function's arguments and calling methods on them.

-- H7GameProcessor.CalculateDamage(out H7CombatResult Result, int resultIdx, bool forecast) fills in the damage of
-- one attack target (heals go through it too). Forecasts (tooltips, AI planning) are scaled as well, so they
-- keep matching real hits.
h7.hook_post("MMH7Game.H7GameProcessor.CalculateDamage", function(processor, args)
    local result, idx = args.Result, args.resultIdx
    if not result or result:IsHeal(idx) then return end
    local damage = result:GetDamage(idx)
    result:SetDamage(math.floor(damage * mod.settings.multiplier + 0.5), idx)
    if not args.forecast and mod.settings.log then
        h7.log(("Damage %d -> %d"):format(damage, result:GetDamage(idx)))
    end
end)
