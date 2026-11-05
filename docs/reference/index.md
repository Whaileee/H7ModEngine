# API reference

The complete Lua API of H7ModEngine. For an introduction with worked examples, read the
[modding guide](../API.md) first.

Every mod runs in a Lua 5.4 state shared by all mods, with the standard libraries (`string`, `table`, `math`,
`io`, `os`, `utf8`, `coroutine`, `debug`, `package`) and these additions:

| Name | What it is | Reference |
|---|---|---|
| `h7` | The engine library: finding objects, hooks, events, timers, mods, creating objects, logging | [h7 library](h7.md) |
| `mod` | The current mod: its name, folder, settings, saved data and `require` | [Mods](mod.md#the-mod-table) |
| `print(...)` | Same as `h7.log(...)` | [h7.log](h7.md#h7log) |
| `require(name)` | Standard `require`, searching `<Game>\H7ModEngine\lib\?.lua` | [Mods](mod.md#shared-libraries) |
| `on_unload()` | A global function a mod may define; called before the mod unloads | [Mods](mod.md#on_unload) |

Values coming from the engine are one of three reference types, plus plain Lua values:

| Type | What it is | Reference |
|---|---|---|
| Object reference | A live engine object (actor, hero, class, function, ...) | [Engine objects](objects.md#object-references) |
| Struct reference | A live view of a struct inside an object, a struct or hook arguments | [Structs](objects.md#struct-references) |
| Array reference | A live view of an UnrealScript `array<T>` | [Arrays](objects.md#array-references) |

How every UnrealScript type is converted is in [Values and types](types.md). The game's own classes, whose
properties and functions mods use through these references, are listed in [Game classes](../sdk/index.md).

## Index

### h7 library

| Function | Summary |
|---|---|
| [`h7.version`](h7.md#h7version) | Engine version string |
| **Logging** | |
| [`h7.log(...)`](h7.md#h7log) | Write a line to `engine.log` |
| [`h7.warn(...)`](h7.md#h7warn) | Write a `WARNING` line |
| [`h7.error(...)`](h7.md#h7error) | Write an `ERROR` line (does not raise) |
| [`h7.log_lines([after])`](h7.md#h7log_lines) | Recent log lines (the last 2000), e.g. to show the log in game |
| **Finding objects** | |
| [`h7.first(class)`](h7.md#h7first) | First live instance of a class |
| [`h7.find_all(class [, include_templates])`](h7.md#h7find_all) | All live instances of a class |
| [`h7.find(path)`](h7.md#h7find) | An object by path |
| [`h7.find_class(name)`](h7.md#h7find_class) | A class by name |
| [`h7.enum(name)`](h7.md#h7enum) | An enum's values by name |
| **Reflection** | |
| [`h7.properties(x)`](h7.md#h7properties) | Properties of a class, struct or function |
| [`h7.functions(x)`](h7.md#h7functions) | Functions of a class |
| [`h7.signature(func)`](h7.md#h7signature) | A function's declaration as text |
| [`h7.to_table(value)`](h7.md#h7to_table) | Plain Lua copy of a struct, array or object |
| **Hooks** | |
| [`h7.hook(func, fn [, when])`](h7.md#h7hook) | Run `fn` before (or after) a game function |
| [`h7.hook_post(func, fn)`](h7.md#h7hook_post) | Run `fn` after a game function |
| [`h7.unhook(id)`](h7.md#h7unhook) | Remove a hook |
| **Events and timers** | |
| [`h7.on(event, fn)`](h7.md#h7on) | Handle an event |
| [`h7.off(id)`](h7.md#h7off) | Remove an event handler |
| [`h7.after(seconds, fn)`](h7.md#h7after) | Run `fn` once after a delay |
| [`h7.every(seconds, fn)`](h7.md#h7every) | Run `fn` repeatedly |
| [`h7.cancel(id)`](h7.md#h7cancel) | Cancel a timer |
| **Creating objects** | |
| [`h7.clone(source, name [, outer [, flags]])`](h7.md#h7clone) | Copy an object or archetype |
| [`h7.new_object(class [, options])`](h7.md#h7new_object) | Create a new object |
| [`h7.package(name)`](h7.md#h7package) | Find or create a top-level package |
| [`h7.name(str)`](h7.md#h7name) | Add a name to the engine's name table |
| [`h7.can_create()`](h7.md#h7can_create) | Whether object creation is available |
| [`h7.flags`](h7.md#h7flags) | Object flag constants |
| **Mods** | |
| [`h7.mods()`](h7.md#h7mods) | Every installed mod |
| [`h7.set_mod_enabled(name, on)`](h7.md#h7set_mod_enabled) | Turn a mod on or off |
| [`h7.mod_settings(name)`](h7.md#h7mod_settings) | A mod's settings and their values |
| [`h7.set_mod_setting(name, key, value)`](h7.md#h7set_mod_setting) | Change a mod's setting |
| [`h7.reload()`](h7.md#h7reload) | Reload every mod |
| **Console** | |
| [`h7.exec(command)`](h7.md#h7exec) | Run a console command |
| [`h7.config(section, key [, default])`](h7.md#h7config) | Read a value from `H7ModEngine.ini` |
| [`h7.clipboard([text])`](h7.md#h7clipboard) | Read or set the Windows clipboard's text |

### Object methods

| Method | Summary |
|---|---|
| [`obj.Property`, `obj.Property = v`](objects.md#properties) | Read or write a property |
| [`obj:Function(...)`](objects.md#calling-functions) | Call an UnrealScript function |
| [`obj:call(name, ...)`](objects.md#objcall) | Call a function by name |
| [`obj:get_name()`](objects.md#objget_name) | Object name |
| [`obj:get_full_name()`](objects.md#objget_full_name) | Class and path |
| [`obj:get_path_name()`](objects.md#objget_path_name) | Path |
| [`obj:get_class()`](objects.md#objget_class) | Class object |
| [`obj:get_outer()`](objects.md#objget_outer) | Containing object |
| [`obj:get_default()`](objects.md#objget_default) | A class's default object |
| [`obj:get_super()`](objects.md#objget_super) | Parent class, struct or function |
| [`obj:get_address()`](objects.md#objget_address) | Memory address |
| [`obj:is_valid()`](objects.md#objis_valid) | Whether the object still exists |
| [`obj:is_a(class)`](objects.md#objis_a) | Class check |
| [`arr:add([value])`](objects.md#arradd) | Append to an array |
| [`arr:remove(i)`](objects.md#arrremove) | Remove an array element |

### Per mod

| Name | Summary |
|---|---|
| [`mod.name`, `mod.version`, `mod.author`, `mod.dir`](mod.md#the-mod-table) | Mod information |
| [`mod.require(name)`](mod.md#modrequire) | Load a Lua file from the mod's folder |
| [`mod.save(table)`](mod.md#modsave) / [`mod.load()`](mod.md#modload) | Persistent data |
| [`mod.settings`](mod.md#modsettings) | The mod's settings |
| [`on_unload()`](mod.md#on_unload) | Cleanup before unloading |
| [Manifest (`mod.lua`)](mod.md#manifest) | Name, version, load order, settings, ... |

### Events

| Event | Arguments |
|---|---|
| [`tick`](events.md#tick) | `dt` |
| [`map_loaded`](events.md#map_loaded) | `map` |
| [`mods_loaded`](events.md#mods_loaded) | `count` |
| [`mod_toggled`](events.md#mod_toggled) | `name`, `enabled` |
| [`setting_changed`](events.md#setting_changed) | `name`, `key`, `value` |

## Conventions in this reference

- Signatures are written `h7.find_all(class [, include_templates]) → table`: square brackets mark optional
  parameters, `→` what the function returns, `|` alternatives.
- **class** parameters take a class name (`"H7AdventureHero"` or `"MMH7Game.H7AdventureHero"`) or a class object.
- **func** parameters take a function path (`"MMH7Game.H7Calendar.NextDay"`, optionally prefixed with
  `"Function "`) or a function object.
- **Errors** are raised with Lua's `error`, so a mistake stops the current chunk, hook, handler or timer and is
  logged with a stack trace (see [Errors](../API.md#logging-and-debugging)); use `pcall` to handle one yourself.
- Every hook, event handler and timer belongs to the mod whose code created it and is removed when that mod
  unloads. Ones created from an `exec` script belong to no mod: nothing removes them automatically.
