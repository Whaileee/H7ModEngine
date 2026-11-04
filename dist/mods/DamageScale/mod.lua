return {
    name = "DamageScale",
    version = "1.0",
    author = "H7ModEngine",
    description = "Example: scales combat damage (reading hook arguments and calling methods on them).",
    category = "Combat",
    enabled = false, -- example mod: turn it on in the Mods menu
    settings = {
        { key = "multiplier", label = "Damage multiplier", default = 1.25, min = 0.25, max = 4, step = 0.25,
          description = "All combat damage (creatures, heroes, spells) is multiplied by this." },
        { key = "log", label = "Log hits", default = true,
          description = "Write every scaled hit to logs\\engine.log." },
    },
}
