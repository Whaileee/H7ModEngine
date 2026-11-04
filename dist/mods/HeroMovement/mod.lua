return {
    name = "HeroMovement",
    version = "1.0",
    author = "H7ModEngine",
    description = "Example: the human player's heroes get more movement points per day.",
    category = "Adventure",
    enabled = false, -- example mod: turn it on in the Mods menu
    settings = {
        { key = "multiplier", label = "Movement multiplier", default = 1.5, min = 1, max = 3, step = 0.1,
          description = "Movement points per day of the human player's heroes are multiplied by this." },
        { key = "who", label = "Applies to", options = { "Human player", "Everyone" }, default = "Human player",
          description = "Whose heroes move further." },
    },
}
