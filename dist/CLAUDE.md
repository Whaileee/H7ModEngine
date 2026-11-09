# dist/ — what gets installed into `<Game>\H7ModEngine\`

| Path | Notes |
|---|---|
| `H7ModEngine.ini` | Default config. `deploy` copies it **only if the game has none**, so changes here don't reach an existing install |
| `mods/DebugConsole/` | In-game Lua console + live log (`enabled = false`, category Developer). Bundled and deployed like ModManager. Key: ini `[Console] Key=` (read with `h7.config` every second; the engine adds `Key=Tilde` to existing inis). Logic can be tested offline with a stubbed `h7` |
| `mods/ModManager/` | The in-game Mods menu (`hidden = true`). Always deployed, and embedded in `dinput8.dll` with the ini (`build.bat` -> `src/engine/bundle.cpp`): a DLL whose bundled ModManager differs from the one it last installed overwrites the game copy at startup. See its own `CLAUDE.md` |
| `mods/HelloWorld`, `DailyGold`, `CombatXP`, `DamageScale`, `HeroMovement` | Example mods referenced by the docs and tests. Ship `enabled = false` (except HelloWorld); installed only by `build.bat deploy examples` |
| `mods/OnlineLobby/` | Online multiplayer through H7Lobby (`enabled = true`, category Multiplayer). `main.lua`: hosting, compatibility, joining; `browser.lua`: the Online > Join browser on the map selection window (same window and rules as ModManager); `reports.lua`: opt-in desync reports (setting `report_desyncs`, off; `docs/ONLINE.md` "Desync reports"). Bundled and deployed like ModManager. Design: `docs/ONLINE.md`; lobby API: H7Lobby README |
| `mods/MPSyncDebug/` | Desync tracing (`enabled = false`, category Developer, so the lobby's compatibility check ignores it). `main.lua` writes the tracer's lines (`lib/mpsync.lua`) to `logs\mpsync-<host|client>-<time>.log` during MP games. Bundled and deployed like ModManager. Read the logs with `tools/mpsync_diff.py`; design: `docs/DESYNC.md` Phase A |
| `lib/` | Shared Lua libraries, bundled in the DLL like the mods. `mpsync.lua`: the multiplayer lockstep tracer (`mpsync.new{...}` per mod, so its hooks belong to the caller; state snapshot + hash), used by MPSyncDebug and OnlineLobby's reports. Its line format is the contract with `tools/mpsync_diff.py` and H7Lobby's report storage |

## The two copies problem

`dist/` is the source of truth, but the game runs `<Game>\H7ModEngine\mods\`. Mods hot-reload from the **game
copy** within about a second. Either edit the game copy and copy back to `dist/` when it works, or edit `dist/`
and copy over. `build.bat deploy` overwrites the game copies of the mods it installs — don't lose live edits.
The game install currently holds only ModManager and H2Enchantress; install the examples before tests that need
them (`settingstest.lua`, `modwindow_autotest.lua`).

## Mod conventions

- `mod.lua` returns the manifest: `name, version, author, description, category, enabled`, optional
  `load_after`, `hidden`, `settings = { {key, label, description, type, default, min, max, step, options,
  control, reload}, ... }`. Full spec: `docs/reference/mod.md`.
- `main.lua` runs in the mod's own environment; use `mod.require`, `mod.settings`, `mod.save/load`, optional
  `on_unload()`. 4-space indent, `local` everything.
- Example mods are documentation: keep them short, commented, and in sync with `docs/API.md`.
- Prefer hooking script functions over natives (natives get no args, can't block).
