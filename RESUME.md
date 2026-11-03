# H7ModEngine — resume notes (saved 2026-11-03 ~19:45)

Goal: a Lua 5.4 modding engine for Might & Magic Heroes VII (UE3, build v2.2.1) that exposes an easy API
to the community. Loaded as a `dinput8.dll` proxy; no game code is patched.

Game workspace reference: `N:\Ubisoft\games\Might and Magic Heroes VII\H7_REFERENCE.md`.

**Decompiled UnrealScript for the whole game** (H7 SDK): `N:\UDK\Custom\Development\Src\<Package>\Classes\*.uc`
(MMH7Game: 1432 classes). Grep it to choose hook targets and find callers before tracing at runtime.

## Where things are

| Path | What |
|---|---|
| `N:\Projects\H7ModEngine\src\` | Engine source (C++20, MSVC) |
| `build.bat` / `build.bat deploy` | Build `bin\dinput8.dll`; `deploy` copies it to `<Game>\Binaries\Win64\` plus `dist\` content to `<Game>\H7ModEngine\` |
| `dist\H7ModEngine.ini`, `dist\mods\` | Default config; example mods HelloWorld (enabled) + DailyGold, CombatXP, DamageScale, HeroMovement (shipped `enabled = false`; `deploy` overwrites the game copies, so re-enable them there for testing) |
| `docs\API.md` | Modder reference (written 2026-11-03 from the source + verified behaviour) |
| `tests\*.lua` | In-game scripts: copy into `<Game>\H7ModEngine\exec\` while the game runs. `selftest` (API), `hooktest` (block/override/args/script→script), `sigdump` (signatures), `daytrace` (call-count tracer for ~117 combat/calendar functions) |
| `third_party\lua` | Lua 5.4.7: git submodule of github.com/lua/lua pinned at tag v5.4.7 (`git submodule update --init` after cloning); compiled as C++ so errors unwind as exceptions |
| `re\dump\` | Unpacked game image dump + early names/objects dumps (git-ignored, like `bin\`, `build\` and `re\ghidra_proj\`) |
| `re\ghidra_proj\` | Ghidra project (headless analysis of the dump was started in background; may be complete) |
| `<Game>\H7ModEngine\` | Runtime root: `logs\engine.log`, `mods\`, `exec\`, `data\`, `lib\`, `dumps\` |

## Source layout

- `proxy/` — dinput8 export forwarding (`dinput8.def`, `proxy.cpp`)
- `core/` — logging, SEH-safe memory reads, region scan, `.pdata` function-start lookup
- `ue3/core.*` — runtime discovery of GNames/GObjects, names, objects, FindObject/FindClass
- `ue3/reflection.*` — layout calibration + UStruct/UProperty/UFunction/UEnum helpers
- `ue3/hooks.*` — function hooks: bytecode trampoline for script functions, `UFunction::Func` swap for natives;
  ProcessEvent discovery/calls
- `ue3/dumper.*` — names/objects/probe/image dumps (dev)
- `lua/bindings.cpp` — `h7.*` API: object/struct/array proxies, marshaling, calls, hooks
- `lua/runtime.cpp` — Lua state, mod loading/order/hot-reload, events, timers, persistent data, exec folder
- `engine/dllmain.cpp` — init thread: wait for unpack → discover → calibrate → find viewport → tick hook

## Key facts established

- Exe is **packed** (stub entry in `.text1`, `.xtext` decrypted in place at runtime). Static analysis of the
  on-disk exe is useless; use the runtime dump (`Dev/DumpImage=1`).
- Ubisoft Connect: starting the exe directly makes it exit and relaunch via Connect; `uplay://launch/1176/0`
  did **not** start it unattended — the user has to click Play. Two processes briefly overlap (log opened shared).
- Discovery works: GNames (FNameEntry: index field `(i<<1)|unicode`, ANSI name inline), GObjects
  (`ObjectInternalInteger` at 0x38 == index).
- Engine allocator is tbbmalloc (`scalable_realloc`) → used for FString/TArray writes from Lua.
- Native classes are registered first (~4.5k objects); script packages load later — calibrate only after
  `Function Engine.Actor.Touch` exists, function pointers only after GameViewportClient exists.

### Confirmed layout (x64)

| Type | Field | Offset |
|---|---|---|
| UObject | VfTable / ObjectFlags / InternalIndex / Outer / Name / Class / Archetype | 0x00 / 0x10 / 0x38 / 0x40 / 0x48 / 0x50 / 0x58 (size 0x60) |
| UField | Next | 0x60 |
| UStruct | SuperField / Children / PropertySize | 0x78 / 0x80 / 0x88 (size 0xD0) |
| UFunction | FunctionFlags / iNative / FriendlyName / NumParms / ParmsSize / ReturnValueOffset / Func | 0xD0 / 0xD4 / 0xD8 / 0xE1 / 0xE2 / 0xE4 / 0xF0 (size 0xF8) |
| UProperty | ArrayDim / ElementSize / PropertyFlags / Offset | 0x68 / 0x6C / 0x70 / 0x8C (size 0xA8; subclass data at 0xA8) |
| UBoolProperty | BitMask | 0xA8 |
| UEnum | Names (TArray<FName>) | 0x68 |
| UClass | size | 0x270 (ClassDefaultObject: calibrated at runtime) |

## Status

Done:
1. Probe build ran in game: discovery + dumps verified (names/objects/probe/unpacked image).
2. Full engine (Lua runtime, API, mod manager, hooks) written and **builds**.
3. Second in-game run exposed two bugs, both fixed: log handle conflict between the relaunching processes
   (`_SH_DENYNO` + per-PID fallback); `UFunction::Func` calibrated too early (fixed offset 0xF0 +
   `CalibrateFunctionPointers()` after the viewport exists; `hooks::Maintain()` re-applies re-bound hooks).
4. Run 3 (2026-11-03 16:25): init fine but Lua never started — `GameViewportClient.Tick` is an empty **probe
   event**, so ProcessEvent filters it (IsProbing) and never calls Func. Heartbeat moved to
   `GameViewportClient.PostRender(Canvas)` (dt from QPC). Hooking a bodiless event now logs a warning.
5. Run 4 (16:51): **engine works end to end** — Lua ready, HelloWorld loaded, `ProcessEvent = vtable[70]
   (game+266b5a0)`, selftest 20/20 OK (reads/writes, structs, arrays, is_a, script call, hook 182× in 2s).
   Three bugs found and fixed in the current build (built + deployed, **not yet re-tested in game**):
   - unhooking from inside a dispatch (timer → `h7.unhook`) still ran the copied listener with a freed Lua
     ref ("attempt to call a number value") → listeners get a `removed` flag;
   - map name came from the first WorldInfo found (a stale `NeutralCreatures` package) → now
     `viewport.GamePlayers[0].Actor.WorldInfo` outermost package;
   - `UClass::ClassDefaultObject` not found → engine structs are **packed to 4 bytes** (UStruct::Script TArray
     sits at 0x8C), so the CDO search now steps by 4. Selftest now asserts `get_default()` is non-nil.

6. Run 5 (17:15): CDO fix confirmed (`Class{CDO=1d4}`, `get_default()` → `Default__Actor`); unhook-in-dispatch
   fix confirmed (no error). `tests\hooktest.lua`: **pre-hook blocking works, post-hook return override works**,
   unhook restores. Two more bugs, fixed for run 6:
   - map name still empty: `GamePlayers` is on `Engine` (the viewport client's outer `GameEngine_0`), not on
     GameViewportClient → now `ObjOuter(viewport).GamePlayers[0].Actor.WorldInfo`;
   - hook `args` nil (FFrame layout never found): FFrame scan stepped by 8 but with pack(4) → scan steps by 4.
7. Run 6 (17:20): **everything verified, no errors.** `Map: h7logoentry` → `Map: councilhub`;
   `FFrame layout: Node=+14 Object=+1c Code=+24 Locals=+2c`; hooktest all OK incl. `PostRender.Canvas` =
   `Canvas Transient.CanvasObject`; selftest 20/20 OK + hook fired 176× in 2s.

Notes: Main menu map is `CouncilHub` (`H7CouncilPlayerController`); boot map `H7LogoEntry`.
In run 5's first instance the game stopped ticking ~1 min in while the window was in the background
(main thread in a game-side `SleepEx` loop, not in our code — checked with `cdb -pv`); it resumed after the user
focused it. Treat as background throttling; ticks (and the exec folder) pause while the game isn't focused.

8. Run 7 (18:47): **script→script calls were never hooked** — the VM calls ProcessInternal directly, so a Func
   swap only sees ProcessEvent (C++→script) calls. Replaced by a **bytecode trampoline** (see "Script hooks"
   below): `DrawTransition` (called from PostRender's script) 88 calls/s with args. Example mods: HeroMovement
   (22 → 33 points) and CombatXP (634 → 951) worked; DailyGold and DamageScale silent.
9. Run 8 (19:21): tracer crashed the game — `Unknown code token 04` in `H7GameProcessor.ResolveCombat` (has
   `optional bool isMainResult = false`). Also showed `h7.first("H7AdventureController")` = the archetype
   `H7Config.H7AdventureSetup`.
10. Run 9 (19:32): fixes below verified; **all four example mods work**: `+500 gold for Sephinroth` on NextDay,
    `Damage 22 -> 28` etc. on real hits (forecasts scaled silently), `Combat XP 500 -> 750`, movement ×1.5;
    clean exit. Fixes: default-parm blocks kept in the trampoline; out params read via `FFrame::OutParms`;
    `h7.first`/`find_all` skip CDOs + archetypes (`ObjectFlags & 0x600`). Run 9 still fell back to Func for
    optional params *without* `= default` (e.g. `H7CombatResult.GetMultiplier`).
11. Run 10 (19:37): treating those as "no entry" crashed at the first attack (`Unknown code token 04` in
    `H7CombatResult.IsRedirect(optional int Idx)`) — each such param has a 1-byte `EX_Nothing` placeholder.
12. Run 11 (19:40): parser accepts a `49` block or `0B` per optional param (else falls back to Func, never
    guesses). Tracer hooked all 117 functions through the trampoline (0 fallbacks); a full manual battle ran with
    every hit scaled and CombatXP at the end. **Hook system fully verified.**

### Script hooks (bytecode trampoline) — verified
- Script = `[original optional-param entries] 04 1C <fake native UFunction*> 16 04 0B 53`. ProcessInternal
  (game+266de40) steps statements until `EX_Return` (0x04), then evaluates ONE expression into the result — so the
  leading `04` makes our `EX_FinalFunction` call the return expression. The fake native is a clone of
  `Core.Object.Sin` (Final|Native, iNative 0, Func = `ScriptHookNative`).
- `ScriptHookNative` runs pre listeners, then ProcessInternal on a 0x80-byte copy of the frame with Node = a shadow
  copy of the UFunction (keeps the original Script; jumps index `Node->Script`) and Code = original Script + body
  offset, then post listeners. Natives, latent and iterator functions still use the Func swap.
- Optional params: one entry per optional param at the start of Script — `EX_DefaultParmValue` =
  `49 <u16 len> <expr> 15` (handler game+264a770, tests `GRuntimeUCFlags & 2` = param skipped), or a lone
  `EX_Nothing` (0B) when the param has no default. The engine steps one entry per optional param while binding
  the call, before ProcessInternal, so the entries must stay at the front of the trampoline.
- Out params: on script→script calls they are NOT copied into the callee's locals; `FFrame::OutParms` (+0x3C,
  list of `{UProperty*, PropAddr, Next}` at +0/+8/+0x10) points at the caller's variable (see
  `EX_LocalOutVariable`, GNatives[0x48]). Hook `args` resolve out params through it (`hooks::OutParmAddr`).
- GNatives at game+2352220. Opcodes checked: 0x04 Return, 0x15 EndParmValue, 0x16 EndFunctionParms,
  0x1C FinalFunction, 0x41 (optional, after the return expression), 0x48 LocalOutVariable, 0x49 DefaultParmValue,
  0x53 EndOfScript. Disassemble with `cdb -z <Game>\H7ModEngine\dumps\MMH7Game_unpacked.exe -c "u <addr>; q"`.

### Gameplay hook targets (from the SDK sources, verified in game)
- New day: `H7AdventureController.SetNextPlayer` → `mCalendar.NextDay()` (calendar is `Transient.H7Calendar_0`).
  Gold: `player:GetResourceSet():ModifyCurrency(n, true)`; local player: `controller:GetLocalPlayer(true)` (native).
- Damage: `H7GameProcessor.ResolveCombat` → `ResolveCombatSingle` → `DamageEffectAttack_X2C` →
  `CalculateDamage(out H7CombatResult Result, int resultIdx, bool forecast)` (ends with `Result.SetDamage`;
  heals pass through it too) → `ApplyDamage`. `_X2C` functions are ordinary script (no natives by that name).
- XP: `H7CombatArmy.GetKilledCreaturesXP()`; movement: `H7EditorHero.GetMovementPoints()` (caravans are
  H7EditorHero too). `H7EditorHero.AddXp` is native.

Next steps:
1. More example mods; update H7_REFERENCE.md. (`dist\H7ModEngine.ini` now ships DumpProbe/DumpImage=0, DumpSDK=1;
   deploy never overwrites an existing game ini; the dev copy has DumpSDK=1, DumpProbe=1, DumpImage=0.)

Open risks: natives with iNative called through ProcessEvent (SDK trick: clear iNative); hooks on native
functions get no params and can't block; protector integrity checks (none hit so far — we only touch heap data).

## Lua API (implemented)

- Logging: `h7.log/warn/error`, `print` → `logs\engine.log` with mod prefix
- Lookup: `h7.find("Class Pkg.Name" | "Pkg.Obj")`, `h7.find_class`, `h7.find_all(class [, include_templates])`,
  `h7.first(class)` (nil if none; both skip class defaults and archetypes), `h7.enum(name)`, `h7.properties(x)`,
  `h7.functions(x)` (entries: name/type/owner/flags), `h7.signature(func)` → UnrealScript-style signature,
  `h7.to_table(v)`
- Objects: `obj.Prop` read/write, `obj:Func(args)` → return + out params, helpers `get_name/get_full_name/
  get_path_name/get_class/get_outer/get_default/get_super/get_address/is_valid/is_a/call`
- Structs/arrays: live proxies; `#arr`, `arr[i]`, `arr:add(v)`, `arr:remove(i)`
- Hooks: `h7.hook(path, fn(self, args) -> true blocks)`, `h7.hook_post(path, fn(self, args, ret) -> new ret)`,
  `h7.unhook(id)`. Script functions see every call (script and C++ callers); `args` exposes params incl. out
  params (read/write). Natives: no params, can't block.
- Events: `h7.on("tick"|"map_loaded"|"mods_loaded", fn)`, `h7.off(id)`; timers `h7.after/every/cancel`
- Mods: `mods\<Name>\mod.lua` (manifest: name, version, author, description, load_after, enabled) + `main.lua`;
  per-mod env with `mod.name/dir/require/save/load`, optional `on_unload()`; hot reload on file change; `h7.reload()`,
  `h7.mods()`; `[Mods] Disabled=` in ini; `h7.exec("console cmd")`
