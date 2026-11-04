# Events and timers

Events and timers run on the game thread, once per rendered frame. **They pause while the game window is in the
background**, because the game stops updating then.

Each frame the engine, in this order: checks for changed mod files and map changes and runs the `exec` folder
(about once a second), applies mods turned on or off, reloads mods whose `reload` setting changed, handles
[`h7.reload()`](h7.md#h7reload), runs due timers, and fires [`tick`](#tick).

## Events

Handle an event with [`h7.on(event, fn)`](h7.md#h7on); `fn` receives the arguments below. Remove the handler
with [`h7.off(id)`](h7.md#h7off). Handlers run in the order they were added; an error in one is logged and the
others still run.

### tick

```lua
h7.on("tick", function(dt) end)
```

| Argument | |
|---|---|
| `dt` | Seconds since the previous frame (number) |

Fires every frame, after timers. Keep handlers cheap; for anything periodic use [`h7.every`](h7.md#h7every).

### map_loaded

```lua
h7.on("map_loaded", function(map) end)
```

| Argument | |
|---|---|
| `map` | Name of the map now active, lowercase: `h7logoentry` (intro), `councilhub` (main menu), `sce_sephinroth`, ... |

Fires when a different map becomes active (checked about once a second), including the first map after startup.
Battles don't change the map and don't fire it. Objects of the previous map are gone: drop references and caches
you kept.

### mods_loaded

```lua
h7.on("mods_loaded", function(count) end)
```

| Argument | |
|---|---|
| `count` | Number of mods loaded |

Fires after every enabled mod has loaded, at startup and after [`h7.reload()`](h7.md#h7reload). Use it for work
that needs other mods to be ready.

### mod_toggled

```lua
h7.on("mod_toggled", function(name, enabled) end)
```

| Argument | |
|---|---|
| `name` | The mod's name |
| `enabled` | `true` if it was loaded, `false` if it was unloaded |

Fires after a mod was loaded or unloaded because of [`h7.set_mod_enabled`](h7.md#h7set_mod_enabled) (the Mods
menu). A mod that was just unloaded doesn't receive it.

### setting_changed

```lua
h7.on("setting_changed", function(name, key, value) end)
```

| Argument | |
|---|---|
| `name` | The mod whose setting changed |
| `key` | The setting's key |
| `value` | The new value |

Fires when a [setting](mod.md#settings) gets a different value: from the Mods menu,
[`mod.settings.key = v`](mod.md#modsettings) or [`h7.set_mod_setting`](h7.md#h7set_mod_setting). It fires for
every mod's settings, so check `name`:

```lua
h7.on("setting_changed", function(name, key, value)
    if name == mod.name and key == "gold" then h7.log("Gold per day is now " .. value) end
end)
```

## Timers

| Function | |
|---|---|
| [`h7.after(seconds, fn)`](h7.md#h7after) | Run `fn()` once after `seconds` |
| [`h7.every(seconds, fn)`](h7.md#h7every) | Run `fn()` every `seconds` |
| [`h7.cancel(id)`](h7.md#h7cancel) | Cancel a timer |

Timers count **game time**: the sum of frame times since the engine started, which stops while the window is in
the background. A timer runs on the first frame at or after its due time, so its precision is one frame.

A repeating timer is rescheduled `seconds` after the frame it ran on: it doesn't catch up on missed runs and runs
at most once per frame. `fn` receives no arguments. Errors in it are logged and a repeating timer keeps running.

Timers are removed when the mod that created them unloads.
