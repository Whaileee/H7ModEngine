return {
    name = "DailyGold",
    version = "1.0",
    author = "H7ModEngine",
    description = "Example: the human player receives bonus gold every new day (hooks + calling game functions).",
    category = "Economy",
    enabled = false, -- example mod: turn it on in the Mods menu
    -- Shown and editable in the Mods menu; read with mod.settings.gold
    settings = {
        { key = "gold", label = "Gold per day", default = 500, min = 0, max = 5000, step = 100,
          description = "Gold the human player receives at the start of every day." },
    },
}
