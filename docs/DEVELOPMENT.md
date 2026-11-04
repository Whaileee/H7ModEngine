# H7ModEngine development guide

This guide is for people working on the engine itself: the C++ DLL, the Lua runtime and bindings, and the
bundled mods. If you want to write mods, read [API.md](API.md) instead.

- [Setup](#setup)
- [Building and deploying](#building-and-deploying)
- [The development loop](#the-development-loop)
- [Architecture](#architecture)
- [Source tour](#source-tour)
- [Hooks in detail](#hooks-in-detail)
- [The Lua side](#the-lua-side)
- [Testing in game](#testing-in-game)
- [Finding things in the game](#finding-things-in-the-game)
- [Reverse-engineering notes](#reverse-engineering-notes)
- [Working with the game's Flash UI](#working-with-the-games-flash-ui)
- [Extending the API](#extending-the-api)
- [Conventions](#conventions)
- [CI](#ci)
- [Troubleshooting](#troubleshooting)

## Setup

**Required**

- Windows 10/11 x64.
- Visual Studio 2022 or the Build Tools for Visual Studio, with the **C++ x64 build tools** workload component.
  `build.bat` finds any edition with `vswhere`.
- Git. The repository uses a submodule for Lua, so clone with `--recursive`, or run
  `git submodule update --init` after a normal clone.
- Might & Magic Heroes VII **v2.2.1** (Ubisoft Connect, 64-bit). The engine is only tested against this build.

**Useful**

- Git Bash: `tests/run_ingame.sh` is a bash script.
- Python 3: the scripts in `re/`.
- `cdb.exe` (Debugging Tools for Windows, part of the Windows SDK): static disassembly of the unpacked game image
  and attaching to the running game.
- The decompiled UnrealScript sources of the game (`<Package>\Classes\*.uc`, e.g. from the H7 SDK / UDK export).
  They are the fastest way to find the function you want to hook; see
  [Finding things in the game](#finding-things-in-the-game).

```bash
git clone --recursive https://github.com/Whaileee/H7ModEngine.git
```

## Building and deploying

```bash
build.bat
```

builds `bin\dinput8.dll` (and `bin\dinput8.pdb`). It compiles Lua 5.4.7 from `third_party\lua` **as C++**
(`/TP /EHsc`), so a Lua error raised inside engine code unwinds as a C++ exception through the C++ frames
instead of `longjmp`-ing over their destructors. The engine is C++20, `/MT`, linked with `src\proxy\dinput8.def`.

```bash
build.bat deploy
```

builds and then installs into the game folder:

| Source | Destination | Notes |
|---|---|---|
| `bin\dinput8.dll` | `<Game>\Binaries\Win64\dinput8.dll` | replaced every time |
| `dist\H7ModEngine.ini` | `<Game>\H7ModEngine\H7ModEngine.ini` | only if there is none yet: your local settings survive |
| `dist\mods\ModManager` | `<Game>\H7ModEngine\mods\ModManager` | overwritten: edits you made in the game copy are lost. `build.bat deploy examples` also installs the example mods |
| `dist\lib\*` | `<Game>\H7ModEngine\lib\` | shared Lua libraries, if any |

The game folder defaults to `N:\Ubisoft\games\Might and Magic Heroes VII`; set `H7_GAME_DIR` to yours:

```bash
set H7_GAME_DIR=C:\Games\Might and Magic Heroes VII
```

The DLL can't be replaced while the game runs (Windows keeps it loaded). Lua files can: see the next section.

## The development loop

1. `build.bat deploy` with the game closed.
2. Start the game through Ubisoft Connect. Starting `MMH7Game-Win64-Shipping.exe` directly makes it exit and
   relaunch through Connect, so a short-lived first process is normal; tools that wait for the game must not
   treat that first exit as the end of the run.
3. Follow the log: `<Game>\H7ModEngine\logs\engine.log` (the previous run is in `engine.prev.log`).

   ```bash
   tail -f "/n/Ubisoft/games/Might and Magic Heroes VII/H7ModEngine/logs/engine.log"
   ```

   A healthy start looks like this:

   ```
   H7ModEngine starting; game image 0000000140000000-0000000146B94000
   Script loaded: 27743 names, 12792 objects
   Calibrated: Struct{Super=78 Children=80 Size=88} Prop{...} Func{...} Class{CDO=1d4} Enum{Names=68}
   Script functions: 18472, bound to ProcessInternal(game+266de40): 18471, unbound: 1, other: 0
   Tick hook on Function Engine.GameViewportClient.PostRender: ok
   Init complete; Lua starts on the next frame
   Lua 5.4 ready (H7ModEngine 0.1.0)
   Loaded mod HelloWorld 1.0 by H7ModEngine
   Map: h7logoentry
   Map: councilhub
   ```

4. Change Lua without restarting:
   - **Mods hot-reload.** Edit the game copy in `<Game>\H7ModEngine\mods\<Mod>\`; within a second the engine
     unloads the mod (removing its hooks, handlers and timers) and loads it again. Copy the change back to
     `dist\mods\` when it works, or edit in `dist\` and copy over; `deploy` overwrites the game copies.
   - **The exec folder** runs a script once in the running game: drop `something.lua` into
     `<Game>\H7ModEngine\exec\`. It runs on the next check (about once a second), its return values are logged
     as `exec: => ...`, and the file is renamed to `.done` (even when it fails, and overwriting an older
     `.done` of the same name, so keep the source in `tests\` and copy it in). Exec scripts use the global
     environment, not a mod's; `_G` is shared with mods, which is handy for poking at a mod's state.
5. Only C++ changes need a restart.

The game pauses its main loop while its window is in the background: ticks, timers, hot reload and the exec
folder stop until it is focused again. While it runs but is covered by other windows it keeps ticking but stops
drawing.

## Architecture

```
MMH7Game-Win64-Shipping.exe
  └─ loads dinput8.dll (ours, from Binaries\Win64)
       ├─ proxy: forwards DirectInput exports to the system dinput8.dll
       └─ InitThread
            1. wait for GNames / GObjects (the exe is packed and unpacks itself first)
            2. wait for script packages (Engine.Actor.Touch, MMH7Game.H7Creature exist)
            3. Calibrate(): learn UObject/UStruct/UProperty/UFunction/UClass layouts from known objects
            4. wait for the GameViewportClient, find UObject::ProcessInternal
            5. hook Engine.GameViewportClient.PostRender  ──► OnGameTick() every frame, on the game thread
                                                                ├─ first tick: create the Lua state, load mods
                                                                ├─ timers, tick event, mod toggles
                                                                └─ once a second: hot reload, map change, exec
```

Everything after step 5 runs on the game thread, inside the engine's own frame. Lua is single-threaded and never
touched from the init thread.

**No code patching.** The executable is protected and packed, so the engine never writes to its code. It only
changes heap data the engine itself reads: `UFunction::Func` pointers, `UFunction::Script` bytecode arrays, and a
few engine objects it creates (a cloned native `UFunction`). This is why hooks are built from the engine's own
mechanisms (see [Hooks in detail](#hooks-in-detail)).

**Discovery instead of offsets.** Global tables are found by scanning the module for structures that look like
`TArray<FNameEntry*>` and `TArray<UObject*>` and checking them against known names. Field offsets are then
measured on objects whose values are known (for example, the size and parameter count of `Engine.Actor.Touch`).
The results are logged in the `Calibrated:` line. When calibration fails, the engine writes `dumps\probe.txt`
and stays out of the way.

### Confirmed layout (v2.2.1, x64)

Engine structs are **packed to 4 bytes**: pointers are not always 8-aligned (for example `UStruct::Script` is at
0x8C). Scan with a step of 4.

| Type | Field | Offset |
|---|---|---|
| UObject | VfTable / ObjectFlags / InternalIndex / Outer / Name / Class / Archetype | 0x00 / 0x10 / 0x38 / 0x40 / 0x48 / 0x50 / 0x58 (size 0x60) |
| UField | Next | 0x60 |
| UStruct | SuperField / Children / PropertySize / Script | 0x78 / 0x80 / 0x88 / 0x8C (size 0xD0) |
| UFunction | FunctionFlags / iNative / FriendlyName / NumParms / ParmsSize / ReturnValueOffset / Func | 0xD0 / 0xD4 / 0xD8 / 0xE1 / 0xE2 / 0xE4 / 0xF0 (size 0xF8) |
| UProperty | ArrayDim / ElementSize / PropertyFlags / Offset | 0x68 / 0x6C / 0x70 / 0x8C (size 0xA8) |
| UBoolProperty | BitMask | 0xA8 |
| UEnum | Names (`TArray<FName>`) | 0x68 |
| UClass | ClassDefaultObject | 0x1D4 (calibrated) |
| FFrame | Node / Object / Code / Locals / OutParms | +0x14 / +0x1C / +0x24 / +0x2C / +0x3C |

Other facts: `ObjectFlags` 0x200 = class default object, 0x400 = archetype. `ProcessEvent` is `vtable[70]`,
found at runtime by watching who calls a hooked event. The engine allocator is tbbmalloc: strings and arrays the
engine may free later are allocated with `scalable_realloc` (`ue3::EngineRealloc`).

## Source tour

| Path | What it does |
|---|---|
| `src/proxy/` | `dinput8.def` and `proxy.cpp`: export forwarding to the system DLL |
| `src/engine/dllmain.cpp` | `DllMain` and the init thread (the sequence above); finds the viewport and installs the tick hook |
| `src/engine/paths.*` | `<Game>\H7ModEngine\` paths and `H7ModEngine.ini` access |
| `src/core/log.*` | The engine log (shared-write, since two game processes overlap briefly at launch) |
| `src/core/memory.*` | SEH-guarded reads/writes (`mem::Read`, `mem::ReadOr`), module bounds, region scans, `.pdata` function starts |
| `src/ue3/core.*` | GNames/GObjects discovery, names, object iteration, `FindObject`/`FindClass` |
| `src/ue3/layout.h` | The calibrated offsets (`ue3::L`) |
| `src/ue3/reflection.*` | Calibration; fields, properties, functions, enums, `Describe(prop)`, `EngineRealloc` |
| `src/ue3/hooks.*` | `ProcessEvent` discovery and calls; script and native function hooks |
| `src/ue3/factory.*` | Creating objects: `StaticConstructObject`, `FName(TCHAR*, FNAME_Add)` and `GetTransientPackage` at fixed v2.2.1 addresses, each checked against its first bytes before use |
| `src/ue3/dumper.*` | `names.txt`, `objects.txt`, `probe.txt` and the unpacked image dump (`[Dev]` settings) |
| `src/lua/runtime.*` | Lua state, mod discovery/order/environments, hot reload, mod toggles, events, timers, saved data, exec folder |
| `src/lua/bindings.cpp` | The `h7` table and the object/struct/array proxies: marshaling between Lua and engine memory, calls, hooks |

`[Dev]` settings in `H7ModEngine.ini`: `DumpSDK=1` writes the object index modders use; `DumpProbe=1` writes
raw layout snapshots (calibration work); `DumpImage=1` writes the unpacked executable (~100 MB, for static
analysis).

## Hooks in detail

UnrealScript functions are called two ways, and a hook has to catch both:

- **C++ → script**: `ProcessEvent(obj, fn, params)` → `fn->Func` (for script functions, `UObject::ProcessInternal`).
- **Script → script**: the VM calls `ProcessInternal` directly; `UFunction::Func` is never consulted.

So swapping `Func` only works for natives and C++-invoked events. Script functions get a **bytecode trampoline**
instead (`ue3/hooks.cpp`):

- The function's `Script` array is replaced by
  `[original optional-parameter entries] 04 1C <fake native UFunction*> 16 04 0B 53`.
  `ProcessInternal` steps statements until `EX_Return` (0x04), then evaluates one expression as the return value;
  that expression is an `EX_FinalFunction` (0x1C) call to a **fake native**: a clone of `Core.Object.Sin`
  (final, native, iNative 0) whose `Func` is `ScriptHookNative`.
- `ScriptHookNative` runs the pre listeners (any can block), then runs the original body with `ProcessInternal`
  on a copy of the frame whose `Node` is a shadow `UFunction` holding the original `Script` and whose `Code`
  points just past the optional-parameter entries, then runs the post listeners (which can replace the return
  value).
- **Optional parameters**: the engine steps one entry per optional parameter at the start of `Script` while
  binding the call, before `ProcessInternal` runs, so those entries must stay at the front. An entry is either
  `EX_DefaultParmValue` (`49 <u16 len> <expr> 15`) or a lone `EX_Nothing` (`0B`) for a parameter without a
  default. The parser (`DefaultParmBlocks`) accepts exactly these and otherwise falls back to a `Func` swap and
  logs it; it never guesses, because a wrong guess crashes the VM with `Unknown code token`.
- **Out parameters** of script → script calls live in the caller's frame. `FFrame::OutParms` (+0x3C) is a list
  of `{UProperty*, PropAddr, Next}`; hook `args` resolve out parameters through it (`hooks::OutParmAddr`).
- Native, latent and iterator functions use the `Func` swap. Hooks on natives see no arguments and can't block.

Several listeners can share one hooked function; they are removed with their mod. Opcode handlers are in
`GNatives` (game+0x2352220); disassemble with `cdb -z <unpacked exe> -c "u <address>; q"`.

## The Lua side

**Proxies.** Engine objects are full userdata (`ObjRef`) holding the `UObject*`; `__index` looks the name up as a
property or function of the object's class (cached per class). Structs (`StructRef`) and dynamic arrays
(`ArrayRef`) are live views into engine memory. Values that point into a hook's parameters carry a frame id and
become invalid when the hook returns. Function return values that are structs or arrays are copied to Lua
tables, because the parameter buffer is gone after the call.

**Marshaling** goes through `PushElem` (engine → Lua) and `WriteElem` (Lua → engine), keyed by property kind:
numbers, bools (bitmask), names (must already exist in the name table), strings (`FString` via the engine
allocator), objects (type-checked), structs (another struct or a table of fields), arrays (a table replaces the
contents), delegates (`{object=, func=}`).

**Calls** (`CallUFunction`): parameters are written into a zeroed buffer of `ParmsSize`, missing arguments stay
zero (not UnrealScript defaults), the call goes through `ProcessEvent`, then the return value and out
parameters are pushed, and strings and arrays left in the buffer are freed.

**Mods** (`runtime.cpp`): every folder in `mods\` is discovered; enabled ones load alphabetically, then after
their `load_after` entries. Each mod runs `main.lua` in its own environment (globals stay in the mod, reads fall
through to the shared globals). Everything a mod registers — hooks, event handlers, timers — is tagged with the
mod's id and removed when it unloads. Callbacks run inside `ModScope(mod)`, so anything they register belongs to
the same mod, and their log lines carry its name. `h7.set_mod_enabled` writes the ini and the load or unload
happens on the next tick, never inside the caller.

**Errors** inside callbacks are caught with a traceback (`Report`) and logged; the hook or handler stays
installed.

## Testing in game

There are no offline unit tests: almost everything depends on the live engine. Tests are Lua scripts in `tests\`
that run through the exec folder and report to the log.

| Script | Run it | Checks |
|---|---|---|
| `selftest.lua` | at the main menu | lookups, reads/writes, structs, arrays (incl. writing them and array arguments), calls, hooks |
| `hooktest.lua` | at the main menu | pre-hook blocking, post-hook return override, arguments, script → script calls, unhook |
| `sigdump.lua` | any map | logs signatures of the gameplay functions the example mods use |
| `daytrace.lua` | adventure map | call-count tracer for ~117 calendar/combat functions: end a day, fight a battle, read which ran |
| `modwindow_autotest.lua` | main menu | drives the Mods window: open, enable/disable DailyGold, back, then checks the game's map selection was restored |
| `swfdump.lua` | main menu | copies every loaded Flash movie to `dumps\swf\` |
| `menulab.lua`, `modlistlab.lua` | main menu | the experiments the Mods menu was built from |

Run one by copying it into `exec\` and reading the log, e.g. a successful self-test ends with

```
[selftest] array argument         OK    first,2.0
[selftest] call function          OK    <player name>
exec: => selftest dispatched
[selftest] hook fired 154 times in 2s
```

**`tests/run_ingame.sh`** automates this and takes screenshots:

```bash
tests/run_ingame.sh tests/modwindow_autotest.lua /c/temp/shots 60
```

It copies the script into `exec\`, prints the log lines it produces, saves `<dir>\<name>.png` for every line
ending in `SHOT <name>` (via `tests/shot.ps1`, which captures the game window with `PrintWindow`, on any display)
and stops at a line ending in `DONE`. A test drives the game with `h7.after` steps, logs the state it checks,
and writes `SHOT`/`DONE` lines; see `modwindow_autotest.lua`. Screenshots only show a current frame while the
game window is visible (it stops drawing when covered), so assert on logged state and use pictures to look.

Before a pull request that touches the engine, run at least `selftest.lua` and `hooktest.lua` at the main menu,
plus a test for what you changed, and note the results in the PR.

## Finding things in the game

1. **Search the object index.** With `DumpSDK=1`, `dumps\objects.txt` lists every class, function, property and
   object loaded at startup (`Function MMH7Game.H7Calendar.NextDay`, ...), and `names.txt` every name.
2. **Read the decompiled UnrealScript.** Grep the `.uc` sources for the behaviour you want and follow callers;
   it is much faster than tracing at runtime. Prefer hooking script functions (bodies in the sources) over
   natives (`native function`), since native hooks get no arguments.
3. **Ask the engine.** `h7.signature("MMH7Game.H7Calendar.NextDay")`, `h7.functions(obj)`,
   `h7.properties("MMH7Game.H7AdventureHero")`, `obj:get_class()`; run them through the exec folder.
4. **Trace.** Copy `daytrace.lua` and adapt its function list to see which candidates actually run, and how
   often, while you play.

Known good targets are listed in `RESUME.md` (new day, damage, combat XP, movement points).

## Reverse-engineering notes

- **The executable is packed** (stub entry in `.text1`, `.xtext` decrypted in place at startup), so the file on
  disk is useless for static analysis. Set `DumpImage=1`, start the game once, and use
  `<Game>\H7ModEngine\dumps\MMH7Game_unpacked.exe`. Keep dumps out of git (`re/dump/` is ignored).
- `cdb -z <unpacked exe> -c "u <address>; q"` disassembles; addresses in the log are given as `game+<rva>`.
- `re/scripts/pe_scan.py <exe>`: sections, anchor strings and UE3 native-function tables (`execFoo` pairs).
- `re/swfinfo.py <movie.swf> [--strings]`: tags, exported symbols and AS3 classes with their instance and static
  members, for the Scaleform movies dumped by `tests/swfdump.lua`.
- A Ghidra project may live in `re/ghidra_proj/` (ignored).
- `RESUME.md` is the running notebook: what was tried, what was verified in which in-game run, and why.

## Working with the game's Flash UI

The menus are Scaleform (GFx) movies driven by UnrealScript controllers (`H7MainMenuCntl`, `H7MapSelectCntl`,
...). Lua reaches them through the controllers' `GFxObject` fields and `GetObject`, `SetString`, `Invoke`,
`AttachMovie`. Lessons from building the Mods menu (`dist/mods/ModManager`):

- **Wrappers are garbage collected.** The `GFxObject` returned by `GetObject`/`AttachMovie` is a UObject that the
  engine frees when no UnrealScript variable holds it; the Flash clip stays. Look clips up by name each time.
- **`GetObject` reaches display objects and Unreal-created objects only.** Members holding arrays or objects that
  ActionScript created (list rows, dictionaries, static class members) come back `nil`.
- **Clicks**: a button added with `AttachMovie` can call a controller function through
  `AddEventListener("CLIK_click", {object = cntl, func = "Options"})`, but the event data arrives empty; hook the
  function and check `mouseX`/`mouseY` against your button.
- **Reusing a game window** is often better than building one: fill it through the same calls its controller
  makes (`SetObject("mPolledMaps", ...)`, `Invoke("AddMaps")`) and hook the controller functions its buttons call.
  Remember every value you change and put it back when the window closes, since the game reuses the window.
- **Translations are cached per movie for the whole session** (`Loca.mLocaDictionary`, filled through
  `H7FlashMovieCntl.FlashLocalize`). Overriding a key changes it everywhere; values in ALL CAPS are treated as
  keys and shown as `[VALUE]` when unknown.
- `H7Label.htmlText` resets the label's font size; prefer `text`.

## Extending the API

1. Implement the function in `src/lua/bindings.cpp` (object and engine access) or `src/lua/runtime.cpp` (mods,
   events, timers) and add it to the registration table at the end of the file (`{"name", H_name}`).
   Object helper methods (`obj:get_name()` style) go in the `H7ObjectMethods` table; they are snake_case so they
   never collide with UnrealScript members.
2. Read engine memory with `mem::Read`/`mem::ReadOr` (SEH-guarded), never raw pointers; check objects with
   `CheckValidObject`. Report misuse with `luaL_error` and a message that says what was expected.
3. Allocate anything the engine may free with `ue3::EngineRealloc`.
4. Document it in `docs/API.md` (the section and the reference table) and add a check to `tests/selftest.lua`.

## Conventions

- C++20, 4-space indent, lines up to about 120 columns. Comments explain *why* (what the engine does, what was verified),
  not what the next line does. Engine facts that were measured in game say so.
- Names: `PascalCase` functions and types, `g_` globals, `k` constants; Lua API names are lowercase
  (`h7.find_all`, `obj:get_name()`), UnrealScript names keep their own spelling.
- Lua (mods and tests): 4-space indent, `local` everything, one `h7.log` prefix per mod or test.
- Keep `docs/API.md` in step with the API, and `RESUME.md` with findings that cost time to learn.
- Commits: one topic each, imperative subject line, body explaining what and why. Work on a branch and open a
  pull request into `main`; CI must be green.
- Line endings: `.bat` files are CRLF and `.sh` files LF (`.gitattributes`); cmd and bash each break on the other.

## CI

`.github/workflows/build.yml` runs on pushes to `main` and on pull requests: it checks out with submodules on
`windows-latest`, runs `build.bat`, and uploads an `H7ModEngine` artifact laid out like the game folder:

```
Binaries/Win64/dinput8.dll
H7ModEngine/            (dist/ + API.md)
```

That artifact is what players install. The version string is `kVersion` in `src/lua/runtime.cpp`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| No `logs\engine.log` at all | `dinput8.dll` not in `Binaries\Win64\`, or another `dinput8.dll` there (e.g. a dangling link) |
| Log stops at `Timed out waiting for ...` | A different game version, or the game didn't finish loading |
| `Engine layout calibration failed` | Unknown build: send `dumps\probe.txt` |
| `Init complete` but no `Lua ... ready` | The tick hook doesn't fire (window minimized or in the background) |
| Nothing happens for a while, then it all runs | The game was in the background and paused its loop |
| `Unknown code token` crash in a hooked function | The trampoline misread the function's optional-parameter entries: report the function and its signature |
| `engine object ... is no longer valid` | A cached Flash `GFxObject` was garbage collected: look it up again |
| Mod changes don't take effect | You edited `dist\` but not the game copy (or ran `deploy`, which overwrote the game copy) |
