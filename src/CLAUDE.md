# src/ — engine C++

C++20, MSVC, `/MT`, x64, one DLL. `build.bat` compiles every `*.cpp` under `src/` recursively (no project file to
update when adding files). See `docs/DEVELOPMENT.md` "Source tour" for what each file does.

| Dir | Role |
|---|---|
| `proxy/` | `dinput8.def` + export forwarding to the system DLL. Rarely touched |
| `core/` | Log (shared-write: two game processes overlap at launch), SEH-guarded memory access, scans, `.pdata` |
| `engine/` | `DllMain`, init thread (wait for unpack → discover → calibrate → find viewport → tick hook), paths + ini |
| `ue3/` | Discovery, reflection/layout, hooks, object factory, dumpers — see `ue3/CLAUDE.md` |
| `lua/` | Lua runtime (mods, events, timers, exec) and the `h7` bindings — see `lua/CLAUDE.md` |

## Rules

- **Never dereference engine memory raw.** Use `mem::Read` / `mem::ReadOr` (SEH-guarded); validate objects before use.
- **Engine structs are packed to 4 bytes.** Any offset scan or calibration steps by 4, not 8.
- Offsets live in `ue3/layout.h` (`ue3::L`) and are calibrated at runtime; don't hard-code new ones elsewhere.
  Fixed RVAs (v2.2.1 only) must be checked against their first bytes before use and disable the feature if they
  don't match (pattern: `ue3/factory.cpp`).
- Memory the engine may free (FString, TArray data, UFunction::Script) is allocated with `ue3::EngineRealloc`
  (tbbmalloc `scalable_realloc`), never `malloc`/`new`.
- Everything that touches Lua or engine objects runs on the game thread (inside the `PostRender` tick or a hook).
  The init thread only discovers and calibrates.
- Style: 4-space indent, ~120 columns, `PascalCase` functions/types, `g_` globals, `k` constants. Comments say
  *why* and mark facts measured in game.
- After a C++ change: `build.bat` must build cleanly; then it needs an in-game run (game closed for `deploy`,
  user starts it). At minimum `tests/selftest.lua` and `tests/hooktest.lua` at the main menu.
- Version string: `kVersion` in `lua/runtime.cpp`.
