# H7ModEngine — resume notes (saved 2026-11-04 ~13:30)

Engineering notebook: findings, verified layouts and the history of in-game runs. For an overview start with
[README.md](README.md), [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (engine developers) and [docs/API.md](docs/API.md)
(modders). The Mods menu (PR #2) is merged into `main`; see [Mods menu](#mods-menu-feature-branch-in-progress).

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
| `tests\*.lua` | In-game scripts: copy into `<Game>\H7ModEngine\exec\` while the game runs. `selftest` (API), `hooktest` (block/override/args/script→script), `sigdump` (signatures), `daytrace` (call-count tracer for ~117 combat/calendar functions), `modwindow_autotest` (Mods window, run with `tests/run_ingame.sh`, which also takes screenshots) |
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

## Mods menu (feature branch, in progress)

User request: when the engine is on, a main menu entry **Mods** lists every mod with its description and lets
players enable/disable it. The user wants it to look like the game's **map selection screen** (table on the left,
details on the right, Back/Continue buttons), not a small popup.

Committed on `feature/mods-menu` (dff0a32, local, not pushed); the 2026-11-04 batch below is not committed yet.
- Engine (`runtime.cpp`): mod state — manifest `enabled`/`hidden`/`category`; ini `[Mods] Enabled=`/`Disabled=`
  override the manifest; `h7.set_mod_enabled(name, on)` writes the ini, the mod loads/unloads on the next frame and
  `mod_toggled(name, enabled)` fires; `h7.mods()` returns name/version/author/description/category/enabled/loaded/
  hidden. Disabled mods are still discovered (listed) but not loaded.
- Engine (`bindings.cpp`): delegate params accept `{object=, func=}` (FScriptDelegate {UObject*, FName}).
- Example mods have a `category` (Combat/Economy/Adventure/Example). All of it is in `docs/API.md`.
- `dist/mods/ModManager/` (`hidden = true`): uses the map selection window. The previous popup version
  (custom-difficulty popup, paged) worked; it was never committed.
- `tests/swfdump.lua` (dumps SwfMovie.RawData to `dumps\swf\`), `tests/menulab.lua` (main menu button),
  `tests/modlistlab.lua` (fills the map selection window with mods via exec — verified with a screenshot).
- `re/swfinfo.py <file.swf> [--strings]`: lists a SWF/GFX movie's tags, symbols and AS3 classes with members
  (instance and static).

Batch 2026-11-04 (all **verified in game**, run 17 + hot reloads; game kept running):
- Engine: hooks run inside `ModScope(mod)` (`ModScope` in runtime.h, `SetCurrentModId`): logs, timers, handlers and
  hooks created in hook callbacks belong to the mod. Verified: `[ModManager] Mods button installed`.
- Engine: `WriteElem` handles `ArrayProperty` (Lua sequence or engine array replaces the contents; old buffer and
  strings freed with `EngineRealloc(p, 0)`); `CallUFunction` frees strings/arrays left in its parameter buffer
  (`FreeOwned`). Selftest 22/22 incl. "write array" and "array argument" (`Invoke("push", {ASValue...})`).
- ModManager (final design, verified by `tests/modwindow_autotest.lua`):
  - columns Mod | (blank, "-") | Version | Category | Status; details panel = "Creator" (author) + Description;
    picture frame shows the game's "no picture" label; Generate map and the 2nd column's filter hidden.
  - Continue = Enable/Disable; on `mod_toggled` the window is filled again (restore() + open()) so Status is
    right — the selection goes back to the first row (rows can't be changed or selected from Lua).
  - `change()` remembers each part's original value once per game session (`ui.originals`), `restore()` sets
    each one in its own pcall — a failed restore once lost the originals and left "Mods" texts in the game's
    map selection (repaired by hand with an exec script).
- Test tooling: `tests/run_ingame.sh <test.lua> <shotdir> [timeout]` copies a test into exec, prints its log
  lines and saves a screenshot (`tests/shot.ps1`, PrintWindow of the game window, any display) for every
  "SHOT <name>" line, ending at "DONE". **The game only draws while it is visible**: covered, PrintWindow returns
  the last frame — trust the logged state, not the picture.

How ModManager drives the window:
- Main menu button: `AttachMovie("H7TextButton", "mBtnMods", -1, GFxClikWidget)` on `H7MainMenuCntl.mMainMenu`,
  `AddEventListener("CLIK_click", {object=cntl, func="Options"})`; its clicks arrive at
  `H7MainMenuCntl.Options` with **empty EventData**, so a pre-hook checks `mouseX/mouseY` against the button and
  blocks. Layout: Extras/Options move up one step (80px), Mods takes Options' slot (y=860), Quit stays (940).
- Window: `sel = h7.first("H7MapSelectCntl")`, `list = sel.mMapSelection` (H7GfxMapList). Open =
  `mMainMenu:SetVisibleSave(false); list:Reset(); list:Update(sel.mMapThumbnail)`. Rows: `sel:CreateArray()`, per
  row `sel:CreateObject("Object", GFxObject)` with Index, Name, Creator, Description, PlayerCount, Size, Type
  (optionally VictoryConditions/LoseConditions arrays); then `list:SetObject("mPolledMaps", arr)`,
  `list:Invoke("AddMaps")`, `list:Invoke("ListingMapDone")`.
- Parts on `list`: mLblTitle, header labels mLblMapName/mLblVictoryCondition/mLblPlayers/mLblMapSize/mLblMapType,
  mMapThumbnail (1345,110 493x493), mLblNoThumbnail ("Brak obrazka"), mMapDetails (1322,634 550x311),
  mBtnContinue, mBtnBack, mGenerateMap; `mMapList` has the filters mDDCondition/mDDPlayerAmount/mDDMapSize/
  mDDMapType (filled from the row data), mBtnClearFilters and the sort buttons.
- Clicks come back as UnrealScript calls on `H7MapSelectCntl`: `MapClicked(clickedMapIndex)` (also once after
  AddMaps for the first row), `BtnContinueClicked(selectedListIndex, isCampaign)`, `BtnGenerateMapClicked()`,
  `BtnBackClicked()` (→ ClosePopup: hides the window, shows the main menu). ModManager pre-hooks them while
  `ui.showing`; Back is not blocked, it restores the window first.

Other gotchas:
- **GFxObjects get garbage collected**: the UObject wrappers from GetObject/AttachMovie are freed when no
  UnrealScript object references them (the Flash clip stays). Never cache them across frames; keep the instance
  name and `GetObject` again. Objects held by script vars (`cntl.mMainMenu`, `sel.mMapSelection`) are safe.
- Hook `args` are by name (`args.clickedMapIndex`), not index.
- `mBG`, `mBtnOK`, `mBlockLayer` of H7CustomDifficultyPopUp are not reachable via GetObject (nil).
- An exec script that errors or is empty still gets renamed to `.done` and overwrites an older `.done` of the
  same name — keep lab scripts in `tests\` and copy them in.
- Ubisoft Connect restarts the game process once at launch: a watcher must not treat the first exit as the end.

What does not work from Lua (Scaleform/UE3 limits found this session):
- `GetObject` returns nil for members holding AS-created Arrays/Objects/Dictionaries (`mMapList.mMapItems`,
  `mVisibleMaps`, `mSelectedMapListItem`, `mData`, `mMapDetails.mChildsAdded`, `Loca.mLocaDictionary`); it works
  for display objects (timeline instances) and for objects Unreal created (`mPolledMaps`). `GetVariableObject
  ("global.Loca")` returns the class, but not its statics. Invoke on mMapList's `clearList`/`sortByNameDown`
  did nothing visible; `AddMaps` adds rows on top of existing ones (sorted by name) and selects the first row.
- **The movie caches translations** (`Loca.mLocaDictionary`, static): `FlashLocalize(locaKey)` is asked once per
  key and movie. Never override a shared key (VICTORY_CONDITIONS, LOSE_CONDITIONS, DESCRIPTION, CREATOR...):
  the game's own map list keeps the override until restart. Row texts also go through it: all-caps values are
  taken for keys and shown as `[VALUE]` when unknown.
- Details panel: shows a Creator line ("Edytor kampanii", key CREATOR) only if `Creator` is set, victory/lose
  lines only if those arrays are set; the list's 2nd column is `VictoryConditions[0]` ("-" if missing).
- Status colors: not possible (rows unreachable); H7Label `htmlText` resets the font size.

Next steps:
1. Commit this batch on `feature/mods-menu` (ask the user), then push / open a PR (ask; `gh` not installed).
2. Optional polish: keep the selection after Enable/Disable (would need a way to select a row by index);
   localized captions for "Mods", "Enable", "Disable" (H7Loca has the game's language).
3. More example mods / API work.

## Object factory + H2Enchantress faction mod (2026-11-04)

Engine part: branch `feature/object-factory` (stacked on `docs/readme-dev-guide`). The mod lives in its own
repository, `N:\Projects\H2Enchantress` (deploy: copy its `H2Enchantress` folder to `<Game>\H7ModEngine\mods\`).

User request: a "serious" mod porting the Heroes II Enchantress (Sorceress) town as a **new** faction (user chose
a real extra faction over reskinning Sylvan).

Engine (`src/ue3/factory.*`, bindings): `h7.clone(src, name [, outer [, extra_flags]])`, `h7.new_object`,
`h7.package(name)`, `h7.name(str)`, `h7.can_create()`, `h7.flags` (Public/Standalone/Transient/Archetype/RootSet).
Calls the game directly at fixed RVAs (checked against their first bytes, else disabled):
StaticConstructObject game+26e13b0 (found in execNew, GNatives[0x11]; execNew refuses Actor classes, so no script
`new`), FName(TCHAR*, FNAME_Add) game+26cb5c0, GetTransientPackage game+26bea20. With a template the
SubobjectRoot is INVALID_OBJECT (like execNew). RootSet is OR'ed into ObjectFlags after construction.
Clones of archetypes keep RF_ArchetypeObject and `H7GameUtility.IsArchetype` is true for them.
Also fixed: `h7.to_table` expanded objects inside arrays (C stack overflow); `Core.DynLoadObjectProperty` (H7's
lazily loaded object refs, e.g. H7Faction.mStartTown, H7Creature/H7EditorHero.mVisuals, town screen, icons) is
now read/written as an object property.

Key facts learned:
- DynLoad fields load on demand from the object's linker ("Cannot find DynloadProperty 'mVisuals' serialized in
  Object X and its ArchetypeParents" -> fatal error in combat). Copies have no linker: call
  `src:DynLoadObjectProperty(name)` / `DynLoadObjectArrayProperty("mTownAssets")` on the original before cloning,
  and block the game's Del* release functions for copies and their sources.
- Build the faction at the main menu (councilhub), not on h7logoentry: GetStartTown returns none there.
- This install is heavily modded: 14 factions before ours (Frost, Hades, Sanctuary, Xel, Citadel, Mercenary,
  Conflux), Sylvan town has extra mod dwellings; Sylvan's town names point at H7FactionSylvan_extra.
- Creatures have a second stat set for skirmish (`mSkirmish*`); cost is `mUnitCost`/`mSkirmishUnitCost`.
- Duel: armies offered per faction by their creatures' faction (H7GameData.GetDuelArmies), heroes from
  mDuelHeroes; without them the army is empty.
- Names are localised by object name; copies show the fallback text, so copy GetName()/GetDesc() texts.

Mod (data.lua lineup/names/heroes, stats.lua, faction.lua builder, hooks.lua):
clones Sylvan faction/town/fort/map dwellings/all buildings/hero classes/heroes/random hero/town names/duel
armies+heroes into package `H2Enchantress`, remaps references, blocks dwellings without an H2 counterpart,
registers in H7GameData. Verified in game: faction in GetFactions (all policies), duel with Enchantress army runs.
Lab script `tools/factionlab.lua` in the mod repo dumps Sylvan's structure to `dumps\factionlab.txt`.

Next: skirmish/adventure map (town screen, recruiting, AI), save/load, H2 abilities (dwarf magic resistance,
unicorn blind, phoenix fire immunity), stat balance (Phoenix initiative low, Dwarf scaling capped at x3),
H2 hero portraits/bios.

## Mod settings + Mods button fix (2026-11-04, branch `feature/mod-settings`, verified in game)

- Manifest `settings = { {key, label, description, type, default, min, max, step, options, reload}, ... }`;
  values in the ini `[Mod.<name>]`; `mod.settings` proxy (read/assign/pairs), `h7.mod_settings`,
  `h7.set_mod_setting`, event `setting_changed`; `reload = true` restarts the mod. `h7.mods()[i].settings` = count.
  Example mods use settings (DailyGold gold, CombatXP/DamageScale/HeroMovement multipliers, DamageScale log,
  HeroMovement who). `tests/settingstest.lua`: 13/13 OK.
- ModManager: the map window's Generate Map button = Settings (mods with settings) / Default (settings view);
  settings view rows: label, value, range, "Changed"; Continue = Change; Back = back to the mod list. Details panel
  lists current values. Selection resets to row 1 after each change (rows unreachable: `mMapList.masked` and its
  children are AS-created; `GetObject` returns nil).
- Mods button in sub-menus: the movie hides its own buttons (`visible = false`) in sub-menus; ModManager copies
  Options' visible/alpha/x to Mods every 0.1 s. Verified with real clicks.
- `GFxObject:Invoke("GotoSPMenu")` etc. on the main menu does NOT open sub-menus; drive the UI with real input:
  `tests/click.ps1 <x> <y>` (window pixels, as in shot.ps1 screenshots; brings the game to the front). Clicks
  right after a list refill can be lost; wait ~2 s. Main menu buttons (2048x1080 window): Jeden gracz 1730,330;
  Mods 1730,890; sub-menu Wstecz 1730,970; map window Back 240,1015, Continue 1810,1015, Generate 1025,978,
  rows from y=224 every 49 px.

### Settings panel (2026-11-04, follow-up on `feature/mod-settings`, verified in game)
User wanted sliders/check boxes, the description in the picture frame and the settings below it. ModManager:
- `panel.lua`: description = H7Label (multiline) in the frame (1366,128 452x456); hidden: mMapThumbnail,
  mLblNoThumbnail, mMapDetails, mGenerateMap. Settings area 1342,640 (470 wide), 5 rows of 48 px.
- Controls are clips from H7MapSelect's own library (no checkbox/slider *component* symbol exists there):
  slider = `H7Slider_BG_Gold` (a 14x423 vertical bar, rotated -90) + `H7Slider_Gold_Thumb`; check box =
  `H7GUICommandButton` (36 px, DisplaySelectState + alpha 0.35 when off); choices = `H7LeftArrow`/`H7RightArrow`
  + H7Label. `H7DropDown` also attaches (AddItem API untested).
- Input: Flash listeners on attached clips call back with EMPTY EventData (no type/target) and only for
  clicks, so the panel ignores Flash events and hooks `Engine.Console.InputKey` (Key "LeftMouseButton", Event 0
  pressed / 1 released / 2 repeat) and hit-tests `window:GetFloat("mouseX"/"mouseY")` (window coords = screen
  pixels - 64 in x here). The press is handled on the next tick: at InputKey time Flash's mouse position is
  still the old one. Slider drag follows the mouse in `tick`, saves on release.
- Synthetic input: the game reads mouse movement as raw input; `SetCursorPos` alone doesn't move the game's
  cursor. `tests/click.ps1` nudges with relative moves and drags with absolute VIRTUALDESK moves (the game
  window is on a monitor with negative coordinates).
- Number box (`control = "input"`, default for numbers without a range): H7TooltipBG frame + its own H7Label
  (created after the frame so it draws on top; clips stack in creation order). Keys come from
  Console.InputKey; key names are lower-case as the engine first registered them ("seven", "enter"), so the
  panel compares lower-cased. A post-hook returning true marks keys as handled while editing (Escape then
  doesn't close the window). `tests/keys.ps1 <keys>` types into the game (keybd_event works, unlike mouse).
  H7DropDown as a box left a grey slot where its arrow was; don't reuse clip names across clip types
  (ensure() returns the existing clip of that name).
- Hot-reloading ModManager while the window is open loses the panel state; re-clicking the selected row
  doesn't fire MapClicked (select another row first).

## Example mods removed from the game install (2026-11-04)
The game's `H7ModEngine\mods` now holds only ModManager and H2Enchantress; the five example mods (and their ini
settings, HelloWorld's saved data) were deleted there but stay in `dist\mods` for the docs and tests.
`build.bat deploy` installs only ModManager; `build.bat deploy examples` installs them all again (needed before
running `tests\settingstest.lua` / `modwindow_autotest.lua`).

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

## Debug console (2026-11-05, branch `feature/debug-console`, uncommitted)

User request: a debug console, enabled from the Mods menu, opened with a key set in H7ModEngine.ini; then
"intellisense" for game commands and one-time multi-line Lua scripts.
- Engine: `h7.log_lines([after])` (ring of the last 2000 log lines in `core/log.cpp`), `h7.config(section, key
  [, default])`, `h7.clipboard([text])`; `ConfigDefault` adds `[Console] Key=Tilde` to existing inis at startup.
- Mod `dist/mods/DebugConsole` (bundled in the DLL like ModManager, `enabled = false`, category Developer):
  `main.lua` draws on the Canvas in a PostRender post-hook, reads keys from `Console.InputKey` and text from
  `Console.InputChar` (both report handled while open), runs Lua in its own env (`return <expr>` first; in a script
  the last line's value), `/cmd` via h7.exec, multi-line input (Enter on `<eof>`-incomplete chunks adds an indented
  line). `suggest.lua`: popup for game commands (exec functions, FUNC_Exec 0x200, of the viewport/console/
  interactions, local PC, CheatManager, HUD, Pawn, PlayerInput, Game, plus `Console.ManualAutoCompleteList` from
  the game ini) and Lua names/members (own class ranked first, signatures via h7.signature for visible rows only).
- Lua logic is tested offline with a stubbed `h7` (a lua.exe built from `third_party/lua/onelua.c`).
- Verified in game 2026-11-05 (user run, v1): enabled from the Mods menu, Tilde opens it, typing through
  InputChar, expressions/variables/`/command` work, drawn visibly. v2 Lua (suggestions, scripts) hot-reloaded into
  the running game: `tests/consolesuggest.lua` -> 311 commands at the main menu in 26 ms (83 CheatManager, 50
  H7PlayerController, ...), `/stat ` continues into multi-word entries, member signatures OK.
- 2026-11-05 09:56, new DLL deployed: selftest 25/25 OK at the main menu (incl. log_lines, config, clipboard
  round trip with Polish letters), hooktest all OK, hook fired 214x in 2 s; bundled files 0 of 7 written (game
  copies already current). Not yet seen on screen: the suggestion popup and multi-line input (logic tested
  offline and via `tests/consolesuggest.lua` in game).
- 2026-11-05 ~10:00, user screenshot: console draws over the GFx main menu; command popup used. Fixes (hot-loaded,
  verified with `tests/consolesuggest.lua`): members after read-only calls (`h7.first(...)`, `:Get*`/`:Is*`,
  1 s cache) -> `h7.first("H7CouncilPlayerController"):Get` lists its getters with signatures; commands the game
  rejects ("Command not recognized") are remembered per full command in DebugConsole's saved data and hidden
  (shipping build: `Stat FPS`/`Stat UNIT` work, `Stat GAME`/`MEMORY`/`PHYSICS` don't); exec output trimmed.
- Unrecognized commands hidden up front: `Console.ManualAutoCompleteList` has 92 entries (dump:
  `dumps\manual_commands.txt`). `tests/probecommands.lua` runs the 33 Stat/Show ones twice (toggle on/off): all 13
  Show and Stat FPS/UNIT/LEVELS/SOUNDWAVES/SOUNDCUES/SOUNDS work; 14 Stat entries are "Command not recognized" ->
  shipped in `DebugConsole/known.lua`. The mod also probes new Stat/Show entries once per install at load or
  map_loaded (`saved.probed`); in game it found the same 14. Other entries (Exit, Open, RestartLevel, sound
  changes, Set, memleak, movie capture...) are never run automatically; failures are learned when typed.
- Typed suggestions (user chose: TypeScript-like notation, parameter hints, types kept in the docs). Signature
  lines in `docs/reference/{h7,mod,objects}.md` are now typed; `tools/docs/console_api.py` generates
  `DebugConsole/api.lua` (51 entries; `--check` runs in build.yml). Rows: name | kind | signature/type | owner;
  game signatures converted from h7.signature (`optional` -> `?`, out params also listed after `->`); hints for
  h7/mod/print (also through aliases, by function value), object methods and exec commands, current argument
  underlined. Engine: `h7.properties` entries gain `type_name` (UnrealScript type, e.g. Vector) - built, not yet
  deployed (game running); until then property rows show Struct/Object/Array. Verified in game with
  `tests/consolesuggest.lua` (Lua hot-load, 2026-11-05 10:22); popup look not yet seen on screen.
- 2026-11-05 10:25, DLL with `type_name` deployed: selftest 26/26 OK (property type_name -> Vector);
  consolesuggest shows real property types (ViewportConsole: Console, Actor: PlayerController,
  ActivePPOverrides: array<PostProcessSettingsOverride>; Viewport/ViewportFrame are `Pointer`, i.e. native pointers).

## Networking + online lobby (branch `feature/networking`, 2026-11-05, uncommitted)

Goal: replace Ubisoft's shut-down online services (design: [docs/ONLINE.md](docs/ONLINE.md); desync plan:
[docs/DESYNC.md](docs/DESYNC.md)). The user decided: the online mod is **part of the engine** (bundled like
ModManager); the Cloudflare Worker lobby is a **separate repo**, `N:/Projects/H7Lobby` (README has the API).

Done (builds; **verified offline only**, not yet in game):
- `src/net/http.*` (WinHTTP, blocking, worker threads), `src/net/upnp.*` (IUPnPNAT + local IP via a connected
  UDP socket), `src/lua/netlib.cpp`: `h7.http.request/get/post/cancel`, `h7.json.encode/decode/array/null`,
  `h7.net.local_ip/upnp_add/upnp_remove`. Results are queued and delivered by `PumpNet` each tick inside the mod's
  scope; `RemoveModNet` drops callbacks on unload. Links winhttp, ws2_32, ole32, oleaut32.
- `build.bat nettest [online]`: offline test host `tests/offline/nethost.cpp` + `net.lua`. 2026-11-05: 26/26 OK
  with `online` (HTTPS GET/POST/404/DNS error via postman-echo.com, unload/cancel drop callbacks, **UPnP map +
  unmap on the user's router worked**, router reported an external IPv4). CI runs the JSON part.
- Gotcha: vcvars64.bat fails when the calling batch's paths contain `..` segments (separate test .bat was dropped
  for that reason; the test lives in build.bat). npm 11 blocks install scripts (H7Lobby approves workerd/esbuild).

In-game run 2026-11-05 18:11 (main menu, councilhub): **verified**. Engine starts clean (no errors in the log),
`ConfigDefault` added `[Online] LobbyUrl` to the existing ini. `selftest.lua` 30/30 OK incl. json round trip,
local_ip, and `h7.http.get("https://example.com/")` -> 200, 577 bytes, callback ~70 ms later on the game thread.
`hooktest.lua` all OK (block / override / restore / PostRender args / script->script 109 calls).

### OnlineLobby mod (2026-11-05, verified in game on one PC)
- Bundled mod `dist/mods/OnlineLobby` (enabled). Hooks: `H7MainMenuCntl.CheckConnection` (block + return true),
  `CreateOnlineGame` pre (IsLAN=true), `OnCreateH7GameSessionComplete` post (UPnP, register, heartbeat 15 s),
  `UpdateOnlineGameStarted` / native `DestroyOnlineGame` / map councilhub (unlist), `SearchOnlineGames` pre (LAN +
  lobby fetch), `StartFilteringBrowserList` pre (append entries), `JoinSelected` pre + `H7GFxRequestPopup`
  `YesButtonClicked`/`NoButtonClicked` post (the "join anyway?" prompt; YesNoPopup with None delegates is fine).
- The hosted lobby map is `h7main`. The host listens on **UDP 27015 only** (no TCP listener); LAN beacon UDP 14001.
- Session record (PlatformData of a LAN result) = **0x38 bytes**: vtable game+1D10EB8, sockaddr_in at +8, embedded
  object (vtable game+1EBB900) at +0x18, TArray at +0x28. A 0x20-byte fake crashed the game at the join timeout
  (`DestroyOnlineGame` freed heap garbage; the first crash looked like a Scaleform render-thread null vcall).
  Fixed; the timeout now shows the normal popup.
- `h7.game_content`: 29 packs / 593 packages on the user's install, ~70 ms. Edition = base/modded.
- UPnP stopped answering on the user's router after 17:40 (Windows' own NATUPnP object finds nothing either);
  not a code issue. The engine now serialises UPnP requests and waits 12 s.
- The browser's Host column truncates long names; the user wants a separate version column. Rows are built in
  ActionScript from fixed fields, so a real column needs a patched Flash movie (open).

Next:
1. Two-PC test of a real online match. Version column (Flash patch) if the user wants it.
2. (done, see above) The bundled OnlineLobby mod (hooks in docs/ONLINE.md). Found 2026-11-05: for LAN queries
   `StartFilteringBrowserList` only filters by game type and calls `InitializeBrowserList(SearchResults)` (no mesh
   beacon), so lobby entries can be appended to `SearchResults` before that. First in-game checks it depends on: can a pre-hook change
   `CreateOnlineGame`'s `IsLAN` arg; dump a LAN search result's `PlatformData` (FSessionInfo layout) so the engine
   can build lobby results (`make_lan_result`, not written yet); direct join `start ip:27015` between two PCs.
2. ~~Deploy H7Lobby~~ done 2026-11-05: live at https://h7-lobby.whaileee.workers.dev (Workers Builds from
   GitHub Whaileee/H7Lobby `main`); register/list/heartbeat/delete round trip checked in production. Engine default
   `[Online] LobbyUrl` (default ini + `ConfigDefault` in dllmain.cpp).
