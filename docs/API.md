# H7ModEngine modding API

H7ModEngine lets you write mods for **Might & Magic Heroes VII** (v2.2.1) in **Lua 5.4**. A mod can read and change
any game object, call any UnrealScript function, and hook game functions to watch, change or block what they do.
Mods reload automatically while the game runs, so you can edit and see the result without restarting.

- [Installation](#installation)
- [Your first mod](#your-first-mod)
- [Mod folder and manifest](#mod-folder-and-manifest)
- [Finding what to change](#finding-what-to-change)
- [Engine objects](#engine-objects)
- [Values and types](#values-and-types)
- [Looking things up](#looking-things-up)
- [Hooks](#hooks)
- [Events and timers](#events-and-timers)
- [Saving data](#saving-data)
- [Logging and debugging](#logging-and-debugging)
- [Configuration](#configuration)
- [Reference](#reference)
- [Limitations](#limitations)

## Installation

1. Copy `dinput8.dll` into `<Game>\Binaries\Win64\`.
2. Copy the `H7ModEngine` folder into the game folder (`<Game>\H7ModEngine\`).
3. Start the game as usual (through Ubisoft Connect).

`<Game>\H7ModEngine\` then holds everything the engine uses:

| Folder | Contents |
|---|---|
| `mods\` | One folder per mod |
| `logs\engine.log` | Everything the engine and your mods log (the previous run is in `engine.prev.log`) |
| `data\` | Saved mod data (`mod.save`) |
| `lib\` | Shared Lua libraries any mod can `require` |
| `exec\` | Drop a `.lua` file here to run it once in the running game (for testing) |
| `dumps\` | `objects.txt` / `names.txt`: an index of every engine object, written at startup |
| `H7ModEngine.ini` | Configuration |

## Your first mod

Create `<Game>\H7ModEngine\mods\MyFirstMod\main.lua`:

```lua
h7.log("Hello from " .. mod.name)

h7.on("map_loaded", function(map)
    h7.log("Entered map: " .. map)
end)
```

Start the game and open `logs\engine.log`:

```
[19:32:14.962] [MyFirstMod] Hello from MyFirstMod
[19:32:14.962] Loaded mod MyFirstMod
[19:32:14.963] [MyFirstMod] Entered map: h7logoentry
[19:32:19.583] [MyFirstMod] Entered map: councilhub
```

Now edit `main.lua` while the game runs and save it: within a second the engine unloads the mod and runs it
again. Errors don't crash the game; they are logged with a stack trace:

```
[19:40:01.120] ERROR in MyFirstMod/main.lua: ...\mods\MyFirstMod\main.lua:3: attempt to call a nil value ...
```

A real gameplay mod is just as short. This one gives the human player 500 gold every new day:

```lua
h7.hook_post("MMH7Game.H7Calendar.NextDay", function(calendar)
    local controller = h7.first("H7AdventureController")
    local player = controller and controller:GetLocalPlayer(true)
    if not player then return end
    player:GetResourceSet():ModifyCurrency(500, true)
    h7.log("+500 gold for " .. player:GetName())
end)
```

## Mod folder and manifest

```
mods\
  MyMod\
    mod.lua      -- manifest (optional)
    main.lua     -- runs when the mod loads (required)
    util.lua     -- extra files, loaded with mod.require("util")
```

`mod.lua` returns a table. Every field is optional; without a manifest the folder name is the mod name.

```lua
return {
    name = "MyMod",              -- default: folder name
    version = "1.0",
    author = "You",
    description = "What the mod does.",
    load_after = { "OtherMod" }, -- load after these mods (if they are installed)
    enabled = true,              -- false: off until a player turns it on
    category = "Combat",         -- shown (and filterable) in the Mods menu
    hidden = false,              -- true: not listed in the in-game Mods menu
    settings = { ... },          -- values players can change in the Mods menu, see Settings below
}
```

**Load order.** Mods load alphabetically by name, except that a mod is moved after every mod it lists in
`load_after`. After all mods have loaded, the `mods_loaded` event fires.

**Turning mods on and off.** Players use the **Mods** entry in the main menu (the bundled ModManager mod),
which lists every mod with its description and an Enabled/Disabled toggle. A change applies at once: the mod
loads or unloads on the next frame. The choice is stored in `H7ModEngine.ini` (see
[Configuration](#configuration)) and overrides the manifest's `enabled`. Mods can do the same with
`h7.set_mod_enabled(name, on)`; the `mod_toggled` event reports each change.

**Settings.** A mod can offer values that players change in the Mods menu: selecting a mod shows its description
and, below it, its settings as controls.
Declare them in the manifest:

```lua
settings = {
    { key = "multiplier", label = "Damage multiplier", default = 1.25, min = 0.25, max = 4, step = 0.25,
      description = "All combat damage is multiplied by this." },
    { key = "gold", label = "Gold per day", default = 500, min = 0, max = 100000, control = "input" },
    { key = "log", label = "Log hits", default = true },
    { key = "who", label = "Applies to", options = { "Human player", "Everyone" }, default = "Human player" },
    { key = "greeting", label = "Greeting", type = "text", default = "Hello" },
}
```

| Field | |
|---|---|
| `key` | Name used in code (required) |
| `label`, `description` | Shown in the Mods menu (`label` defaults to `key`) |
| `type` | `"bool"`, `"number"`, `"choice"` or `"text"`; inferred from `default` (and `options`) when left out |
| `default` | Value until the player changes it |
| `min`, `max`, `step` | Numbers: values are clamped to `min`..`max` and rounded to a multiple of `step` (counted from `min`) |
| `options` | Choices: the allowed values, in the order the menu cycles through them |
| `reload` | `true`: the mod restarts when the value changes (for values only read at load time) |
| `control` | Numbers: how the Mods menu shows it. `"slider"` (default with `min` and `max`), `"input"` (a box the player types the number into; default without a range) or `"arrows"` (steps of `step`) |

Read the current values from `mod.settings` (`mod.settings.multiplier`); it always has the latest value, so read
it when you need it rather than copying it once. Assigning (`mod.settings.log = false`) changes the setting like
the menu does. `pairs(mod.settings)` lists them. Values are stored in `H7ModEngine.ini`, section
`[Mod.<name>]`. When a value changes the `setting_changed` event fires (`mod`, `key`, `value`). Other mods
and exec scripts use `h7.mod_settings(name)` (a list of the declarations with their current `value`) and
`h7.set_mod_setting(name, key, value)`, which returns the stored (clamped) value and raises an error for an
unknown key or an invalid value. In the menu, numbers get a slider (drag or click it) or a number box (click it,
type, Enter saves, Escape cancels, clicking elsewhere saves; the value is clamped to the range), on/off settings
a check box, choices arrows that step through the values; text settings are shown but can only be changed in
code or the ini. Every change is saved at once. The panel has room
for five settings; further ones are only listed as being in the ini.

**Each mod has its own globals.** Global variables you create stay inside your mod; reading a global you didn't
create falls through to the shared environment (`h7`, `string`, `math`, `table`, ...). Two mods can both define
`function on_unload()` without clashing.

**The `mod` table** is available in every mod:

| Field | |
|---|---|
| `mod.name`, `mod.version`, `mod.author` | From the manifest |
| `mod.dir` | Full path of the mod folder |
| `mod.require(name)` | Runs `<mod folder>\<name>.lua` in the mod's environment once and returns its result (or `true`); later calls return the cached result |
| `mod.save(table)` / `mod.load()` | Persistent data, see [Saving data](#saving-data) |
| `mod.settings` | The mod's settings: read the current values, assign to change them (see Settings above) |

**Unloading and hot reload.** About once a second the engine checks each loaded mod's folder (including
subfolders) for changed `.lua` files. When one changed, the mod is unloaded and its `main.lua` runs again.
Unloading calls the mod's global `on_unload()` if it defines one, then removes **everything the mod
registered**: hooks, event handlers and timers. You don't have to clean those up yourself.

Hot reload only covers mods that are already loaded. To pick up a new mod folder or a mod you just enabled,
call `h7.reload()` (for example from the `exec` folder) or restart the game.

**Shared libraries.** The standard `require("name")` loads `<Game>\H7ModEngine\lib\name.lua`. Use it for code
several mods share.

## Finding what to change

Heroes VII's gameplay is written in UnrealScript, and every class, property and function in it is reachable from
Lua. To find the right one:

- **Read the UnrealScript sources.** Decompiled sources for the game (the `MMH7Game` package has about 1400
  classes) are the best map: they show what each function does and who calls it. Search them for the
  behaviour you want to change, then hook the function that computes it.
- **Search `dumps\objects.txt`.** One line per engine object, written at startup:
  `12345 00007ff4fd9f4f40 Function MMH7Game.H7Calendar.NextDay`.
- **Ask the engine.** From a mod or the `exec` folder:

  ```lua
  h7.log(h7.signature("MMH7Game.H7GameProcessor.CalculateDamage"))
  -- function void MMH7Game.H7GameProcessor.CalculateDamage(out H7CombatResult Result, int resultIdx, bool forecast)

  for _, f in ipairs(h7.functions("MMH7Game.H7Calendar")) do h7.log(f.owner, f.name) end
  for _, p in ipairs(h7.properties("MMH7Game.H7Calendar")) do h7.log(p.type, p.name) end
  ```

Hook targets used by the example mods (all verified in game):

| To change | Hook | Notes |
|---|---|---|
| Start of a new day | `MMH7Game.H7Calendar.NextDay` | Called from `H7AdventureController.SetNextPlayer` when the last player ends the turn |
| Damage of an attack | `MMH7Game.H7GameProcessor.CalculateDamage` | Also runs for damage previews (`forecast = true`) and heals |
| Experience after combat | `MMH7Game.H7CombatArmy.GetKilledCreaturesXP` | `H7EditorHero.AddXp` is native and can't be hooked usefully |
| Hero movement points | `MMH7Game.H7EditorHero.GetMovementPoints` | Caravans are `H7EditorHero`s too |

## Engine objects

Engine objects (actors, heroes, players, armies, ...) appear in Lua as references. A reference points at the
live object: reading a property always returns its current value.

```lua
local hero = h7.first("H7AdventureHero")

h7.log(hero)                     -- H7AdventureHero sce_sephinroth.TheWorld.PersistentLevel.H7AdventureHero_0
local player = hero:GetPlayer()  -- call an UnrealScript function
h7.log(player:GetName())
```

**Properties** are read and written by name, exactly as declared in UnrealScript:

```lua
local stack = h7.first("H7CreatureStack")
h7.log(stack.mHumanDamageMultiplier)
stack.mHumanDamageMultiplier = 2
```

Writing a property that doesn't exist is an error; reading one returns `nil`.

**Functions** are called with `:` and take their parameters in declaration order:

```lua
local resources = player:GetResourceSet()
resources:ModifyCurrency(500, true)     -- ModifyCurrency(int modAmount, bool updateGui)
```

- The function's return value comes back first, followed by the values of its `out` parameters, in order.
  Struct and array results come back as plain Lua tables.
- Missing arguments (or `nil`) are passed as zero: `0`, `false`, `""`, no object. UnrealScript default values
  are **not** applied, so pass optional parameters explicitly when their default isn't zero.
- If a property and a function share a name, call the function with `obj:call("Name", ...)`.
- Calls run the function exactly as the game would, including any hooks on it.

**Helper methods** on every object (lowercase, so they never clash with game members):

| Method | Returns |
|---|---|
| `get_name()` | Object name, e.g. `H7AdventureHero_0` |
| `get_full_name()` | Class and path, e.g. `H7AdventureHero sce_sephinroth.TheWorld.PersistentLevel.H7AdventureHero_0` (also what `tostring` gives) |
| `get_path_name()` | Path without the class |
| `get_class()` | The object's class (an object) |
| `get_outer()` | The object that contains it |
| `get_default()` | On a class: its default object (`Default__ClassName`) |
| `get_super()` | On a class, struct or function: its parent |
| `get_address()` | Memory address as an integer (useful as a table key) |
| `is_valid()` | `false` once the game has destroyed the object |
| `is_a(class)` | Whether the object is of `class` or a subclass (class object or name) |
| `call(name, ...)` | Calls a function by name |

Two references to the same object are equal (`==`). An object can be destroyed while your mod still holds a
reference (for example when a map unloads); using it then raises *"engine object ... is no longer valid"*. Don't
keep object references across maps, or check `is_valid()` first.

## Values and types

| UnrealScript | Lua |
|---|---|
| `int`, `byte`, enums | integer (enums are their numeric value; see `h7.enum`) |
| `float` | number |
| `bool` | boolean |
| `string` | string |
| `name` | string (a name the engine doesn't know yet is added to its name table) |
| object, class, interface | object reference, or `nil` for `none` |
| struct | live struct reference: `v.X` reads and `v.X = 1` writes the field in place |
| `array<T>` | live array reference (see below) |
| fixed array (`int X[4]`) | a Lua table copy; assign a table to write it |
| delegate | `{object = ..., func = "Name"}`; pass the same shape to a delegate parameter, e.g. `widget:AddEventListener("CLIK_click", {object = cntl, func = "Options"})` |

Assigning an object to an object property checks its type. A struct property can be assigned a struct
reference of the same type or a Lua table of fields: `obj.Location = {X = 0, Y = 0, Z = 100}`.

**Arrays** are live views of the engine array:

```lua
local obelisks = player.mVisitedObelisks   -- array<H7Obelisk> property of H7Player
h7.log(#obelisks)                          -- length
local first = obelisks[1]                  -- 1-based, like Lua
obelisks[1] = other                        -- replace an element
obelisks:add(value)                        -- append; returns the new length
obelisks:remove(2)                         -- remove element 2, shifting the rest down
```

Assigning a Lua table (or another engine array) to an array property replaces its contents:
`obj.mNames = {"a", "b"}`. The same works for array parameters of a function call; elements are converted like
properties, so an array of structs takes a table of tables:

```lua
-- GFxObject.Invoke(string Member, array<ASValue> args)
local AS = h7.enum("ASType")
widget:Invoke("setTextColor", { { Type = AS.AS_Number, n = 0x9be37a } })
```

Leaving an array argument out passes an empty array.

An array *returned by a function* is a plain Lua table copy instead (`player:GetHeroes()`), since the engine's
copy is gone once the call returns. UnrealScript's `private`/`protected` don't apply: mods can read and write
every property.

`h7.to_table(value)` copies a struct, array or object's properties into a plain Lua table: handy for logging or
for keeping a snapshot.

## Looking things up

| Function | Returns |
|---|---|
| `h7.first(class)` | The first live instance of `class` (or a subclass), or `nil` |
| `h7.find_all(class [, include_templates])` | All live instances. Class defaults and archetypes (templates stored in content packages) are skipped unless `include_templates` is `true` |
| `h7.find(path)` | The object with this path: `"MMH7Game.H7Calendar"`, or with its class to be exact: `"Class MMH7Game.H7Calendar"`, `"Function MMH7Game.H7Calendar.NextDay"` |
| `h7.find_class(name)` | A class by `"Package.Class"` or bare `"Class"` name |
| `h7.enum(name)` | A table of an enum's values: `h7.enum("EMyEnum").SOME_VALUE` → integer |
| `h7.properties(x)` / `h7.functions(x)` | Lists of `{name, type, owner, offset}` / `{name, type, owner, flags}`, including inherited members (`type` is the engine type, e.g. `IntProperty`; `owner` the declaring class). `x` is a class or struct name, an object, or a struct reference |
| `h7.signature(func)` | The function's declaration as text, e.g. `function string Engine.Controller.GetHumanReadableName()` |

`class` can be a name (`"H7AdventureHero"` or `"MMH7Game.H7AdventureHero"`) or a class object. `find_all` scans
every engine object, so call it when you need it rather than every frame.

## Creating objects

Mods can create new engine objects, most usefully **copies of game data** (archetypes: the templates the game
spawns creatures, heroes, towns and buildings from). A copy starts with every property of its source; change
what you need and add it to the game's lists. This is how a mod adds a new creature, hero or a whole faction.

```lua
local pkg = h7.package("MyMod")                 -- a package of your own: gives copies stable names
local src = h7.find("Creature_Pixie.A_CreaturePixie")
local sprite, created = h7.clone(src, "A_CreatureFrostSprite", pkg, h7.flags.RootSet)
sprite.mNameInst = "Frost Sprite"               -- name shown in game (see below)
sprite.mHitpoints = 20
```

| Function | |
|---|---|
| `h7.clone(source, name [, outer [, flags]])` | A copy of `source` (same class, every property copied) called `name` inside `outer` (default: the transient package). Returns the object and `true`; if `outer` already holds an object called `name` of the same class it is returned with `false` instead, so the same code can run again after a hot reload. Archetypes stay archetypes. `flags` adds object flags |
| `h7.new_object(class, {name=, outer=, template=, flags=, archetype=, replace=})` | A new object of `class`, with properties from `template` (or the class defaults). Fails if `outer` already has an object called `name`, unless `replace = true` |
| `h7.package(name)` | The top-level package `name`, created if it doesn't exist |
| `h7.name(str)` | Adds `str` to the engine's name table and returns it |
| `h7.can_create()` | `false` if the engine couldn't verify the game functions it needs (see the log); the functions above then fail |
| `h7.flags` | Object flags: `Public`, `Standalone`, `Transient`, `Archetype`, `RootSet` |

Things to know:

- **Keep copies alive.** Outside the editor the garbage collector frees anything nothing references, on every map
  change. Pass `h7.flags.RootSet` (never freed) or make sure game data the game keeps references the copy.
- **Lazily loaded references.** Many Heroes VII references (creature and hero models `mVisuals`, icons, town
  screens, `H7Faction.mStartTown`...) are `DynLoadObjectProperty` fields that the game loads from the object's
  package when first needed and releases again with `Del*` functions. A copy has no package, so it can never
  load them: call `source:DynLoadObjectProperty("mVisuals")` (or `DynLoadObjectArrayProperty` for arrays) on the
  source **before** cloning, and stop the game's `Del*` functions for your copies (and their sources) with a
  pre-hook. A creature without its model crashes the game when a battle starts.
- **Names and texts** are localised by object name, which a copy doesn't share with its source: the game shows
  the copy's fallback text (usually English). Set the cached texts yourself, e.g. `mNameInst` on units, or
  hook the `GetName` of the class.
- **Save games** store references by object path (`MyMod.A_CreatureFrostSprite`), so create your copies with
  the same names before a save is loaded (at the main menu), and never rename them.

## Hooks

A hook runs your Lua function whenever a game function is called.

```lua
-- Before the function runs. Return true to block it.
local id = h7.hook("Package.Class.Function", function(self, args)
end)

-- After the function has run. Return a value to replace its return value.
local id = h7.hook_post("Package.Class.Function", function(self, args, result)
end)

h7.unhook(id)
```

`h7.hook(func, fn, "post")` is the same as `h7.hook_post(func, fn)`. The function is named
`Package.Class.Function` or passed as a function object.

**Parameters.**
- `self` is the object the function was called on.
- `args` gives access to the function's parameters by name: `args.Result`, `args.resultIdx`. You can also assign
  them (`args.Amount = 10` in a pre-hook changes what the function receives). `out` parameters can be read and
  written the same way.
- In a post-hook, `args` shows the values after the function ran, and `result` is its return value (absent for
  functions that return nothing).
- `args` and any struct or array read from it are only valid while your hook runs. Copy what you need
  (`h7.to_table`) if you want to keep it.

**Blocking.** A pre-hook that returns `true` stops the original function from running. The caller then gets a
zero return value (`0`, `false`, `""`, `nil`). Post-hooks still run.

**Changing the result.** A post-hook that returns a non-`nil` value replaces the function's return value.
Returning nothing leaves it unchanged.

```lua
-- CombatXP: 50% more experience from combat
h7.hook_post("MMH7Game.H7CombatArmy.GetKilledCreaturesXP", function(army, args, xp)
    if xp > 0 then return math.floor(xp * 1.5 + 0.5) end
end)

-- DamageScale: read an argument and call methods on it
h7.hook_post("MMH7Game.H7GameProcessor.CalculateDamage", function(processor, args)
    local result, idx = args.Result, args.resultIdx
    if not result or result:IsHeal(idx) then return end
    result:SetDamage(math.floor(result:GetDamage(idx) * 1.25 + 0.5), idx)
end)
```

**What a hook sees.**
- Hooks on **UnrealScript functions** see every call, whether it comes from other UnrealScript or from the
  engine's C++ code.
- A hook applies to the function exactly as named. If a subclass overrides the function, calls on subclass
  objects run the override, so hook the override too (`H7CreatureStack.ApplyDamage` is separate from
  `H7Unit.ApplyDamage`).
- **Native functions** (implemented in C++, `native` in `h7.signature`) can be hooked, but `args` is `nil`
  and they can't be blocked.
- Latent and iterator functions (`Sleep`, `foreach` iterators) only report calls made from C++.

**Several hooks** on one function (from one or more mods) all run, in the order they were added.

**Speed.** Some functions run hundreds of times a second (for example during combat). Keep hooks on them cheap:
check the cheapest condition first, and cache answers per object, as the HeroMovement example does:

```lua
local is_local = {}
h7.on("map_loaded", function() is_local = {} end)

h7.hook_post("MMH7Game.H7EditorHero.GetMovementPoints", function(hero, args, points)
    local key = hero:get_address()
    if is_local[key] == nil then
        local player = hero:GetPlayer()
        is_local[key] = player ~= nil and player:IsControlledByLocalPlayer()
    end
    if is_local[key] then return math.floor(points * 1.5 + 0.5) end
end)
```

## Events and timers

```lua
local id = h7.on("map_loaded", function(map) ... end)
h7.off(id)
```

| Event | Arguments | When |
|---|---|---|
| `tick` | `dt` (seconds since the last frame) | Every frame |
| `map_loaded` | map name (lowercase), e.g. `h7logoentry`, `councilhub` (main menu), `sce_sephinroth` | When a different map is active. Battles don't trigger it |
| `mods_loaded` | number of mods | After all mods have loaded (also after `h7.reload()`) |
| `mod_toggled` | mod name, `enabled` | After a mod was loaded or unloaded by `h7.set_mod_enabled` (the Mods menu) |
| `setting_changed` | mod name, key, new value | After a mod setting changed (Mods menu, `mod.settings`, `h7.set_mod_setting`) |

Timers run on game time:

```lua
local id = h7.after(5, function() h7.log("5 seconds later") end)   -- once
local id = h7.every(60, function() h7.log("every minute") end)     -- repeating
h7.cancel(id)
```

Events, timers and the hot-reload check run on the game's frame. **They pause while the game window is in the
background**, because the game stops updating then.

## Saving data

```lua
local data = mod.load()            -- {} the first time
data.launches = (data.launches or 0) + 1
mod.save(data)
```

`mod.save(table)` writes `<Game>\H7ModEngine\data\<ModName>.lua`; `mod.load()` reads it back. Numbers,
strings, booleans and nested tables are saved; anything else (functions, engine objects) is dropped. The data
belongs to the mod, not to a savegame.

## Logging and debugging

| Function | Writes to `logs\engine.log` |
|---|---|
| `h7.log(...)`, `print(...)` | `[MyMod] message` |
| `h7.warn(...)` | `WARNING [MyMod] message` |
| `h7.error(...)` | `ERROR [MyMod] message` |

Hooks, event handlers and timers run as the mod that registered them: their log lines carry its name, and any
hook, handler or timer they create belongs to that mod too (and is removed when it unloads).

Errors inside hooks, event handlers and timers are caught and logged as `ERROR in hook ... (MyMod)` with a stack
trace; the game keeps running and the hook stays active.

**The exec folder** runs Lua in the running game. Copy a `.lua` file into `<Game>\H7ModEngine\exec\`. Within
about a second it runs, its output and return values go to the log (`exec: => ...`), and the file is renamed to
`.done`. Exec scripts share the global environment, not a mod's.

```lua
-- exec\check.lua
local hero = h7.first("H7AdventureHero")
return hero, hero:GetMovementPoints()
```

`h7.exec("command")` runs a console command through the local player controller and returns its output.

## Configuration

`<Game>\H7ModEngine\H7ModEngine.ini`:

```ini
[Mods]
; Comma-separated mod names, written by the in-game Mods menu. They override the manifests' enabled field.
Enabled=DailyGold
Disabled=HelloWorld

; Mod settings, written by the Mods menu (one section per mod; see Settings)
[Mod.DamageScale]
multiplier=1.5

[Dev]
; Write names.txt / objects.txt to dumps\ at startup (default 1)
DumpSDK=1
; Engine development only (default 0): raw layout snapshots used for engine calibration
DumpProbe=0
; Engine development only (default 0): the unpacked game executable (~100 MB)
DumpImage=0
```

## Reference

**h7**

| | |
|---|---|
| `h7.version` | Engine version string |
| `h7.log/warn/error(...)` | [Logging](#logging-and-debugging) |
| `h7.first`, `h7.find_all`, `h7.find`, `h7.find_class`, `h7.enum`, `h7.properties`, `h7.functions`, `h7.signature` | [Looking things up](#looking-things-up) |
| `h7.to_table(value)` | Plain Lua copy of a struct, array or object |
| `h7.clone`, `h7.new_object`, `h7.package`, `h7.name`, `h7.can_create`, `h7.flags` | [Creating objects](#creating-objects) |
| `h7.hook(func, fn [, "pre" \| "post"])`, `h7.hook_post(func, fn)`, `h7.unhook(id)` | [Hooks](#hooks) |
| `h7.on(event, fn)`, `h7.off(id)` | [Events](#events-and-timers) |
| `h7.after(seconds, fn)`, `h7.every(seconds, fn)`, `h7.cancel(id)` | [Timers](#events-and-timers) |
| `h7.mods()` | `{name, version, author, description, category, enabled, loaded, hidden, settings}` for every installed mod (`settings`: how many it declares) (`enabled` but not `loaded`: it failed to load, see the log) |
| `h7.set_mod_enabled(name, on)` | Turn a mod on or off; saved in the ini, applied on the next frame |
| `h7.mod_settings(name)`, `h7.set_mod_setting(name, key, value)` | A mod's settings with their values; change one (see Settings) |
| `h7.reload()` | Unload and reload all mods (on the next frame) |
| `h7.exec(command)` | Run a console command |

**Per mod**: `mod.name`, `mod.version`, `mod.author`, `mod.dir`, `mod.require(name)`, `mod.save(table)`,
`mod.load()`, and an optional global `function on_unload()`.

**Example mods** (in `mods\`; all except HelloWorld ship disabled; turn them on in the Mods menu):

| Mod | Shows |
|---|---|
| ModManager | The main menu's Mods entry, the mod list and the settings controls: adding Flash clips to a game window and reading mouse input |
| HelloWorld | Events, timers, saved data, `on_unload` |
| DailyGold | Post-hook on a game function; finding objects; calling functions |
| CombatXP | Replacing a return value |
| DamageScale | Reading hook arguments (including an `out` parameter) and calling methods on them |
| HeroMovement | Filtering by owner; caching per object in a hot hook |

## Limitations

- Built for Heroes VII **v2.2.1** (64-bit). Other versions may not work.
- Mods change the local game only. **Multiplayer is untested** and will likely desync when a mod changes
  gameplay.
- Native functions: hooks get no arguments and can't block.
- New mod folders need `h7.reload()` or a restart.
- Flash objects (`GFxObject`) returned by `GetObject`/`AttachMovie` are freed by the engine's garbage
  collector when no UnrealScript object holds them. Keep the clip's name and call `GetObject` again when you
  need it, instead of storing the object.
- `GetObject` only reaches display objects (named clips, buttons, labels) and objects created from Unreal
  (`CreateObject`/`CreateArray`); members holding arrays or objects created by the movie's own ActionScript
  return `nil`.
- The game's Flash menus cache every translation they ask for (`H7FlashMovieCntl.FlashLocalize`) for the rest of
  the session, so changing a translation through a hook also changes it everywhere else that key is used.
- Mods run on the game thread: a slow hook slows the game down.
- In the Mods menu the selection goes back to the first row after a mod is enabled or disabled (the list is
  rebuilt; its rows can't be selected from Lua).
