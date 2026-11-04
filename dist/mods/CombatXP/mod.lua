return {
    name = "CombatXP",
    version = "1.0",
    author = "H7ModEngine",
    description = "Example: heroes earn more experience from combat (overriding a return value).",
    category = "Combat",
    enabled = false, -- example mod: turn it on in the Mods menu
    settings = {
        { key = "multiplier", label = "XP multiplier", default = 1.5, min = 1, max = 5, step = 0.25,
          description = "Experience from combat is multiplied by this." },
    },
}
