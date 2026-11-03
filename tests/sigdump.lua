-- Dev: logs the signatures of gameplay functions considered for example mods.
-- Copy into <Game>\H7ModEngine\exec\ (any map); results go to logs\engine.log.
local fns = {
    "MMH7Game.H7EditorHero.AddXp", "MMH7Game.H7EditorHero.SetXp", "MMH7Game.H7EditorHero.GetExperiencePoints",
    "MMH7Game.H7EditorHero.GetMovementPoints", "MMH7Game.H7EditorHero.GetCurrentMovementPoints",
    "MMH7Game.H7EditorHero.SetCurrentMovementPoints", "MMH7Game.H7EditorHero.UseMovementPoints",
    "MMH7Game.H7EditorHero.AddMana", "MMH7Game.H7EditorHero.GetLevel",
    "MMH7Game.H7AdventurePlayerController.AddXp", "MMH7Game.H7AdventurePlayerController.AddResourcesToLocalPlayer",
    "MMH7Game.H7AdventureController.GetInstance", "MMH7Game.H7AdventureController.GetLocalPlayer",
    "MMH7Game.H7AdventureController.GetLocalPlayerHeroes", "MMH7Game.H7AdventureController.BeginTurn",
    "MMH7Game.H7AdventureController.StartPlayerTurn", "MMH7Game.H7AdventureController.UpdateHerosNewTurn",
    "MMH7Game.H7Player.GetResourceSet", "MMH7Game.H7Player.IsControlledByLocalPlayer", "MMH7Game.H7Player.GetHeroes",
    "MMH7Game.H7ResourceSet.ModifyCurrency", "MMH7Game.H7ResourceSet.ModifyResource",
    "MMH7Game.H7ResourceSet.GetCurrency", "MMH7Game.H7ResourceSet.GetResource",
    "MMH7Game.H7ResourceSet.SetResource", "MMH7Game.H7ResourceSet.GetResourceByIDString",
    "MMH7Game.H7Calendar.DayStart", "MMH7Game.H7Calendar.NextDay", "MMH7Game.H7Calendar.WeekStart",
    "MMH7Game.H7GameProcessor.CalculateDamage", "MMH7Game.H7GameProcessor.ApplyDamage",
    "MMH7Game.H7CombatResult.SetDamage", "MMH7Game.H7CombatResult.GetDamage",
    "MMH7Game.H7CombatResult.CalcFinalDamageModifier", "MMH7Game.H7CreatureStack.ApplyDamage",
    "MMH7Game.H7CombatController.GetInstance", "MMH7Game.H7CombatController.GetLocalPlayer",
}
for _, f in ipairs(fns) do
    local ok, s = pcall(h7.signature, f)
    h7.log("[sig] " .. tostring(s))
end
return "sigdump done"
