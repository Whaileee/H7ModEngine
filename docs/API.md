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
    enabled = true,              -- false: the mod is skipped
}
```

**Load order.** Mods load alphabetically by name, except that a mod is moved after every mod it lists in
`load_after`. After all mods have loaded, the `mods_loaded` event fires.

**Disabling.** Set `enabled = false` in the manifest, or list the mod in `H7ModEngine.ini`
(see [Configuration](#configuration)).

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
| `name` | string (writing a name requires it to exist in the engine already) |
| object, class, interface | object reference, or `nil` for `none` |
| struct | live struct reference: `v.X` reads and `v.X = 1` writes the field in place |
| `array<T>` | live array reference (see below) |
| fixed array (`int X[4]`) | a Lua table copy; assign a table to write it |
| delegate | `{object = ..., func = "Name"}` (read only) |

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
; Comma-separated mod names to skip
Disabled=DamageScale, HeroMovement

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
| `h7.hook(func, fn [, "pre" \| "post"])`, `h7.hook_post(func, fn)`, `h7.unhook(id)` | [Hooks](#hooks) |
| `h7.on(event, fn)`, `h7.off(id)` | [Events](#events-and-timers) |
| `h7.after(seconds, fn)`, `h7.every(seconds, fn)`, `h7.cancel(id)` | [Timers](#events-and-timers) |
| `h7.mods()` | `{name, version, author, loaded}` for every mod |
| `h7.reload()` | Unload and reload all mods (on the next frame) |
| `h7.exec(command)` | Run a console command |

**Per mod**: `mod.name`, `mod.version`, `mod.author`, `mod.dir`, `mod.require(name)`, `mod.save(table)`,
`mod.load()`, and an optional global `function on_unload()`.

**Example mods** (in `mods\`; all except HelloWorld ship disabled; set `enabled = true` in their `mod.lua`):

| Mod | Shows |
|---|---|
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
- New mod folders and newly enabled mods need `h7.reload()` or a restart.
- Mods run on the game thread: a slow hook slows the game down.
