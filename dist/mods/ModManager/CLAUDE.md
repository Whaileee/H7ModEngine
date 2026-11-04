# ModManager — the in-game Mods menu

Reuses the game's **map selection window** (`H7MapSelectCntl` / `H7GfxMapList`, Scaleform) instead of building
a new one; the user explicitly wants it to look like that screen. `main.lua`: main menu button, mod list,
enable/disable, settings view. `panel.lua`: description + settings controls (sliders, check boxes, arrows,
number box) in the right-hand panel. The detailed how-it-works is in `RESUME.md` ("Mods menu", "Mod settings",
"Settings panel") and `docs/DEVELOPMENT.md` "Working with the game's Flash UI".

## Rules that came from breakage

- **Never cache GFxObject wrappers across frames** — the engine garbage-collects them; look clips up by name
  each time (`child(parent, "a.b")`). Objects held by script vars (`cntl.mMainMenu`, `sel.mMapSelection`) are safe.
- **Restore everything you change** in the shared window: `change()` records each original once per session in
  `ui.originals`, `restore()` puts each back in its own `pcall`. A failed restore once left "Mods" texts in the
  game's real map selection.
- **Never override shared localisation keys** (VICTORY_CONDITIONS, CREATOR, DESCRIPTION, ...): the movie caches
  translations for the whole session. ALL-CAPS row values are treated as keys and shown as `[VALUE]`.
- State that must survive a hot reload lives in `_G.__modmanager`. Reloading while the window is open still loses
  panel state.
- Flash event data from attached clips arrives empty: identify clicks by hooking the controller function and
  hit-testing `mouseX`/`mouseY`; panel input comes from hooking `Engine.Console.InputKey` (handled on the next
  tick, key names lower-case). Don't reuse a clip name for a different clip type.
- Rows can't be selected or styled from Lua (AS-created, `GetObject` returns nil): the selection resets to row 1
  after each change; no status colours.

## Testing

Needs real input and a visible game window: `tests/run_ingame.sh tests/modwindow_autotest.lua <shotdir>`,
`tests/click.ps1 <x> <y>`, `tests/keys.ps1 <keys>`. Coordinates for the 2048x1080 window are in `tests/CLAUDE.md`.
