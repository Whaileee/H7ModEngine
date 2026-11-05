# Mods, manifest and settings

## Mod folder

```
<Game>\H7ModEngine\mods\
  MyMod\
    mod.lua      -- manifest (optional)
    main.lua     -- runs when the mod loads (required)
    util.lua     -- extra files, loaded with mod.require("util")
```

A folder without `main.lua` is listed but never loads (`Mod X has no main.lua; skipped` in the log). A folder
whose `mod.lua` raises an error is skipped entirely.

## Manifest

`mod.lua` returns a table. Every field is optional.

```lua
return {
    name = "MyMod",
    version = "1.0",
    author = "You",
    description = "What the mod does.",
    category = "Combat",
    load_after = { "OtherMod" },
    enabled = true,
    hidden = false,
    settings = { ... },
}
```

| Field | Type | Default | |
|---|---|---|---|
| `name` | string | folder name | The mod's name: in the log, the Mods menu, `H7ModEngine.ini`, `data\<name>.lua` and every `h7.*` function that takes a mod name. Keep it stable |
| `version` | string | `""` | Shown in the Mods menu and the log |
| `author` | string | `""` | Shown in the Mods menu and the log |
| `description` | string | `""` | Shown in the Mods menu |
| `category` | string | `""` | Shown, and filterable, in the Mods menu |
| `load_after` | `{ string, ... }` | `{}` | Mods (by `name`) this one must load after, when they are installed |
| `enabled` | boolean | `true` | `false`: off until the player turns it on. The player's choice (stored in the ini) overrides this |
| `hidden` | boolean | `false` | `true`: not listed in the Mods menu (libraries, the ModManager itself) |
| `settings` | `{ setting, ... }` | `{}` | Values players change in the Mods menu, see [Settings](#settings) |

The manifest runs once when mods are discovered (at startup and on [`h7.reload()`](h7.md#h7reload)), in the shared
environment, so keep it to a plain `return { ... }`.

**Load order.** Mods load alphabetically by name (case-insensitive), except that each mod is moved after every
installed mod it lists in `load_after`. A cycle is logged as `load_after cycle among remaining mods` and the mods
in it load alphabetically. After all enabled mods have loaded, [`mods_loaded`](events.md#mods_loaded) fires.

## Settings

Each entry of the manifest's `settings` list declares one value players can change in the Mods menu:

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

| Field | | |
|---|---|---|
| `key` | string, **required** | Name used in code (`mod.settings.key`) and in the ini. Entries without a key are ignored |
| `label` | string | Shown in the Mods menu. Default: `key` |
| `description` | string | Shown in the Mods menu |
| `type` | `"bool"` \| `"number"` \| `"choice"` \| `"text"` | Default: inferred, `bool` for a boolean `default`, `number` for a number, `choice` when `options` is given, otherwise `text` |
| `default` | value | Value until the player changes it. Missing or invalid: `false`, `min` (or `0`), the first option, or `""` |
| `min`, `max` | number | Numbers: allowed range (values are clamped) |
| `step` | number | Numbers: values are rounded to a multiple of `step`, counted from `min` (or `0`) |
| `options` | `{ value, ... }` | Choices: the allowed values, in the order the menu cycles through them. Required for `choice` |
| `control` | `"slider"` \| `"input"` \| `"arrows"` | Numbers: how the menu shows it. Default: `slider` with both `min` and `max`, otherwise `input` |
| `reload` | boolean | `true`: the mod restarts when the value changes (for values only read at load time) |

Invalid declarations (unknown `type`, a `choice` without `options`) are logged and skipped.

**Values.** Booleans come back as `true`/`false`, numbers as integers when whole, choices and text as strings.
Choices compare case-insensitively and are stored with the spelling of `options`. Booleans also accept
`"true"/"false"`, `"yes"/"no"`, `"on"/"off"`, `1`/`0`. Values are stored in `H7ModEngine.ini`, section
`[Mod.<name>]`; an invalid value there is logged and replaced by the default.

**In the Mods menu** numbers get a slider, a number box (Enter saves, Escape cancels) or arrows, booleans a check
box, and choices arrows. Text settings are shown but can only be changed in code or in the ini. The panel shows up
to five settings; further ones are listed as being in the ini.

## The mod table

Every mod has a global `mod` table describing itself:

| Field | |
|---|---|
| `mod.name` | The mod's name (manifest `name` or folder name) |
| `mod.version` | Manifest `version` (or `""`) |
| `mod.author` | Manifest `author` (or `""`) |
| `mod.dir` | Full path of the mod folder, e.g. `C:\...\H7ModEngine\mods\MyMod` |
| `mod.require(name)` | See [below](#modrequire) |
| `mod.save(table)`, `mod.load()` | See [below](#modsave) |
| `mod.settings` | See [below](#modsettings) |

### mod.require

```lua
mod.require(name: string) → any
```

Runs `<mod folder>\<name>.lua` in the mod's environment (it sees and sets the same globals as `main.lua`) and
returns what the file returns, or `true` if it returns nothing. The file runs once per load of the mod; later calls
return the cached result. `name` is a path relative to the mod folder without `.lua`: `"util"`,
`"lib/strings"`.

**Errors:** the file is missing or has a syntax error; any error it raises.

```lua
-- util.lua
local M = {}
function M.round(x) return math.floor(x + 0.5) end
return M

-- main.lua
local util = mod.require("util")
```

### mod.save

```lua
mod.save(data: table)
```

Writes `table` to `<Game>\H7ModEngine\data\<mod name>.lua`, replacing what was saved before. Numbers, strings,
booleans and nested tables are kept; other values (functions, engine objects) become `nil`. Keys must be strings,
numbers or booleans. The data belongs to the mod, not to a save game.

**Errors:** `table too deep to save` (more than 32 levels, or a table that contains itself); `could not write save
data`.

### mod.load

```lua
mod.load() → table
```

The table last saved with [`mod.save`](#modsave), or an empty table if there is none. The data file is read
without access to any functions.

```lua
local data = mod.load()
data.launches = (data.launches or 0) + 1
mod.save(data)
```

### mod.settings

```lua
mod.settings.key → value
mod.settings.key = value
pairs(mod.settings)
```

The mod's [settings](#settings). Reading gives the current value (`nil` for an unknown key); read it when you need
it rather than copying it at load time, since players can change it at any time. Assigning works like
[`h7.set_mod_setting`](h7.md#h7set_mod_setting): the value is validated and saved and
[`setting_changed`](events.md#setting_changed) fires. `pairs(mod.settings)` iterates `key, value`.

**Errors (assigning):** `mod <name> has no setting '<key>'`; `invalid value for <name>.<key> (<type>)`.

```lua
h7.hook_post("MMH7Game.H7CombatArmy.GetKilledCreaturesXP", function(army, args, xp)
    return math.floor(xp * mod.settings.multiplier + 0.5)
end)
```

## Globals

Each mod has its own global environment. Globals a mod assigns stay inside it; reading a global it didn't assign
falls through to the shared environment (`h7`, `string`, `math`, ...). Two mods can both define the same global
names without clashing. `_G` still refers to the shared environment.

### on_unload

```lua
function on_unload() ... end
```

If a mod defines a global function `on_unload`, it is called before the mod unloads (when it's disabled, hot
reloaded, or on [`h7.reload()`](h7.md#h7reload)). Use it to undo changes the game won't undo by itself, such as
property values you changed. Hooks, event handlers and timers the mod registered are removed after it returns;
you don't need to remove them yourself.

## Loading, hot reload and errors

- Loading runs `main.lua` in the mod's environment. An error in it is logged
  (`ERROR in MyMod/main.lua: ...`) with a stack trace; whatever the mod registered before the error stays active.
- About once a second, the engine checks each loaded mod's folder (including subfolders) for changed `.lua`
  files. When one changed, the mod is unloaded and loaded again. Fixing an error and saving the file therefore
  retries the mod.
- New mod folders and mods turned on in the ini need [`h7.reload()`](h7.md#h7reload) or a restart; mods turned on
  in the Mods menu or with [`h7.set_mod_enabled`](h7.md#h7set_mod_enabled) load on the next frame.
- Errors in hooks, event handlers and timers are logged with the mod's name and a stack trace; the hook, handler
  or timer stays registered.

## Shared libraries

The standard `require(name)` loads `<Game>\H7ModEngine\lib\<name>.lua`; as usual in Lua, dots in `name` are
folders (`require("ui.list")` loads `lib\ui\list.lua`). The library runs once, in the shared environment, and its result is shared by every mod. Use it for code
several mods share; use [`mod.require`](#modrequire) for a mod's own files.
