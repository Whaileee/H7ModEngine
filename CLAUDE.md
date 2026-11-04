# H7ModEngine

Lua 5.4 modding engine for Might & Magic Heroes VII (UE3, **v2.2.1 only**, 64-bit). It ships as a `dinput8.dll`
proxy that discovers the engine's internals at runtime and exposes them to Lua mods. No game code is patched on
disk; hooks only touch heap data (UFunction bytecode and `Func` pointers).

## Read first

| File | When |
|---|---|
| `RESUME.md` | Before anything else when resuming work: current branch state, what was verified in which in-game run, open next steps, hard-won gotchas |
| `docs/DEVELOPMENT.md` | Architecture, source tour, hooks in detail, testing, conventions, CI |
| `docs/API.md` + `docs/reference/` | The modder-facing Lua API (the contract the engine must keep) |
| `README.md` | Player-facing overview and repository layout |

Each subdirectory with its own rules has a `CLAUDE.md`: `src/`, `src/ue3/`, `src/lua/`, `dist/`,
`dist/mods/ModManager/`, `tests/`, `docs/`, `tools/docs/`, `re/`.

## Map

| Path | What |
|---|---|
| `src/` | Engine (C++20, MSVC): proxy, UE3 discovery/reflection/hooks/factory, Lua runtime and bindings |
| `dist/` | Installed into `<Game>\H7ModEngine\`: default ini, ModManager, example mods |
| `tests/` | In-game Lua tests (run via the exec folder) and PowerShell/bash drivers |
| `docs/`, `mkdocs.yml`, `tools/docs/` | Docs site (GitHub Pages, built from release tags) and its generators |
| `re/` | Reverse-engineering helpers; dumps and the Ghidra project are git-ignored |
| `third_party/lua` | Lua 5.4.7 submodule, pinned at tag v5.4.7. **Never edit**; it is compiled as C++ |
| `bin/`, `build/`, `site/` | Build output, ignored |

## Commands

```bash
build.bat                    # -> bin\dinput8.dll (+ .pdb); needs VS 2022 C++ x64 tools
build.bat deploy             # build + install DLL, ini (only if missing) and ModManager into the game
build.bat deploy examples    # same, plus all example mods
```

`<Game>` defaults to `N:\Ubisoft\games\Might and Magic Heroes VII` (override with `H7_GAME_DIR`). Runtime root is
`<Game>\H7ModEngine\` (`logs\engine.log`, `mods\`, `exec\`, `dumps\`, `data\`).

## Working rules

- **The user launches the game** through Ubisoft Connect; unattended launch does not work. Ask them to start it
  (or to close it before a C++ deploy: the DLL is locked while the game runs).
- **No offline tests exist.** C++ changes are only "done" once verified in game; say "builds, not yet verified in
  game" otherwise. Lua changes can be tested live (mods hot-reload, `exec\` runs scripts once).
- The game pauses ticks (and hot reload, timers, exec) while its window is unfocused, and stops drawing while
  covered. Trust logged state over screenshots.
- Find hook targets by grepping the decompiled UnrealScript at `N:\UDK\Custom\Development\Src\<Package>\Classes\*.uc`
  before tracing at runtime. Game workspace notes: `N:\Ubisoft\games\Might and Magic Heroes VII\H7_REFERENCE.md`.
- This game install is heavily modded (14 factions etc.); don't assume vanilla data.
- Git: work on a feature branch, PR into `main` (CI must be green). Commit, push and open PRs only when the user
  asks. One topic per commit, imperative subject. `gh` may not be installed.
- `.bat` files are CRLF, `.sh` files LF (`.gitattributes`); keep it that way when writing them.
- Keep in step with every change: `docs/API.md` + `docs/reference/` (API), `tests/selftest.lua` (new API),
  `RESUME.md` (findings that cost time to learn, and in-game verification results with the date).
- The user sometimes writes in Polish and plays the game in Polish (UI texts in screenshots are Polish).

## Related repositories

- `N:\Projects\H2Enchantress`: the Heroes II Enchantress faction mod built on this engine (separate repo; deploy
  by copying its `H2Enchantress` folder to `<Game>\H7ModEngine\mods\`).
