return {
    name = "DebugConsole",
    version = "1.0",
    author = "H7ModEngine",
    description = "An in-game Lua console for mod makers: run Lua or game console commands and watch the engine log. "
        .. "Open it with the key set in H7ModEngine.ini, [Console] Key= (default Tilde, the key left of 1).",
    category = "Developer",
    enabled = false, -- turn it on in the Mods menu
    settings = {
        { key = "height", label = "Height", default = 0.45, min = 0.2, max = 0.9, step = 0.05,
          description = "Part of the screen the console covers." },
        { key = "scale", label = "Text size", default = 1.5, min = 0.75, max = 3, step = 0.25,
          description = "Size of the console text." },
    },
}
